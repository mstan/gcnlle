#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic GameCube differential co-simulation coordinator.

The runtime side speaks JSON-over-newline on GCN_DEBUG_PORT.  Dolphin's
independent PPC oracle uses its built-in GDB remote stub.  This tool follows
recomp-template/DIFFERENTIAL-COSIMULATION.md: park both machines at guest
boundaries, compare state, validate A-vs-A first, and prove fault detection
before trusting A-vs-B.

The initial runtime state surface is deliberately partial (CPU + MEM1 + MEM2).
`cosim_state.complete` stays false until canonical device snapshots land.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import Any


class ProtocolError(RuntimeError):
    pass


class JsonTcp:
    def __init__(self, port: int, timeout: float = 10.0):
        self.port = port
        self.timeout = timeout

    def request(self, cmd: str, **params: Any) -> dict[str, Any]:
        request = {"id": 1, "cmd": cmd, **params}
        with socket.create_connection(("127.0.0.1", self.port), self.timeout) as sock:
            sock.settimeout(self.timeout)
            sock.sendall((json.dumps(request, separators=(",", ":")) + "\n").encode())
            data = bytearray()
            while b"\n" not in data:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data.extend(chunk)
        if b"\n" not in data:
            raise ProtocolError(f"port {self.port}: truncated response to {cmd!r}")
        response = json.loads(bytes(data).split(b"\n", 1)[0])
        if response.get("ok") is not True:
            raise ProtocolError(f"port {self.port}: {cmd}: {response}")
        return response


class GdbRemote:
    """Small client for Dolphin's built-in PowerPC GDB remote stub."""

    def __init__(self, port: int, timeout: float = 10.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout)
        self.sock.settimeout(timeout)
        self._rx = bytearray()

    def close(self) -> None:
        self.sock.close()

    @staticmethod
    def _packet(payload: str) -> bytes:
        raw = payload.encode("ascii")
        checksum = sum(raw) & 0xFF
        return b"$" + raw + f"#{checksum:02x}".encode("ascii")

    def _read_exact(self, count: int) -> bytes:
        while len(self._rx) < count:
            # GDB memory packets are several KiB of hex. _read_packet consumes
            # them one protocol byte at a time, so retain a large socket read
            # here instead of issuing one recv syscall per character.
            chunk = self.sock.recv(max(65536, count - len(self._rx)))
            if not chunk:
                raise ProtocolError("Dolphin GDB socket closed")
            self._rx.extend(chunk)
        data = bytes(self._rx[:count])
        del self._rx[:count]
        return data

    def _read_packet_body(self) -> str:
        data = bytearray()
        while True:
            ch = self._read_exact(1)
            if ch == b"#":
                break
            data.extend(ch)
        received = int(self._read_exact(2), 16)
        calculated = sum(data) & 0xFF
        if received != calculated:
            self.sock.sendall(b"-")
            raise ProtocolError(
                f"Dolphin GDB checksum mismatch: {received:02x} != {calculated:02x}"
            )
        self.sock.sendall(b"+")
        return data.decode("ascii")

    def _read_packet(self) -> str:
        while self._read_exact(1) != b"$":
            pass
        return self._read_packet_body()

    def request(self, payload: str) -> str:
        packet = self._packet(payload)
        self.sock.sendall(packet)
        ack = self._read_exact(1)
        # Dolphin's raw Ctrl-C path can publish a duplicate stop after the
        # normal drain window. If that late packet arrives where this request's
        # ACK belongs, acknowledge it, then keep waiting: the request is already
        # queued in Dolphin's socket and must not be transmitted a second time.
        while ack == b"$":
            unsolicited = self._read_packet_body()
            if not unsolicited.startswith("T"):
                raise ProtocolError(
                    f"unexpected unsolicited Dolphin packet: {unsolicited!r}"
                )
            ack = self._read_exact(1)
        if ack != b"+":
            raise ProtocolError(f"Dolphin GDB rejected {payload!r}: {ack!r}")
        return self._read_packet()

    def register(self, register_id: int) -> int:
        reply = self.request(f"p{register_id:x}")
        if len(reply) == 3 and reply.startswith("E"):
            raise ProtocolError(f"Dolphin register {register_id} unavailable: {reply}")
        return int(reply, 16)

    def registers(self) -> dict[str, Any]:
        packed_gprs = self.request("g")
        if len(packed_gprs) < 32 * 8:
            raise ProtocolError("Dolphin returned a short packed GPR set")
        return {
            "gpr": [
                int(packed_gprs[i * 8:(i + 1) * 8], 16)
                for i in range(32)
            ],
            "pc": self.register(64),
            "msr": self.register(65),
            "cr": self.register(66),
            "lr": self.register(67),
            "ctr": self.register(68),
            "xer": self.register(69),
            "fpscr": self.register(70),
            "sr": [self.register(i) for i in range(71, 87)],
            "srr0": self.register(112),
            "srr1": self.register(113),
            "tbl": self.register(114),
            "tbu": self.register(115),
            "dec": self.register(116),
            "dar": self.register(106),
            "dsisr": self.register(107),
        }

    def memory(self, address: int, length: int) -> bytes:
        reply = self.request(f"m{address:x},{length:x}")
        # A successful binary-as-hex reply can legitimately begin with byte
        # 0xE0..0xEF. GDB errors are exactly E followed by a two-digit code.
        if len(reply) == 3 and reply.startswith("E"):
            raise ProtocolError(
                f"Dolphin memory read {address:08x}+{length:x} failed: {reply}"
            )
        return bytes.fromhex(reply)

    def write_memory(self, address: int, data: bytes) -> None:
        reply = self.request(f"M{address:x},{len(data):x}:{data.hex()}")
        if reply != "OK":
            raise ProtocolError(
                f"Dolphin memory write {address:08x}+{len(data):x} failed: {reply}"
            )

    def step(self) -> dict[str, Any]:
        self.step_signal()
        return self.registers()

    def step_signal(self) -> str:
        stop = self.request("s")
        if not stop.startswith("T"):
            raise ProtocolError(f"unexpected Dolphin stop reply: {stop}")
        return stop

    def add_breakpoint(self, address: int) -> None:
        reply = self.request(f"Z0,{address:x},4")
        if reply != "OK":
            raise ProtocolError(
                f"Dolphin rejected breakpoint at 0x{address:08X}: {reply}"
            )

    def remove_breakpoint(self, address: int) -> None:
        reply = self.request(f"z0,{address:x},4")
        if reply != "OK":
            raise ProtocolError(
                f"Dolphin rejected breakpoint removal at 0x{address:08X}: {reply}"
            )

    def continue_signal(self) -> str:
        stop = self.request("c")
        if not stop.startswith("T"):
            raise ProtocolError(f"unexpected Dolphin continue reply: {stop}")
        return stop

    def continue_for(self, seconds: float) -> str:
        """Continue, then send the GDB interrupt byte on this same connection."""
        self.sock.sendall(self._packet("c"))
        ack = self._read_exact(1)
        if ack != b"+":
            raise ProtocolError(f"Dolphin GDB rejected continue: {ack!r}")
        timer = threading.Timer(seconds, self.sock.sendall, args=(b"\x03",))
        timer.daemon = True
        timer.start()
        try:
            stop = self._read_packet()
            # Dolphin's raw Ctrl-C path emits one stop reply directly from
            # ReadCommand and may emit a second when the CPU thread enters its
            # stepping loop. Consume that duplicate now so the next ordinary
            # request does not mistake '$' for its ACK.
            while select.select([self.sock], [], [], 0.25)[0]:
                extra = self._read_packet()
                if extra.startswith("T"):
                    stop = extra
        finally:
            timer.cancel()
        if not stop.startswith("T"):
            raise ProtocolError(f"unexpected Dolphin timed stop reply: {stop}")
        return stop


def wait_json(port: int, process: subprocess.Popen[Any], timeout: float = 30.0) -> JsonTcp:
    deadline = time.monotonic() + timeout
    client = JsonTcp(port, timeout=5.0)
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ProtocolError(f"runtime exited early with code {process.returncode}")
        try:
            client.request("ping")
            return client
        except (OSError, ProtocolError) as exc:
            last_error = exc
            time.sleep(0.05)
    raise ProtocolError(f"runtime port {port} did not become ready: {last_error}")


def wait_parked(client: JsonTcp, instruction: int, timeout: float = 120.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = client.request("cosim_status")
        if status["parked"] and status["instruction"] == instruction:
            return status
        time.sleep(0.002)
    raise ProtocolError(
        f"port {client.port}: did not park at instruction {instruction}"
    )


def wait_parked_pc(
    client: JsonTcp, pc: int, timeout: float = 120.0
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = client.request("cosim_status")
        if status["parked"]:
            if status["pc"] != pc:
                raise ProtocolError(
                    f"port {client.port}: exhausted run-to budget at "
                    f"0x{status['pc']:08X}, wanted 0x{pc:08X}"
                )
            return status
        time.sleep(0.002)
    raise ProtocolError(
        f"port {client.port}: did not park at PC 0x{pc:08X}"
    )


@dataclass
class RuntimeInstance:
    name: str
    process: subprocess.Popen[Any]
    client: JsonTcp
    stdout: Any
    stderr: Any

    def close(self) -> None:
        try:
            if self.process.poll() is None:
                self.client.request("quit")
                self.process.wait(timeout=10)
        except (OSError, ProtocolError, subprocess.TimeoutExpired):
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
        self.stdout.close()
        self.stderr.close()


def start_runtime(args: argparse.Namespace, name: str, port: int) -> RuntimeInstance:
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    stdout = (output_dir / f"{name}.out.log").open("wb")
    stderr = (output_dir / f"{name}.err.log").open("wb")
    env = os.environ.copy()
    env.update(
        {
            "GCN_DISC": str(pathlib.Path(args.disc).resolve()),
            "GCN_DSP_ROM": str(pathlib.Path(args.dsp_rom).resolve()),
            "GCN_DSP_COEF": str(pathlib.Path(args.dsp_coef).resolve()),
            "GCN_DEBUG_PORT": str(port),
            "GCN_COSIM": "1",
            "GCN_CYCLES_DERIVED": "1",
            "GCN_GX_PIPELINE": "0",
            "GCN_GX_THREADS": "1",
            "GCN_WINDOW": "0",
            "GCN_AUDIO": "0",
            "GCN_THROTTLE": "0",
            "GCN_NATIVE_MISS_JOURNAL": str(output_dir / f"{name}-misses.jsonl"),
            "GCN_DI_JOURNAL": str(output_dir / f"{name}-di.jsonl"),
        }
    )
    if args.boot_mode == "bs1":
        env["GCN_BOOT_BS1"] = "1"
    else:
        env.pop("GCN_BOOT_BS1", None)
    process = subprocess.Popen(
        [str(pathlib.Path(args.exe).resolve()), str(pathlib.Path(args.ipl).resolve())],
        cwd=str(pathlib.Path(args.exe).resolve().parent),
        env=env,
        stdout=stdout,
        stderr=stderr,
    )
    client = wait_json(port, process)
    wait_parked(client, 0)
    return RuntimeInstance(name, process, client, stdout, stderr)


def compare_states(
    checkpoint: int, a: dict[str, Any], b: dict[str, Any]
) -> tuple[bool, dict[str, Any]]:
    for response in (a, b):
        if not isinstance(response.get("hash"), str):
            raise ProtocolError("state hash is missing or not a string")
        if not isinstance(response.get("sub"), dict):
            raise ProtocolError("subsystem hashes are missing")
    differing = {
        key: {"a": a["sub"].get(key), "b": b["sub"].get(key)}
        for key in sorted(set(a["sub"]) | set(b["sub"]))
        if a["sub"].get(key) != b["sub"].get(key)
    }
    report = {
        "checkpoint": checkpoint,
        "instruction": {"a": a["instruction"], "b": b["instruction"]},
        "cycles": {"a": a["cycles"], "b": b["cycles"]},
        "pc": {"a": f"0x{a['pc']:08X}", "b": f"0x{b['pc']:08X}"},
        "hash": {"a": a["hash"], "b": b["hash"]},
        "differing_subsystems": differing,
        "complete": bool(a.get("complete") and b.get("complete")),
    }
    return a["hash"] == b["hash"], report


def first_page_difference(a: JsonTcp, b: JsonTcp, space: str) -> dict[str, Any] | None:
    layouts = {
        "mem1": (6144, 0x80000000),
        "mem2": (16384, 0x90000000),
        "l1": (64, 0xE0000000),
    }
    total_pages, guest_base = layouts[space]
    for start in range(0, total_pages, 256):
        pa = a.request("cosim_pages", space=space, start=start, count=256)
        pb = b.request("cosim_pages", space=space, start=start, count=256)
        for index, (ha, hb) in enumerate(zip(pa["hashes"], pb["hashes"])):
            if ha != hb:
                page = start + index
                return {"space": space, "page": page,
                        "address": f"0x{guest_base + page * 4096:08X}",
                        "a": ha, "b": hb}
    return None


def full_byte_audit(
    a: JsonTcp, b: JsonTcp, state_a: dict[str, Any], state_b: dict[str, Any]
) -> dict[str, Any]:
    """Gate 4: compare canonical CPU and every represented memory byte."""
    cpu_a = a.request("cosim_cpu_bytes")
    cpu_b = b.request("cosim_cpu_bytes")
    for cpu_blob, state in ((cpu_a, state_a), (cpu_b, state_b)):
        if cpu_blob["hash"] != state["sub"]["cpu"]:
            raise ProtocolError(
                "canonical CPU byte hash does not match cosim_state CPU sub-hash"
            )
        if len(bytes.fromhex(cpu_blob["hex"])) != cpu_blob["length"]:
            raise ProtocolError("canonical CPU byte response has the wrong length")

    report: dict[str, Any] = {
        "cpu": {
            "equal": cpu_a["hex"] == cpu_b["hex"],
            "length": cpu_a["length"],
            "hash": {"a": cpu_a["hash"], "b": cpu_b["hash"]},
        },
        "memory": {},
    }
    if state_a.get("layout") != state_b.get("layout"):
        raise ProtocolError("A-vs-A instances report different memory layouts")
    layouts = {
        space: (item["base"], item["size"])
        for space, item in state_a["layout"].items()
    }
    all_equal = report["cpu"]["equal"]
    for space, (address, length) in layouts.items():
        sha_a = hashlib.sha256()
        sha_b = hashlib.sha256()
        first_difference = None
        equal = True
        for offset in range(0, length, 65536):
            chunk_len = min(65536, length - offset)
            bytes_a = runtime_memory(a, address + offset, chunk_len)
            bytes_b = runtime_memory(b, address + offset, chunk_len)
            sha_a.update(bytes_a)
            sha_b.update(bytes_b)
            if bytes_a != bytes_b:
                equal = False
                if first_difference is None:
                    index = next(
                        i for i, pair in enumerate(zip(bytes_a, bytes_b))
                        if pair[0] != pair[1]
                    )
                    first_difference = {
                        "address": f"0x{address + offset + index:08X}",
                        "a": bytes_a[index],
                        "b": bytes_b[index],
                    }
        report["memory"][space] = {
            "equal": equal,
            "length": length,
            "sha256": {"a": sha_a.hexdigest(), "b": sha_b.hexdigest()},
            "first_difference": first_difference,
        }
        all_equal = all_equal and equal
    report["equal"] = all_equal
    return report


def cmd_gate1(args: argparse.Namespace) -> int:
    a = start_runtime(args, "gate1-a", args.port)
    b = start_runtime(args, "gate1-b", args.port + 1)
    try:
        for checkpoint in range(args.checkpoints + 1):
            if checkpoint:
                target = checkpoint * args.stride
                a.client.request("cosim_step", count=args.stride)
                b.client.request("cosim_step", count=args.stride)
                wait_parked(a.client, target)
                wait_parked(b.client, target)
            state_a = a.client.request("cosim_state")
            state_b = b.client.request("cosim_state")
            equal, report = compare_states(checkpoint, state_a, state_b)
            print(json.dumps(report, separators=(",", ":")))
            if not equal:
                for space in ("mem1", "mem2", "l1"):
                    if space in report["differing_subsystems"]:
                        print(json.dumps({"first_page": first_page_difference(
                            a.client, b.client, space)}, separators=(",", ":")))
                return 1
            if (
                args.byte_audit_every > 0
                and checkpoint > 0
                and checkpoint % args.byte_audit_every == 0
            ):
                audit = full_byte_audit(
                    a.client, b.client, state_a, state_b
                )
                print(json.dumps({
                    "gate": "hash-vs-byte-audit",
                    "checkpoint": checkpoint,
                    "result": "pass" if audit["equal"] else "fail",
                    "audit": audit,
                }, separators=(",", ":")))
                if not audit["equal"]:
                    return 1
        print(json.dumps({
            "gate": "A-vs-A",
            "result": "pass",
            "checkpoints": args.checkpoints,
            "instructions": args.checkpoints * args.stride,
            "scope": "CPU+MEM1+MEM2+locked-L1",
            "full_machine": False,
        }, separators=(",", ":")))
        return 0
    finally:
        a.close()
        b.close()


def cmd_gate3(args: argparse.Namespace) -> int:
    a = start_runtime(args, "gate3-a", args.port)
    b = start_runtime(args, "gate3-b", args.port + 1)
    try:
        if args.instructions:
            a.client.request("cosim_step", count=args.instructions)
            b.client.request("cosim_step", count=args.instructions)
            wait_parked(a.client, args.instructions)
            wait_parked(b.client, args.instructions)
        before_a = a.client.request("cosim_state")
        before_b = b.client.request("cosim_state")
        equal_before, _ = compare_states(0, before_a, before_b)
        if not equal_before:
            raise ProtocolError("gate 3 precondition failed: pair already differs")
        b.client.request("cosim_inject", kind="gpr", index=args.gpr, xor=args.xor)
        after_a = a.client.request("cosim_state")
        after_b = b.client.request("cosim_state")
        equal_after, report = compare_states(1, after_a, after_b)
        detected = not equal_after and "cpu" in report["differing_subsystems"]
        print(json.dumps({
            "gate": "injected-divergence",
            "result": "pass" if detected else "fail",
            "injection": {"kind": "gpr", "index": args.gpr, "xor": args.xor},
            "report": report,
        }, separators=(",", ":")))
        return 0 if detected else 1
    finally:
        a.close()
        b.close()


def cmd_dolphin_probe(args: argparse.Namespace) -> int:
    gdb = GdbRemote(args.port, timeout=args.timeout)
    try:
        regs = gdb.registers()
        print(json.dumps(regs, separators=(",", ":")))
        if args.steps:
            for step in range(1, args.steps + 1):
                regs = gdb.step()
                print(json.dumps({"step": step, "pc": regs["pc"], "lr": regs["lr"],
                                  "gpr": regs["gpr"]}, separators=(",", ":")))
        return 0
    finally:
        gdb.close()


def cmd_dolphin_run_to(args: argparse.Namespace) -> int:
    if (args.gpr is None) != (args.gpr_value is None):
        raise ProtocolError("--gpr and --gpr-value must be supplied together")
    if args.gpr is not None and not 0 <= args.gpr < 32:
        raise ProtocolError("--gpr must be in 0..31")
    if (args.after_gpr is None) != (args.after_gpr_value is None):
        raise ProtocolError(
            "--after-gpr and --after-gpr-value must be supplied together"
        )
    if args.after_gpr is not None and not 0 <= args.after_gpr < 32:
        raise ProtocolError("--after-gpr must be in 0..31")
    if args.after_pc is None and any(
        value is not None
        for value in (args.after_gpr, args.after_gpr_value, args.after_lr)
    ):
        raise ProtocolError("--after-pc is required for an after condition")
    if args.after_pc is not None and args.runtime_port is None:
        raise ProtocolError("--after-pc requires --runtime-port")
    gdb = GdbRemote(args.port, timeout=args.timeout)
    breakpoints: set[int] = set()
    try:
        start = gdb.registers()
        gdb.add_breakpoint(args.pc)
        breakpoints.add(args.pc)
        stop = ""
        end = start
        hits = 0
        condition_matched = args.gpr is None and args.lr is None
        while hits < args.max_hits:
            stop = gdb.continue_signal()
            end = gdb.registers()
            if end["pc"] != args.pc:
                break
            hits += 1
            condition_matched = (
                args.gpr is None or end["gpr"][args.gpr] == args.gpr_value
            ) and (args.lr is None or end["lr"] == args.lr)
            if condition_matched:
                break
        reached = end["pc"] == args.pc and condition_matched
        first_end = end
        first_stop = stop
        first_hits = hits
        first_reached = reached
        runtime = (
            JsonTcp(args.runtime_port, timeout=args.timeout)
            if args.runtime_port is not None else None
        )
        runtime_gate = None
        if args.normalize_u32 or args.after_pc is not None:
            if runtime is None:
                raise ProtocolError(
                    "memory normalization and after gates require --runtime-port"
                )
            runtime_gate = runtime.request("checkpoint_status")
            if (
                not runtime_gate.get("parked")
                or runtime_gate.get("live_pc") != args.pc
            ):
                raise ProtocolError(
                    "runtime is not parked at the requested initial checkpoint"
                )
        normalizations = []
        if args.normalize_u32:
            if not reached:
                raise ProtocolError(
                    "refusing to normalize memory before the requested "
                    "conditional checkpoint is reached"
                )
            assert runtime is not None
            for address, value in args.normalize_u32:
                fixed = value.to_bytes(4, "big")
                native_before = runtime_memory(runtime, address, 4)
                oracle_before = dolphin_memory(gdb, address, 4)
                runtime.request("write_ram", addr=address, hex=fixed.hex())
                gdb.write_memory(address, fixed)
                native_after = runtime_memory(runtime, address, 4)
                oracle_after = dolphin_memory(gdb, address, 4)
                if native_after != fixed or oracle_after != fixed:
                    raise ProtocolError(
                        f"normalization readback failed at 0x{address:08X}"
                    )
                normalizations.append({
                    "address": f"0x{address:08X}",
                    "value": f"0x{value:08X}",
                    "runtime_before": native_before.hex(),
                    "dolphin_before": oracle_before.hex(),
                    "runtime_after": native_after.hex(),
                    "dolphin_after": oracle_after.hex(),
                })
        after = None
        if args.after_pc is not None:
            if not reached:
                raise ProtocolError(
                    "refusing to continue before the initial Dolphin "
                    "checkpoint is reached"
                )
            assert runtime is not None
            gdb.remove_breakpoint(args.pc)
            breakpoints.remove(args.pc)
            gdb.add_breakpoint(args.after_pc)
            breakpoints.add(args.after_pc)

            continue_params: dict[str, int] = {"pc": args.after_pc}
            if args.after_gpr is not None:
                continue_params["gpr"] = args.after_gpr
                continue_params["gpr_value"] = args.after_gpr_value
            if args.after_lr is not None:
                continue_params["lr"] = args.after_lr
            runtime.request("checkpoint_continue", **continue_params)

            after_stop = ""
            after_end = end
            after_hits = 0
            after_condition_matched = (
                args.after_gpr is None and args.after_lr is None
            )
            while after_hits < args.max_hits:
                after_stop = gdb.continue_signal()
                after_end = gdb.registers()
                if after_end["pc"] != args.after_pc:
                    break
                after_hits += 1
                after_condition_matched = (
                    args.after_gpr is None
                    or after_end["gpr"][args.after_gpr] == args.after_gpr_value
                ) and (
                    args.after_lr is None or after_end["lr"] == args.after_lr
                )
                if after_condition_matched:
                    break

            deadline = time.monotonic() + args.after_runtime_timeout
            runtime_after = runtime.request("checkpoint_status")
            while not runtime_after.get("parked"):
                if not runtime_after.get("armed"):
                    raise ProtocolError(
                        "runtime disarmed before reaching the after checkpoint"
                    )
                if time.monotonic() >= deadline:
                    raise ProtocolError(
                        "runtime did not reach the after checkpoint within "
                        f"{args.after_runtime_timeout:g} seconds"
                    )
                time.sleep(0.01)
                runtime_after = runtime.request("checkpoint_status")

            after_reached = (
                after_end["pc"] == args.after_pc
                and after_condition_matched
                and runtime_after.get("live_pc") == args.after_pc
            )
            after = {
                "target_pc": f"0x{args.after_pc:08X}",
                "stop": after_stop,
                "end": after_end,
                "hits": after_hits,
                "condition": None if args.after_gpr is None else {
                    "gpr": args.after_gpr,
                    "value": f"0x{args.after_gpr_value:08X}",
                    "matched": after_condition_matched,
                },
                "lr_condition": None if args.after_lr is None else {
                    "value": f"0x{args.after_lr:08X}",
                    "matched": after_end["lr"] == args.after_lr,
                },
                "runtime": runtime_after,
                "reached": after_reached,
            }
            end = after_end
            reached = reached and after_reached
        relative_memory = []
        if args.gpr_memory:
            if args.runtime_port is None:
                raise ProtocolError("--gpr-memory requires --runtime-port")
            assert runtime is not None
            native = runtime.request("get_registers")
            for gpr, offset, length in args.gpr_memory:
                native_address = (native["gpr"][gpr] + offset) & 0xFFFFFFFF
                oracle_address = (end["gpr"][gpr] + offset) & 0xFFFFFFFF
                native_bytes = runtime_memory(runtime, native_address, length)
                oracle_bytes = dolphin_memory(gdb, oracle_address, length)
                first = next(
                    (i for i, pair in enumerate(zip(native_bytes, oracle_bytes))
                     if pair[0] != pair[1]),
                    None,
                )
                differing_chunks = [
                    offset
                    for offset in range(0, length, 32)
                    if native_bytes[offset:offset + 32] !=
                       oracle_bytes[offset:offset + 32]
                ]
                context = None
                if first is not None:
                    context_start = max(0, first - 16)
                    context_end = min(length, first + 17)
                    context = {
                        "start_offset": context_start,
                        "runtime_hex":
                            native_bytes[context_start:context_end].hex(),
                        "dolphin_hex":
                            oracle_bytes[context_start:context_end].hex(),
                    }
                relative_memory.append({
                    "gpr": gpr,
                    "offset": offset,
                    "length": length,
                    "runtime_address": f"0x{native_address:08X}",
                    "dolphin_address": f"0x{oracle_address:08X}",
                    "equal": native_bytes == oracle_bytes,
                    "runtime_sha256": hashlib.sha256(native_bytes).hexdigest(),
                    "dolphin_sha256": hashlib.sha256(oracle_bytes).hexdigest(),
                    "first_difference": None if first is None else {
                        "offset": first,
                        "runtime": native_bytes[first],
                        "dolphin": oracle_bytes[first],
                    },
                    "first_difference_context": context,
                    "differing_32byte_chunks": differing_chunks[:128],
                    "differing_32byte_chunk_count": len(differing_chunks),
                })
        report = {
            "start_pc": f"0x{start['pc']:08X}",
            "target_pc": f"0x{args.pc:08X}",
            "stop": first_stop,
            "end": first_end,
            "hits": first_hits,
            "condition": None if args.gpr is None else {
                "gpr": args.gpr,
                "value": f"0x{args.gpr_value:08X}",
                "matched": condition_matched,
            },
            "lr_condition": None if args.lr is None else {
                "value": f"0x{args.lr:08X}",
                "matched": first_end["lr"] == args.lr,
            },
            "runtime": runtime_gate,
            "normalizations": normalizations,
            "after": after,
            "relative_memory": relative_memory,
            "reached": first_reached and reached,
        }
        print(json.dumps(report, separators=(",", ":")))
        return 0 if report["reached"] else 1
    finally:
        for breakpoint in breakpoints:
            try:
                gdb.remove_breakpoint(breakpoint)
            except (OSError, ProtocolError):
                pass
        gdb.close()


def scan_dolphin_memory(
    gdb: GdbRemote, memory_range: tuple[int, int], needle: bytes
) -> list[str]:
    address, length = memory_range
    hits: list[str] = []
    tail = b""
    offset = 0
    while offset < length:
        chunk_len = min(4096, length - offset)
        chunk = gdb.memory(address + offset, chunk_len)
        joined = tail + chunk
        base = address + offset - len(tail)
        position = joined.find(needle)
        while position >= 0:
            hit = base + position
            if address <= hit and hit + len(needle) <= address + length:
                hits.append(f"0x{hit:08X}")
            position = joined.find(needle, position + 1)
        tail = joined[-(len(needle) - 1):] if len(needle) > 1 else b""
        offset += chunk_len
    return hits


def parse_hex_needle(value: str) -> bytes:
    try:
        needle = bytes.fromhex(value)
    except ValueError as exc:
        raise ProtocolError(f"invalid hex signature: {exc}") from exc
    if not needle:
        raise ProtocolError("hex signature must not be empty")
    return needle


def cmd_dolphin_run_for(args: argparse.Namespace) -> int:
    gdb = GdbRemote(args.port, timeout=args.timeout)
    try:
        start = gdb.registers()
        stop = gdb.continue_for(args.seconds)
        end = gdb.registers()
        hits: list[str] = []
        if args.find_hex:
            hits = scan_dolphin_memory(
                gdb, args.memory, parse_hex_needle(args.find_hex)
            )
        report = {
            "start_pc": f"0x{start['pc']:08X}",
            "seconds": args.seconds,
            "stop": stop,
            "end": end,
            "signature_hits": hits,
        }
        print(json.dumps(report, separators=(",", ":")))
        return 0
    finally:
        gdb.close()


def cmd_dolphin_find_live(args: argparse.Namespace) -> int:
    """Locate transient relocated code without reconnecting Dolphin's GDB stub."""
    gdb = GdbRemote(args.port, timeout=args.timeout)
    needle = parse_hex_needle(args.find_hex)
    checkpoints: list[dict[str, Any]] = []
    try:
        start = gdb.registers()
        for attempt in range(1, args.attempts + 1):
            stop = gdb.continue_for(args.interval)
            state = gdb.registers()
            hits = scan_dolphin_memory(gdb, args.memory, needle)
            checkpoint = {
                "attempt": attempt,
                "guest_seconds": attempt * args.interval,
                "pc": f"0x{state['pc']:08X}",
                "stop": stop,
                "signature_hits": hits,
            }
            checkpoints.append(checkpoint)
            print(json.dumps(checkpoint, separators=(",", ":")), file=sys.stderr,
                  flush=True)
            if hits:
                report = {
                    "start_pc": f"0x{start['pc']:08X}",
                    "found": True,
                    "checkpoint": checkpoint,
                    "checkpoints": checkpoints,
                    "end": state,
                }
                print(json.dumps(report, separators=(",", ":")))
                return 0
        end = gdb.registers()
        report = {
            "start_pc": f"0x{start['pc']:08X}",
            "found": False,
            "checkpoints": checkpoints,
            "end": end,
        }
        print(json.dumps(report, separators=(",", ":")))
        return 1
    finally:
        gdb.close()


def parse_range(spec: str) -> tuple[int, int]:
    address, separator, length = spec.partition(":")
    if not separator:
        raise argparse.ArgumentTypeError("memory range must be ADDRESS:LENGTH")
    try:
        parsed = (int(address, 0), int(length, 0))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if parsed[1] <= 0:
        raise argparse.ArgumentTypeError("memory range length must be positive")
    return parsed


def parse_gpr_range(spec: str) -> tuple[int, int, int]:
    fields = spec.split(":")
    if len(fields) != 3:
        raise argparse.ArgumentTypeError(
            "GPR-relative range must be GPR:OFFSET:LENGTH"
        )
    try:
        gpr, offset, length = (int(field, 0) for field in fields)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 <= gpr < 32:
        raise argparse.ArgumentTypeError("GPR must be in 0..31")
    if length <= 0:
        raise argparse.ArgumentTypeError("range length must be positive")
    return gpr, offset, length


def parse_u32_patch(spec: str) -> tuple[int, int]:
    address, separator, value = spec.partition(":")
    if not separator:
        raise argparse.ArgumentTypeError(
            "32-bit normalization must be ADDRESS:VALUE"
        )
    try:
        parsed = (int(address, 0), int(value, 0))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 <= parsed[0] <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("normalization address must fit in 32 bits")
    if not 0 <= parsed[1] <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("normalization value must fit in 32 bits")
    return parsed


def runtime_memory(client: JsonTcp, address: int, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk_len = min(65536, length - len(data))
        reply = client.request("read_ram", addr=address + len(data), len=chunk_len)
        chunk = bytes.fromhex(reply["hex"])
        if len(chunk) != chunk_len:
            raise ProtocolError("runtime returned a short memory read")
        data.extend(chunk)
    return bytes(data)


def dolphin_memory(gdb: GdbRemote, address: int, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        # Dolphin's GDB reply buffer holds just under 5 KiB of binary-as-hex.
        chunk_len = min(4096, length - len(data))
        data.extend(gdb.memory(address + len(data), chunk_len))
    return bytes(data)


def cmd_ab_initial(args: argparse.Namespace) -> int:
    runtime = start_runtime(args, "ab-runtime", args.port)
    gdb = GdbRemote(args.dolphin_port, timeout=args.timeout)
    try:
        native = runtime.client.request("get_registers")
        oracle = gdb.registers()
        common = ("pc", "lr", "ctr", "cr", "xer", "msr", "srr0", "srr1", "dar", "dsisr")
        reg_diff: dict[str, Any] = {}
        for key in common:
            if native[key] != oracle[key]:
                reg_diff[key] = {
                    "runtime": f"0x{native[key]:08X}",
                    "dolphin": f"0x{oracle[key]:08X}",
                }
        gpr_diff = {
            str(i): {
                "runtime": f"0x{native['gpr'][i]:08X}",
                "dolphin": f"0x{oracle['gpr'][i]:08X}",
            }
            for i in range(32)
            if native["gpr"][i] != oracle["gpr"][i]
        }
        if gpr_diff:
            reg_diff["gpr"] = gpr_diff
        dolphin_tb = (oracle["tbu"] << 32) | oracle["tbl"]
        if native["timebase"] != dolphin_tb:
            reg_diff["timebase"] = {
                "runtime": f"0x{native['timebase']:016X}",
                "dolphin": f"0x{dolphin_tb:016X}",
            }

        ranges = args.memory or [
            (0x80000000, 0x4000),
            (0x81200000, 0x1000),
        ]
        memory_reports = []
        for address, length in ranges:
            native_bytes = runtime_memory(runtime.client, address, length)
            oracle_bytes = dolphin_memory(gdb, address, length)
            first = next(
                (i for i, pair in enumerate(zip(native_bytes, oracle_bytes))
                 if pair[0] != pair[1]),
                None,
            )
            memory_reports.append({
                "address": f"0x{address:08X}",
                "length": length,
                "equal": native_bytes == oracle_bytes,
                "runtime_sha256": hashlib.sha256(native_bytes).hexdigest(),
                "dolphin_sha256": hashlib.sha256(oracle_bytes).hexdigest(),
                "first_difference": None if first is None else {
                    "address": f"0x{address + first:08X}",
                    "runtime": native_bytes[first],
                    "dolphin": oracle_bytes[first],
                },
            })

        report = {
            "sync": {
                "kind": "shared-bs2-entry",
                "runtime_pc": f"0x{native['pc']:08X}",
                "dolphin_pc": f"0x{oracle['pc']:08X}",
            },
            "registers_equal": not reg_diff,
            "register_differences": reg_diff,
            "memory": memory_reports,
            "runtime_state": runtime.client.request("cosim_state"),
            "full_machine": False,
        }
        print(json.dumps(report, separators=(",", ":")))
        return 0 if not reg_diff and all(item["equal"] for item in memory_reports) else 1
    finally:
        gdb.close()
        runtime.close()


def common_register_diff(native: dict[str, Any], oracle: dict[str, Any]) -> dict[str, Any]:
    common = ("pc", "lr", "ctr", "cr", "xer", "msr", "srr0", "srr1", "dar", "dsisr")
    diff: dict[str, Any] = {}
    for key in common:
        if native[key] != oracle[key]:
            diff[key] = {
                "runtime": f"0x{native[key]:08X}",
                "dolphin": f"0x{oracle[key]:08X}",
            }
    gprs = {
        str(i): {
            "runtime": f"0x{native['gpr'][i]:08X}",
            "dolphin": f"0x{oracle['gpr'][i]:08X}",
        }
        for i in range(32)
        if native["gpr"][i] != oracle["gpr"][i]
    }
    if gprs:
        diff["gpr"] = gprs
    dolphin_tb = (oracle["tbu"] << 32) | oracle["tbl"]
    if native["timebase"] != dolphin_tb:
        diff["timebase"] = {
            "runtime": f"0x{native['timebase']:016X}",
            "dolphin": f"0x{dolphin_tb:016X}",
        }
    return diff


def compare_memory_ranges(
    runtime: JsonTcp, gdb: GdbRemote, ranges: list[tuple[int, int]]
) -> list[dict[str, Any]]:
    reports = []
    for address, length in ranges:
        native_bytes = runtime_memory(runtime, address, length)
        oracle_bytes = dolphin_memory(gdb, address, length)
        first = next(
            (i for i, pair in enumerate(zip(native_bytes, oracle_bytes))
             if pair[0] != pair[1]),
            None,
        )
        reports.append({
            "address": f"0x{address:08X}",
            "length": length,
            "equal": native_bytes == oracle_bytes,
            "runtime_sha256": hashlib.sha256(native_bytes).hexdigest(),
            "dolphin_sha256": hashlib.sha256(oracle_bytes).hexdigest(),
            "first_difference": None if first is None else {
                "address": f"0x{address + first:08X}",
                "runtime": native_bytes[first],
                "dolphin": oracle_bytes[first],
            },
        })
    return reports


def cmd_ab_step(args: argparse.Namespace) -> int:
    runtime = start_runtime(args, "ab-step-runtime", args.port)
    gdb = GdbRemote(args.dolphin_port, timeout=args.timeout)
    ranges = args.memory or [
        (0x81200000, 0x700),
        (0x81300000, 0x1000),
    ]
    try:
        native = runtime.client.request("get_registers")
        oracle = gdb.registers()
        initial_diff = common_register_diff(native, oracle)
        initial_memory = compare_memory_ranges(runtime.client, gdb, ranges)
        if initial_diff or not all(item["equal"] for item in initial_memory):
            raise ProtocolError(
                "A-vs-B initial checkpoint is not aligned; run ab-initial first"
            )

        instruction = 0
        normalization_used = False
        for checkpoint in range(1, args.checkpoints + 1):
            target = instruction + args.stride
            runtime.client.request("cosim_step", count=args.stride)
            for _ in range(args.stride):
                gdb.step_signal()
            wait_parked(runtime.client, target)
            instruction = target

            native = runtime.client.request("get_registers")
            oracle = gdb.registers()
            reg_diff = common_register_diff(native, oracle)
            memory = compare_memory_ranges(runtime.client, gdb, ranges)
            memory_equal = all(item["equal"] for item in memory)
            normalization = None
            if args.align_dolphin_timebase_at == instruction:
                # Dolphin's BS2 loader exposes TBL/TBU as zero at 0x81200150,
                # then GetFakeTimeBase materializes a host/restart-dependent
                # epoch at the first mftb (0x81200174). Align exactly once at
                # the parked boundary after that instruction. This is an
                # explicit seam normalization, not a comparison mask.
                actual = set(reg_diff)
                gpr_diff = reg_diff.get("gpr", {})
                if (
                    not memory_equal
                    or native["pc"] != oracle["pc"]
                    or not actual.issubset({"gpr", "timebase"})
                    or set(gpr_diff) - {"5"}
                ):
                    raise ProtocolError(
                        "time-base alignment boundary has an unrelated divergence"
                    )
                oracle_tb = (oracle["tbu"] << 32) | oracle["tbl"]
                runtime.client.request(
                    "cosim_inject",
                    kind="timebase",
                    value_hi=(oracle_tb >> 32) & 0xFFFFFFFF,
                    value_lo=oracle_tb & 0xFFFFFFFF,
                )
                if native["gpr"][5] != oracle["gpr"][5]:
                    runtime.client.request(
                        "cosim_inject",
                        kind="gpr",
                        index=5,
                        xor=native["gpr"][5] ^ oracle["gpr"][5],
                    )
                normalization = {
                    "kind": "dolphin-timebase-epoch",
                    "instruction": instruction,
                    "pc": f"0x{native['pc']:08X}",
                    "oracle_timebase": f"0x{oracle_tb:016X}",
                    "fields_written": ["timebase", "gpr5"],
                    "reason": (
                        "Dolphin materializes a restart-dependent fake-timebase "
                        "epoch on the first mftb"
                    ),
                }
                normalization_used = True
                native = runtime.client.request("get_registers")
                reg_diff = common_register_diff(native, oracle)
            report = {
                "checkpoint": checkpoint,
                "instruction": instruction,
                "runtime_pc": f"0x{native['pc']:08X}",
                "dolphin_pc": f"0x{oracle['pc']:08X}",
                "register_differences": reg_diff,
                "memory": memory,
            }
            if normalization is not None:
                report["normalization"] = normalization
            print(json.dumps(report, separators=(",", ":")))
            if reg_diff or not memory_equal:
                report["mmio_window"] = runtime.client.request("mmio_dump", count=64)
                report["event_window"] = runtime.client.request("event_dump", count=32)
                report["block_window"] = runtime.client.request("block_dump", count=32)
                print(json.dumps({
                    "first_divergence": report,
                    "scope": "common PPC registers + requested RAM ranges",
                    "full_machine": False,
                }, separators=(",", ":")))
                return 1

        print(json.dumps({
            "result": "no divergence in sampled surface",
            "instructions": instruction,
            "checkpoints": args.checkpoints,
            "stride": args.stride,
            "scope": "common PPC registers + requested RAM ranges",
            "full_machine": False,
            "normalization_used": normalization_used,
        }, separators=(",", ":")))
        return 0
    finally:
        gdb.close()
        runtime.close()


def cmd_ab_milestone(args: argparse.Namespace) -> int:
    runtime = start_runtime(args, "ab-milestone-runtime", args.port)
    gdb = GdbRemote(args.dolphin_port, timeout=args.timeout)
    ranges = args.memory or [
        (0x81200000, 0x700),
        (0x81300000, 0x1000),
    ]
    final_ranges = args.final_memory or ranges
    breakpoint_added = False
    try:
        native = runtime.client.request("get_registers")
        oracle = gdb.registers()
        initial_diff = common_register_diff(native, oracle)
        initial_memory = compare_memory_ranges(runtime.client, gdb, ranges)
        if initial_diff or not all(item["equal"] for item in initial_memory):
            raise ProtocolError(
                "A-vs-B initial checkpoint is not aligned; run ab-initial first"
            )

        runtime.client.request(
            "cosim_run_to",
            pc=args.pc,
            max_instructions=args.max_instructions,
        )
        gdb.add_breakpoint(args.pc)
        breakpoint_added = True
        gdb.continue_signal()
        oracle = gdb.registers()
        if oracle["pc"] != args.pc:
            raise ProtocolError(
                f"Dolphin stopped at 0x{oracle['pc']:08X}, "
                f"wanted 0x{args.pc:08X}"
            )
        runtime_status = wait_parked_pc(runtime.client, args.pc, args.timeout)
        native = runtime.client.request("get_registers")

        raw_diff = common_register_diff(native, oracle)
        decision_diff = dict(raw_diff)
        ignored: dict[str, Any] = {}
        if args.ignore_timebase and "timebase" in decision_diff:
            ignored["timebase"] = decision_diff.pop("timebase")
        if "gpr" in decision_diff:
            remaining_gprs = dict(decision_diff["gpr"])
            ignored_gprs = {
                key: remaining_gprs.pop(key)
                for key in sorted(set(remaining_gprs) & {str(i) for i in args.ignore_gpr})
            }
            if ignored_gprs:
                ignored["gpr"] = ignored_gprs
            if remaining_gprs:
                decision_diff["gpr"] = remaining_gprs
            else:
                decision_diff.pop("gpr")

        memory = compare_memory_ranges(runtime.client, gdb, final_ranges)
        memory_equal = all(item["equal"] for item in memory)
        report = {
            "sync": {
                "kind": "guest-pc-milestone",
                "pc": f"0x{args.pc:08X}",
                "runtime_instruction": runtime_status["instruction"],
            },
            "register_differences": raw_diff,
            "ignored_timing_carriers": ignored,
            "decision_register_differences": decision_diff,
            "memory": memory,
            "runtime_mmio_window": runtime.client.request("mmio_dump", count=128),
            "runtime_event_window": runtime.client.request("event_dump", count=64),
            "scope": "common PPC registers + requested RAM ranges",
            "full_machine": False,
        }
        passed = not decision_diff and memory_equal
        report["result"] = "match" if passed else "divergence"
        print(json.dumps(report, separators=(",", ":")))
        return 0 if passed else 1
    finally:
        if breakpoint_added:
            try:
                gdb.remove_breakpoint(args.pc)
            except (OSError, ProtocolError):
                pass
        gdb.close()
        runtime.close()


_XFB_INDEX_RE = {
    "runtime": re.compile(r"^runtime\.(\d+)\.yuy2$"),
    "dolphin": re.compile(r"^dolphin\.(\d+)\.yuy2$"),
}


def discover_max_xfb_index(directory: pathlib.Path, prefix: str) -> int:
    """Highest ordinal named `<prefix>.<k>.yuy2` in `directory`, or -1 if none.

    Used only to size the k-range xfb-diff walks (COSIM_DESIGN.md §4) — a
    missing intermediate file inside that range is a pairing_gap finding, not
    silently skipped.
    """
    pattern = _XFB_INDEX_RE[prefix]
    highest = -1
    if directory.is_dir():
        for entry in directory.iterdir():
            match = pattern.match(entry.name)
            if match:
                highest = max(highest, int(match.group(1)))
    return highest


def read_xfb_dump(path: pathlib.Path) -> dict[str, Any]:
    """Parse one runtime.<k>.yuy2 / dolphin.<k>.yuy2 file: a little-endian
    width,height,stride u32 prefix followed by exactly stride*height raw
    bytes (COSIM_DESIGN.md §3/§2 — same wire shape on both sides)."""
    with open(path, "rb") as handle:
        header = handle.read(12)
        if len(header) < 12:
            return {"error": "header_too_short", "header_bytes": len(header)}
        width, height, stride = struct.unpack("<III", header)
        payload = handle.read()
    expected = stride * height
    if len(payload) != expected:
        return {
            "error": "payload_size_mismatch",
            "width": width, "height": height, "stride": stride,
            "expected_bytes": expected, "actual_bytes": len(payload),
        }
    return {"width": width, "height": height, "stride": stride, "payload": payload}


def xfb_tile_for_offset(
    offset: int, width: int, height: int, stride: int, grid: int
) -> tuple[int, int] | None:
    """8x8 (default) tile-grid localization for one differing byte offset.

    Returns None for a byte inside row padding (stride > width*2) — that
    region is not part of any visible tile and is reported separately so it
    cannot be mistaken for a rendering-content divergence.
    """
    if stride <= 0 or height <= 0:
        return None
    row = offset // stride
    col = offset % stride
    row_bytes = width * 2
    if row >= height or col >= row_bytes or row_bytes <= 0:
        return None
    tile_row = min(grid - 1, (row * grid) // height)
    tile_col = min(grid - 1, (col * grid) // row_bytes)
    return tile_row, tile_col


def compare_xfb_payloads(
    a: bytes, b: bytes, width: int, height: int, stride: int, grid: int
) -> dict[str, Any]:
    """Byte-exact diff + tile-grid localization for one paired XFB dump."""
    differing = 0
    padding_differing = 0
    first: int | None = None
    tiles: dict[str, int] = {}
    for offset, (byte_a, byte_b) in enumerate(zip(a, b)):
        if byte_a == byte_b:
            continue
        differing += 1
        if first is None:
            first = offset
        tile = xfb_tile_for_offset(offset, width, height, stride, grid)
        if tile is None:
            padding_differing += 1
        else:
            key = f"{tile[0]},{tile[1]}"
            tiles[key] = tiles.get(key, 0) + 1
    return {
        "differing_byte_count": differing,
        "padding_differing_byte_count": padding_differing,
        "first_offset": first,
        "tiles": tiles,
    }


def yuy2_pixel_to_rgb(y8: int, u8: int, v8: int) -> tuple[float, float, float]:
    """Inverse BT.601, transcribed from the matrix documented at
    runtime/src/debug_server.c:747-749 (also runtime/include/vi/yuy2.h) —
    the exact formula Dolphin's TextureConversionShader.cpp:1009-1035 uses,
    so the perceptual fallback (COSIM_DESIGN.md §4) decodes both sides
    identically."""
    yc = 1.164 * (y8 - 16.0)
    r = yc + 1.596 * (v8 - 128.0)
    g = yc - 0.813 * (v8 - 128.0) - 0.391 * (u8 - 128.0)
    b = yc + 2.018 * (u8 - 128.0)
    clamp = lambda c: 0.0 if c < 0.0 else 255.0 if c > 255.0 else c
    return clamp(r), clamp(g), clamp(b)


def xfb_perceptual_delta(
    a: bytes, b: bytes, width: int, height: int, stride: int
) -> float:
    """Mean absolute per-channel RGB difference after YUY2 decode
    (COSIM_DESIGN.md §4 "Perceptual fallback only for documented-
    nondeterministic layers") — diagnostic only, never the pass/fail basis."""
    total = 0.0
    count = 0
    for y in range(height):
        row_a = a[y * stride:y * stride + width * 2]
        row_b = b[y * stride:y * stride + width * 2]
        for x in range(0, width - 1, 2):
            y0a, ua, y1a, va = row_a[x * 2:x * 2 + 4]
            y0b, ub, y1b, vb = row_b[x * 2:x * 2 + 4]
            for ya, yb, u_a, v_a, u_b, v_b in (
                (y0a, y0b, ua, va, ub, vb),
                (y1a, y1b, ua, va, ub, vb),
            ):
                ra = yuy2_pixel_to_rgb(ya, u_a, v_a)
                rb = yuy2_pixel_to_rgb(yb, u_b, v_b)
                total += abs(ra[0] - rb[0]) + abs(ra[1] - rb[1]) + abs(ra[2] - rb[2])
                count += 3
    return total / count if count else 0.0


def cmd_xfb_diff(args: argparse.Namespace) -> int:
    """Compare runtime.<k>.yuy2 (runtime-dir) against dolphin.<k+offset>.yuy2
    (dolphin-dir) — COSIM_DESIGN.md §4. Pure filesystem comparator: no GDB
    stub, no debug port, so it runs against dumps captured by separate,
    already-finished runtime/Dolphin processes."""
    runtime_dir = pathlib.Path(args.runtime_dir)
    dolphin_dir = pathlib.Path(args.dolphin_dir)
    offset = args.dolphin_offset
    grid = args.tile_grid

    max_runtime = discover_max_xfb_index(runtime_dir, "runtime")
    max_dolphin = discover_max_xfb_index(dolphin_dir, "dolphin")
    upper = max(max_runtime, max_dolphin - offset)
    if args.first_n is not None:
        upper = min(upper, args.first_n - 1)

    compared = 0
    first_divergence: int | None = None
    first_geometry_mismatch: int | None = None
    first_pairing_gap: int | None = None

    for k in range(0, upper + 1):
        runtime_path = runtime_dir / f"runtime.{k}.yuy2"
        dolphin_path = dolphin_dir / f"dolphin.{k + offset}.yuy2"
        runtime_present = runtime_path.is_file()
        dolphin_present = dolphin_path.is_file()
        if not (runtime_present and dolphin_present):
            # A gap is a harness/alignment problem (§1 item 3), never itself
            # the finding — reported under its own "kind", not folded into
            # first_divergence.
            entry = {
                "k": k,
                "kind": "pairing_gap",
                "runtime_path": str(runtime_path),
                "dolphin_path": str(dolphin_path),
                "runtime_missing": not runtime_present,
                "dolphin_missing": not dolphin_present,
            }
            print(json.dumps(entry, separators=(",", ":")))
            if first_pairing_gap is None:
                first_pairing_gap = k
            continue

        runtime_dump = read_xfb_dump(runtime_path)
        dolphin_dump = read_xfb_dump(dolphin_path)
        entry = {
            "k": k,
            "kind": "compared",
            "runtime_k": k,
            "dolphin_k": k + offset,
            "runtime_path": str(runtime_path),
            "dolphin_path": str(dolphin_path),
        }
        if "error" in runtime_dump or "error" in dolphin_dump:
            entry["kind"] = "read_error"
            entry["runtime_error"] = runtime_dump.get("error")
            entry["dolphin_error"] = dolphin_dump.get("error")
            print(json.dumps(entry, separators=(",", ":")))
            if first_divergence is None:
                first_divergence = k
            compared += 1
            continue

        geometry_match = (
            runtime_dump["width"] == dolphin_dump["width"]
            and runtime_dump["height"] == dolphin_dump["height"]
            and runtime_dump["stride"] == dolphin_dump["stride"]
        )
        entry["geometry"] = {
            "runtime": {"width": runtime_dump["width"], "height": runtime_dump["height"],
                        "stride": runtime_dump["stride"]},
            "dolphin": {"width": dolphin_dump["width"], "height": dolphin_dump["height"],
                        "stride": dolphin_dump["stride"]},
        }
        entry["geometry_match"] = geometry_match
        if not geometry_match:
            # A geometry mismatch is a VI-register-programming divergence,
            # triaged separately from a rendering-content divergence (§4).
            entry["exact_match"] = False
            print(json.dumps(entry, separators=(",", ":")))
            if first_geometry_mismatch is None:
                first_geometry_mismatch = k
            if first_divergence is None:
                first_divergence = k
            compared += 1
            continue

        payload_a = runtime_dump["payload"]
        payload_b = dolphin_dump["payload"]
        exact = payload_a == payload_b
        entry["exact_match"] = exact
        if exact:
            entry["differing_byte_count"] = 0
            entry["padding_differing_byte_count"] = 0
            entry["first_difference"] = None
            entry["tiles"] = {}
        else:
            diff = compare_xfb_payloads(
                payload_a, payload_b,
                runtime_dump["width"], runtime_dump["height"], runtime_dump["stride"],
                grid,
            )
            first_offset = diff["first_offset"]
            tile = xfb_tile_for_offset(
                first_offset, runtime_dump["width"], runtime_dump["height"],
                runtime_dump["stride"], grid,
            )
            entry["differing_byte_count"] = diff["differing_byte_count"]
            entry["padding_differing_byte_count"] = diff["padding_differing_byte_count"]
            entry["first_difference"] = {
                "offset": first_offset,
                "runtime_byte": payload_a[first_offset],
                "dolphin_byte": payload_b[first_offset],
                "tile": None if tile is None else {"row": tile[0], "col": tile[1]},
            }
            entry["tiles"] = diff["tiles"]
            if first_divergence is None:
                first_divergence = k
            if args.perceptual_fallback:
                # Exact-byte result above is always printed; this is an
                # additional, explicitly-requested diagnostic, never a
                # silent override (§4 "the fallback never silently hides a
                # real divergence").
                delta = xfb_perceptual_delta(
                    payload_a, payload_b,
                    runtime_dump["width"], runtime_dump["height"], runtime_dump["stride"],
                )
                entry["perceptual_fallback"] = {
                    "layers": args.perceptual_fallback,
                    "mean_abs_channel_diff": delta,
                    "threshold": args.perceptual_threshold,
                    "would_pass_perceptually": delta <= args.perceptual_threshold,
                }
        print(json.dumps(entry, separators=(",", ":")))
        compared += 1

    result = "pass" if first_divergence is None and first_pairing_gap is None else "fail"
    summary = {
        "kind": "summary",
        "compared": compared,
        "dolphin_offset": offset,
        "first_divergence": first_divergence,
        "first_geometry_mismatch": first_geometry_mismatch,
        "first_pairing_gap": first_pairing_gap,
        "result": result,
    }
    print(json.dumps(summary, separators=(",", ":")))
    return 0 if result == "pass" else 1


def add_runtime_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--exe", required=True)
    parser.add_argument("--ipl", required=True)
    parser.add_argument("--boot-mode", choices=("bs1", "bs2"), default="bs1")
    parser.add_argument("--disc", required=True)
    parser.add_argument("--dsp-rom", required=True)
    parser.add_argument("--dsp-coef", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--port", type=int, default=4410)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    gate1 = sub.add_parser("gate1", help="validate deterministic A-vs-A state")
    add_runtime_args(gate1)
    gate1.add_argument("--stride", type=int, default=1000)
    gate1.add_argument("--checkpoints", type=int, default=10)
    gate1.add_argument(
        "--byte-audit-every",
        type=int,
        default=10,
        help="compare canonical CPU and all represented memory bytes every N checkpoints",
    )
    gate1.set_defaults(func=cmd_gate1)

    gate3 = sub.add_parser("gate3", help="validate injected-divergence detection")
    add_runtime_args(gate3)
    gate3.add_argument("--instructions", type=int, default=1000)
    gate3.add_argument("--gpr", type=int, default=7)
    gate3.add_argument("--xor", type=lambda value: int(value, 0), default=1)
    gate3.set_defaults(func=cmd_gate3)

    probe = sub.add_parser("dolphin-probe", help="query a parked Dolphin GDB stub")
    probe.add_argument("--port", type=int, required=True)
    probe.add_argument("--timeout", type=float, default=10.0)
    probe.add_argument("--steps", type=int, default=0)
    probe.set_defaults(func=cmd_dolphin_probe)

    dolphin_run_to = sub.add_parser(
        "dolphin-run-to", help="continue Dolphin to a guest-PC breakpoint"
    )
    dolphin_run_to.add_argument("--port", type=int, required=True)
    dolphin_run_to.add_argument("--timeout", type=float, default=600.0)
    dolphin_run_to.add_argument(
        "--pc", type=lambda value: int(value, 0), required=True
    )
    dolphin_run_to.add_argument(
        "--gpr", type=int,
        help="accept the breakpoint only when this GPR matches --gpr-value",
    )
    dolphin_run_to.add_argument(
        "--gpr-value", type=lambda value: int(value, 0),
        help="required live value for --gpr",
    )
    dolphin_run_to.add_argument(
        "--lr", type=lambda value: int(value, 0),
        help="accept the breakpoint only when the live link register matches",
    )
    dolphin_run_to.add_argument(
        "--max-hits", type=int, default=100000,
        help="maximum breakpoint hits to inspect before failing the condition",
    )
    dolphin_run_to.add_argument(
        "--runtime-port", type=int,
        help="parked runtime TCP port used by --gpr-memory comparisons",
    )
    dolphin_run_to.add_argument(
        "--gpr-memory", type=parse_gpr_range, action="append",
        help=(
            "compare bytes relative to each machine's own live GPR as "
            "GPR:OFFSET:LENGTH (repeatable)"
        ),
    )
    dolphin_run_to.add_argument(
        "--normalize-u32", type=parse_u32_patch, action="append",
        help=(
            "after the conditional checkpoint is reached, write the same "
            "big-endian guest word to both parked machines as ADDRESS:VALUE; "
            "repeatable and reported as an explicit oracle seam"
        ),
    )
    dolphin_run_to.add_argument(
        "--after-pc", type=lambda value: int(value, 0),
        help=(
            "after normalization, atomically resume the parked runtime and "
            "continue both machines to this second AOT checkpoint"
        ),
    )
    dolphin_run_to.add_argument(
        "--after-gpr", type=int,
        help="accept the second checkpoint only when this live GPR matches",
    )
    dolphin_run_to.add_argument(
        "--after-gpr-value", type=lambda value: int(value, 0),
        help="required live value for --after-gpr",
    )
    dolphin_run_to.add_argument(
        "--after-lr", type=lambda value: int(value, 0),
        help="accept the second checkpoint only when the live LR matches",
    )
    dolphin_run_to.add_argument(
        "--after-runtime-timeout", type=float, default=300.0,
        help=(
            "seconds to wait for the runtime's second checkpoint after "
            "Dolphin reaches it (default: 300)"
        ),
    )
    dolphin_run_to.set_defaults(func=cmd_dolphin_run_to)

    dolphin_run_for = sub.add_parser(
        "dolphin-run-for",
        help="continue Dolphin for wall time, interrupt it, and optionally scan RAM",
    )
    dolphin_run_for.add_argument("--port", type=int, required=True)
    dolphin_run_for.add_argument("--timeout", type=float, default=600.0)
    dolphin_run_for.add_argument("--seconds", type=float, required=True)
    dolphin_run_for.add_argument(
        "--find-hex", help="hex byte signature to locate after the timed stop"
    )
    dolphin_run_for.add_argument(
        "--memory",
        type=parse_range,
        default=(0x80000000, 0x01800000),
        help="guest memory range ADDRESS:LENGTH to scan",
    )
    dolphin_run_for.set_defaults(func=cmd_dolphin_run_for)

    dolphin_find_live = sub.add_parser(
        "dolphin-find-live",
        help=(
            "repeatedly continue and scan a single Dolphin GDB session for "
            "transient relocated code"
        ),
    )
    dolphin_find_live.add_argument("--port", type=int, required=True)
    dolphin_find_live.add_argument("--timeout", type=float, default=600.0)
    dolphin_find_live.add_argument(
        "--interval", type=float, default=5.0,
        help="guest wall-time seconds to run between parked scans",
    )
    dolphin_find_live.add_argument(
        "--attempts", type=int, default=12,
        help="maximum number of advance-and-scan checkpoints",
    )
    dolphin_find_live.add_argument(
        "--find-hex", required=True, help="hex byte signature to locate"
    )
    dolphin_find_live.add_argument(
        "--memory",
        type=parse_range,
        default=(0x80000000, 0x01800000),
        help="guest memory range ADDRESS:LENGTH to scan",
    )
    dolphin_find_live.set_defaults(func=cmd_dolphin_find_live)

    ab_initial = sub.add_parser(
        "ab-initial", help="compare a parked runtime against Dolphin at BS2 entry"
    )
    add_runtime_args(ab_initial)
    ab_initial.add_argument("--dolphin-port", type=int, required=True)
    ab_initial.add_argument("--timeout", type=float, default=10.0)
    ab_initial.add_argument(
        "--memory", type=parse_range, action="append",
        help="guest memory range ADDRESS:LENGTH (repeatable)",
    )
    ab_initial.set_defaults(func=cmd_ab_initial)

    ab_step = sub.add_parser(
        "ab-step", help="step parked runtime and Dolphin from the shared BS2 entry"
    )
    add_runtime_args(ab_step)
    ab_step.add_argument("--dolphin-port", type=int, required=True)
    ab_step.add_argument("--timeout", type=float, default=10.0)
    ab_step.add_argument("--stride", type=int, default=1)
    ab_step.add_argument("--checkpoints", type=int, default=100)
    ab_step.add_argument(
        "--align-dolphin-timebase-at",
        type=int,
        metavar="INSTRUCTION",
        help=(
            "once, after this instruction, align runtime timebase and r5 to "
            "Dolphin's restart-dependent first-mftb epoch; comparison remains "
            "strict before and after the disclosed write"
        ),
    )
    ab_step.add_argument(
        "--memory", type=parse_range, action="append",
        help="guest memory range ADDRESS:LENGTH (repeatable)",
    )
    ab_step.set_defaults(func=cmd_ab_step)

    ab_milestone = sub.add_parser(
        "ab-milestone",
        help="run parked runtime and Dolphin to a shared guest-PC milestone",
    )
    add_runtime_args(ab_milestone)
    ab_milestone.add_argument("--dolphin-port", type=int, required=True)
    ab_milestone.add_argument("--timeout", type=float, default=120.0)
    ab_milestone.add_argument("--pc", type=lambda value: int(value, 0), required=True)
    ab_milestone.add_argument("--max-instructions", type=int, default=10000000)
    ab_milestone.add_argument(
        "--ignore-gpr",
        type=int,
        action="append",
        default=[],
        help="explicit timing-carrier GPR excluded from the milestone decision",
    )
    ab_milestone.add_argument(
        "--ignore-timebase",
        action="store_true",
        help="report raw timebase mismatch but exclude it from the decision",
    )
    ab_milestone.add_argument(
        "--memory",
        type=parse_range,
        action="append",
        help="guest memory range ADDRESS:LENGTH (repeatable)",
    )
    ab_milestone.add_argument(
        "--final-memory",
        type=parse_range,
        action="append",
        help=(
            "additional/replacement guest memory range checked only at the "
            "milestone; useful when boot code deliberately overwrites an "
            "initially oracle-specific region"
        ),
    )
    ab_milestone.set_defaults(func=cmd_ab_milestone)

    xfb_diff = sub.add_parser(
        "xfb-diff",
        help=(
            "compare runtime.<k>.yuy2 / dolphin.<k+offset>.yuy2 XFB dumps "
            "(COSIM_DESIGN.md sec4); filesystem-only, no GDB/debug-port"
        ),
    )
    xfb_diff.add_argument(
        "--runtime-dir", required=True,
        help="directory containing runtime.<k>.yuy2 dumps",
    )
    xfb_diff.add_argument(
        "--dolphin-dir", required=True,
        help="directory containing dolphin.<k>.yuy2 dumps",
    )
    xfb_diff.add_argument(
        "--dolphin-offset", type=int, default=0,
        help=(
            "publication-count offset from the sec1 snapshot-resume handshake: "
            "runtime.<k> pairs with dolphin.<k + offset>"
        ),
    )
    xfb_diff.add_argument(
        "--first-n", type=int,
        help="stop after this many runtime ordinals (default: walk every ordinal found)",
    )
    xfb_diff.add_argument(
        "--tile-grid", type=int, default=8,
        help="tile-grid edge length for divergence localization (default: 8x8)",
    )
    xfb_diff.add_argument(
        "--perceptual-fallback", action="append", default=[], metavar="LAYER",
        help=(
            "name a documented-nondeterministic layer (e.g. dither) to also "
            "report a BT.601 perceptual delta on byte-divergent frames; "
            "repeatable; the exact-byte result is always reported regardless"
        ),
    )
    xfb_diff.add_argument(
        "--perceptual-threshold", type=float, default=4.0,
        help="mean abs per-channel RGB delta below which --perceptual-fallback reports a perceptual pass",
    )
    xfb_diff.set_defaults(func=cmd_xfb_diff)

    args = parser.parse_args()
    try:
        return args.func(args)
    except (OSError, ProtocolError, subprocess.SubprocessError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
