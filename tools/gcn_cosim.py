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
import shutil
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


def host_path(value: str | os.PathLike[str]) -> str:
    path = str(value)
    if re.match(r"^[A-Za-z]:[\\/]", path) or path.startswith("\\\\"):
        return path
    if sys.platform.startswith("cygwin"):
        path = os.path.abspath(path.replace("\\", "/"))
        try:
            return subprocess.check_output(
                ["cygpath", "-w", path], text=True
            ).strip()
        except (OSError, subprocess.SubprocessError):
            pass
    return str(pathlib.Path(path).resolve())


def local_path(value: str | os.PathLike[str]) -> str:
    path = str(value)
    if sys.platform.startswith("cygwin"):
        if re.match(r"^[A-Za-z]:[\\/]", path) or path.startswith("\\\\"):
            try:
                return subprocess.check_output(
                    ["cygpath", "-u", path], text=True
                ).strip()
            except (OSError, subprocess.SubprocessError):
                pass
        return os.path.abspath(path.replace("\\", "/"))
    return str(pathlib.Path(path).resolve())


def parse_pad_pulse(spec: str) -> dict[str, int]:
    parts = spec.split(":")
    if len(parts) not in (2, 3, 5):
        raise argparse.ArgumentTypeError(
            "pad pulse must be PUB:BUTTONS[:POLLS[:STICK_X:STICK_Y]]"
        )
    try:
        pulse = {
            "pub": int(parts[0], 0),
            "buttons": int(parts[1], 0),
            "polls": int(parts[2], 0) if len(parts) >= 3 else 30,
            "stick_x": int(parts[3], 0) if len(parts) == 5 else 0x80,
            "stick_y": int(parts[4], 0) if len(parts) == 5 else 0x80,
        }
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    return pulse


class JsonTcp:
    def __init__(self, port: int, timeout: float = 10.0):
        self.port = port
        self.timeout = timeout

    def close(self) -> None:
        pass

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


def wait_json(
    port: int,
    process: subprocess.Popen[Any],
    timeout: float = 30.0,
    name: str = "runtime",
    request_timeout: float = 10.0,
) -> JsonTcp:
    deadline = time.monotonic() + timeout
    client = JsonTcp(port, timeout=min(5.0, request_timeout))
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ProtocolError(f"{name} exited early with code {process.returncode}")
        try:
            client.request("ping")
            client.timeout = request_timeout
            return client
        except (OSError, ProtocolError) as exc:
            last_error = exc
            time.sleep(0.05)
    raise ProtocolError(f"{name} port {port} did not become ready: {last_error}")


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


@dataclass
class DolphinIplInstance:
    process: subprocess.Popen[Any]
    client: JsonTcp
    stdout: Any
    stderr: Any

    def close(self, timeout: float = 3.0) -> None:
        try:
            if self.process.poll() is None:
                self.client.request("quit")
                self.process.wait(timeout=timeout)
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
            "GCN_DISC": host_path(args.disc),
            "GCN_DSP_ROM": host_path(args.dsp_rom),
            "GCN_DSP_COEF": host_path(args.dsp_coef),
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
        [local_path(args.exe), host_path(args.ipl)],
        cwd=os.path.dirname(local_path(args.exe)),
        env=env,
        stdout=stdout,
        stderr=stderr,
    )
    client = wait_json(port, process)
    wait_parked(client, 0)
    return RuntimeInstance(name, process, client, stdout, stderr)


def start_runtime_visual(args: argparse.Namespace) -> RuntimeInstance:
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if getattr(args, "snapshot_exit", False) and not getattr(args, "snapshot_save", None):
        raise ProtocolError("--snapshot-exit requires --snapshot-save")
    stdout = (output_dir / "runtime-capture.out.log").open("wb")
    stderr = (output_dir / "runtime-capture.err.log").open("wb")
    env = os.environ.copy()
    env.update(
        {
            "GCN_DISC": host_path(args.disc),
            "GCN_DSP_ROM": host_path(args.dsp_rom),
            "GCN_DSP_COEF": host_path(args.dsp_coef),
            "GCN_DEBUG_PORT": str(args.port),
            "GCN_CYCLES_DERIVED": "1",
            "GCN_GX_PIPELINE": "0" if args.sync_gx else "1",
            "GCN_GX_THREADS": "1",
            "GCN_WINDOW": "0",
            "GCN_AUDIO": "0",
            "GCN_THROTTLE": "0",
            "GCN_NATIVE_MISS_JOURNAL": str(output_dir / "runtime-capture-misses.jsonl"),
            "GCN_DI_JOURNAL": str(output_dir / "runtime-capture-di.jsonl"),
        }
    )
    if args.backend:
        env["GCN_GX_BACKEND"] = args.backend
    if args.draw_state:
        env["GCN_GX_DRAW_STATE"] = "1"
        if args.draw_min_y is not None:
            env["GCN_GX_DRAW_MIN_Y"] = str(args.draw_min_y)
    if args.tev_census:
        env["GCN_GX_TEV_CENSUS"] = "1"
    if args.skip_draw_pc:
        env["GCN_GX_SKIP_DRAW_PC"] = args.skip_draw_pc
    if args.gpr_probe_pc:
        env["GCN_PROBE_PCS"] = ",".join(f"0x{pc:08X}" for pc in args.gpr_probe_pc)
    if getattr(args, "gpr_probe_inline_memory", None):
        env["GCN_PROBE_MEM"] = ",".join(args.gpr_probe_inline_memory)
    if getattr(args, "watch_range", None):
        env["GCN_WATCH"] = args.watch_range
    if getattr(args, "checkpoint_pc", None) is not None:
        env["GCN_CHECKPOINT_PC"] = f"0x{args.checkpoint_pc:08X}"
        if getattr(args, "checkpoint_gpr", None) is not None:
            env["GCN_CHECKPOINT_GPR"] = str(args.checkpoint_gpr)
        if getattr(args, "checkpoint_gpr_value", None) is not None:
            env["GCN_CHECKPOINT_GPR_VALUE"] = f"0x{args.checkpoint_gpr_value:08X}"
        if getattr(args, "checkpoint_lr", None) is not None:
            env["GCN_CHECKPOINT_LR"] = f"0x{args.checkpoint_lr:08X}"
    if getattr(args, "snapshot_save", None):
        env["GCN_SNAPSHOT_SAVE"] = host_path(args.snapshot_save)
    if getattr(args, "snapshot_exit", False):
        env["GCN_SNAPSHOT_EXIT"] = "1"
    if args.efb_copy_dump:
        env["GCN_GX_EFB_COPY_DUMP"] = host_path(output_dir)
        env["GCN_GX_EFB_COPY_DUMP_EVERY"] = str(args.efb_copy_dump_every)
    if args.efb_source_dump:
        env["GCN_GX_EFB_SOURCE_DUMP"] = host_path(output_dir)
        env["GCN_GX_EFB_SOURCE_DUMP_EVERY"] = str(args.efb_source_dump_every)
        if args.efb_source_dump_addr:
            env["GCN_GX_EFB_SOURCE_DUMP_ADDR"] = args.efb_source_dump_addr
    if args.snapshot_load:
        env["GCN_SNAPSHOT_LOAD"] = host_path(args.snapshot_load)
    if getattr(args, "no_memcard_a", False):
        env["GCN_MEMCARD_A_NONE"] = "1"
    if args.boot_mode == "bs1":
        env["GCN_BOOT_BS1"] = "1"
    else:
        env.pop("GCN_BOOT_BS1", None)

    process = subprocess.Popen(
        [local_path(args.exe), host_path(args.ipl), str(args.max_blocks)],
        cwd=local_path(output_dir),
        env=env,
        stdout=stdout,
        stderr=stderr,
    )
    client = wait_json(
        args.port,
        process,
        timeout=args.tcp_ready_timeout,
        name="runtime",
        request_timeout=args.request_timeout,
    )
    return RuntimeInstance("runtime-capture", process, client, stdout, stderr)


def cmd_runtime_capture(args: argparse.Namespace) -> int:
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    poll_path = output_dir / "runtime-capture.poll.jsonl"
    if "/" in args.screenshot_name or "\\" in args.screenshot_name:
        raise ProtocolError("--screenshot-name must be a file name, not a path")
    ppm_path = output_dir / args.screenshot_name
    if ppm_path.suffix.lower() != ".ppm":
        raise ProtocolError("--screenshot-name must end in .ppm")

    runtime = start_runtime_visual(args)
    try:
        deadline = time.monotonic() + args.wait_timeout
        last_pub = 0
        last_generation = 0
        final_screenshot: dict[str, Any] | None = None
        snapshot_exit_path = (
            pathlib.Path(args.snapshot_save)
            if getattr(args, "snapshot_exit", False)
            and getattr(args, "snapshot_save", None)
            else None
        )
        with poll_path.open("w", encoding="utf-8") as poll:
            for i in range(args.max_polls):
                item: dict[str, Any] = {
                    "time": time.time(),
                    "i": i,
                    "process_exited": runtime.process.poll() is not None,
                }
                try:
                    item["ping"] = runtime.client.request("ping")
                except (OSError, ProtocolError) as exc:
                    item["ping_error"] = str(exc)
                try:
                    xfb = runtime.client.request("xfb_pub_count")
                    item["xfb"] = xfb
                    last_pub = int(xfb.get("pub_count", 0))
                    last_generation = int(xfb.get("generation", 0))
                except (OSError, ProtocolError) as exc:
                    item["xfb_error"] = str(exc)

                if item["process_exited"] and snapshot_exit_path is not None:
                    item["snapshot_exit"] = {
                        "path": str(snapshot_exit_path),
                        "exists": snapshot_exit_path.exists(),
                    }
                    poll.write(json.dumps(item, separators=(",", ":")) + "\n")
                    poll.flush()
                    if not snapshot_exit_path.exists():
                        raise ProtocolError(
                            "runtime exited before writing requested snapshot: "
                            f"{snapshot_exit_path}"
                        )
                    summary = {
                        "ok": True,
                        "output_dir": str(output_dir),
                        "poll_path": str(poll_path),
                        "snapshot_exit": True,
                        "snapshot_path": str(snapshot_exit_path),
                        "target_pub": args.target_pub,
                        "last_pub_count": last_pub,
                        "last_generation": last_generation,
                    }
                    summary_path = output_dir / "runtime-capture.summary.json"
                    summary_path.write_text(
                        json.dumps(summary, indent=2), encoding="utf-8"
                    )
                    summary["summary_path"] = str(summary_path)
                    print(json.dumps(summary, separators=(",", ":")))
                    return 0

                if last_pub >= args.target_pub and i % args.screenshot_every == 0:
                    try:
                        screenshot = runtime.client.request(
                            "screenshot", path=args.screenshot_name
                        )
                        item["screenshot"] = screenshot
                        height = int(screenshot.get("height", 0))
                        mean_luma = float(screenshot.get("mean_luma", 0.0))
                        if height >= args.min_height and mean_luma >= args.min_luma:
                            final_screenshot = screenshot
                            if args.draw_state:
                                try:
                                    item["gx_draw_state"] = runtime.client.request(
                                        "gx_draw_state"
                                    )
                                except (OSError, ProtocolError) as exc:
                                    item["gx_draw_state_error"] = str(exc)
                            if args.pc_seen:
                                item["pc_seen"] = []
                                for pc in args.pc_seen:
                                    try:
                                        item["pc_seen"].append(
                                            runtime.client.request("pc_seen", pc=pc)
                                        )
                                    except (OSError, ProtocolError) as exc:
                                        item["pc_seen"].append(
                                            {
                                                "ok": False,
                                                "pc": pc,
                                                "error": str(exc),
                                            }
                                        )
                            if args.block_dump_count:
                                try:
                                    item["block_dump"] = runtime.client.request(
                                        "block_dump", count=args.block_dump_count
                                    )
                                except (OSError, ProtocolError) as exc:
                                    item["block_dump_error"] = str(exc)
                            if args.fifo_dump_count:
                                try:
                                    item["fifo_dump"] = runtime.client.request(
                                        "fifo_dump", count=args.fifo_dump_count
                                    )
                                except (OSError, ProtocolError) as exc:
                                    item["fifo_dump_error"] = str(exc)
                            if getattr(args, "watch_dump_count", 0):
                                try:
                                    item["watch_dump"] = runtime.client.request(
                                        "watch_dump", count=args.watch_dump_count
                                    )
                                except (OSError, ProtocolError) as exc:
                                    item["watch_dump_error"] = str(exc)
                            if args.gpr_probe_dump_count:
                                try:
                                    item["gpr_probe_dump"] = runtime.client.request(
                                        "gpr_probe_dump",
                                        count=args.gpr_probe_dump_count,
                                    )
                                    if (
                                        args.gpr_probe_memory
                                        or args.gpr_probe_memory_deref
                                    ):
                                        item["gpr_probe_memory"] = sample_gpr_probe_memory(
                                            runtime.client,
                                            item["gpr_probe_dump"],
                                            args.gpr_probe_memory,
                                            "runtime",
                                            args.gpr_probe_memory_deref,
                                        )
                                except (OSError, ProtocolError) as exc:
                                    item["gpr_probe_dump_error"] = str(exc)
                    except (OSError, ProtocolError) as exc:
                        item["screenshot_error"] = str(exc)

                poll.write(json.dumps(item, separators=(",", ":")) + "\n")
                poll.flush()
                if final_screenshot is not None:
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        "runtime capture did not produce a qualifying "
                        f"screenshot within {args.wait_timeout:g}s "
                        f"(last_pub_count={last_pub}, generation={last_generation})"
                    )
                time.sleep(args.poll_interval)

        if final_screenshot is None:
            raise ProtocolError("runtime capture did not produce a qualifying screenshot")

        draw_state_path: pathlib.Path | None = None
        if args.draw_state:
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "gx_draw_state" in item:
                        draw_state_path = output_dir / "runtime-capture.draw-state.json"
                        draw_state_path.write_text(
                            json.dumps(item["gx_draw_state"], indent=2),
                            encoding="utf-8",
                        )

        pc_seen_path: pathlib.Path | None = None
        if args.pc_seen:
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "pc_seen" in item:
                        pc_seen_path = output_dir / "runtime-capture.pc-seen.json"
                        pc_seen_path.write_text(
                            json.dumps(item["pc_seen"], indent=2),
                            encoding="utf-8",
                        )

        block_dump_path: pathlib.Path | None = None
        if args.block_dump_count:
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "block_dump" in item:
                        block_dump_path = output_dir / "runtime-capture.block-dump.json"
                        block_dump_path.write_text(
                            json.dumps(item["block_dump"], indent=2),
                            encoding="utf-8",
                        )

        fifo_dump_path: pathlib.Path | None = None
        if args.fifo_dump_count:
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "fifo_dump" in item:
                        fifo_dump_path = output_dir / "runtime-capture.fifo-dump.json"
                        fifo_dump_path.write_text(
                            json.dumps(item["fifo_dump"], indent=2),
                            encoding="utf-8",
                        )

        gpr_probe_dump_path: pathlib.Path | None = None
        gpr_probe_memory_path: pathlib.Path | None = None
        if args.gpr_probe_dump_count:
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "gpr_probe_dump" in item:
                        gpr_probe_dump_path = output_dir / "runtime-capture.gpr-probe.json"
                        gpr_probe_dump_path.write_text(
                            json.dumps(item["gpr_probe_dump"], indent=2),
                            encoding="utf-8",
                        )
                    if "gpr_probe_memory" in item:
                        gpr_probe_memory_path = (
                            output_dir / "runtime-capture.gpr-probe-memory.json"
                        )
                        gpr_probe_memory_path.write_text(
                            json.dumps(item["gpr_probe_memory"], indent=2),
                            encoding="utf-8",
                        )

        png_path: pathlib.Path | None = None
        if args.convert_png:
            png_path = ppm_path.with_suffix(".png")
            subprocess.run(
                [args.magick, str(ppm_path), str(png_path)],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

        summary = {
            "ok": True,
            "output_dir": str(output_dir),
            "poll_path": str(poll_path),
            "screenshot": final_screenshot,
            "ppm_path": str(ppm_path),
            "png_path": None if png_path is None else str(png_path),
            "draw_state_path": None if draw_state_path is None else str(draw_state_path),
            "pc_seen_path": None if pc_seen_path is None else str(pc_seen_path),
            "block_dump_path": None if block_dump_path is None else str(block_dump_path),
            "fifo_dump_path": None if fifo_dump_path is None else str(fifo_dump_path),
            "gpr_probe_dump_path": (
                None if gpr_probe_dump_path is None else str(gpr_probe_dump_path)
            ),
            "gpr_probe_memory_path": (
                None if gpr_probe_memory_path is None else str(gpr_probe_memory_path)
            ),
            "target_pub": args.target_pub,
            "last_pub_count": last_pub,
            "last_generation": last_generation,
        }
        print(json.dumps(summary, separators=(",", ":")))
        return 0
    finally:
        runtime.close()


def cmd_runtime_pub_sweep(args: argparse.Namespace) -> int:
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    poll_path = output_dir / "runtime-pub-sweep.poll.jsonl"
    if "/" in args.screenshot_prefix or "\\" in args.screenshot_prefix:
        raise ProtocolError("--screenshot-prefix must be a file prefix, not a path")

    targets = sorted(set(args.sample_pub))
    if not targets:
        raise ProtocolError("at least one --sample-pub is required")
    pending = list(targets)
    samples: list[dict[str, Any]] = []

    runtime = start_runtime_visual(args)
    try:
        deadline = time.monotonic() + args.wait_timeout
        last_pub = 0
        last_generation = 0
        snapshot_exit_path = (
            pathlib.Path(args.snapshot_save)
            if getattr(args, "snapshot_exit", False)
            and getattr(args, "snapshot_save", None)
            else None
        )
        with poll_path.open("w", encoding="utf-8") as poll:
            for i in range(args.max_polls):
                item: dict[str, Any] = {
                    "time": time.time(),
                    "i": i,
                    "process_exited": runtime.process.poll() is not None,
                }
                try:
                    item["ping"] = runtime.client.request("ping")
                except (OSError, ProtocolError) as exc:
                    item["ping_error"] = str(exc)
                try:
                    xfb = runtime.client.request("xfb_pub_count")
                    item["xfb"] = xfb
                    last_pub = int(xfb.get("pub_count", 0))
                    last_generation = int(xfb.get("generation", 0))
                except (OSError, ProtocolError) as exc:
                    item["xfb_error"] = str(exc)

                if item["process_exited"] and snapshot_exit_path is not None:
                    item["snapshot_exit"] = {
                        "path": str(snapshot_exit_path),
                        "exists": snapshot_exit_path.exists(),
                    }
                    poll.write(json.dumps(item, separators=(",", ":")) + "\n")
                    poll.flush()
                    if not snapshot_exit_path.exists():
                        raise ProtocolError(
                            "runtime exited before writing requested snapshot: "
                            f"{snapshot_exit_path}"
                        )
                    summary = {
                        "ok": True,
                        "output_dir": str(output_dir),
                        "poll_path": str(poll_path),
                        "snapshot_exit": True,
                        "snapshot_path": str(snapshot_exit_path),
                        "targets": targets,
                        "samples": samples,
                        "last_pub_count": last_pub,
                        "last_generation": last_generation,
                    }
                    summary_path = output_dir / "runtime-pub-sweep.summary.json"
                    summary_path.write_text(
                        json.dumps(summary, indent=2), encoding="utf-8"
                    )
                    summary["summary_path"] = str(summary_path)
                    print(json.dumps(summary, separators=(",", ":")))
                    return 0

                item["samples"] = []
                while pending and last_pub >= pending[0] and i % args.screenshot_every == 0:
                    target = pending.pop(0)
                    stem = f"{args.screenshot_prefix}-pub{target:04d}"
                    ppm_name = f"{stem}.ppm"
                    sample: dict[str, Any] = {
                        "target_pub": target,
                        "observed_pub_count": last_pub,
                        "observed_generation": last_generation,
                        "ppm_path": str(output_dir / ppm_name),
                    }
                    pre_screenshot_draw_state: dict[str, Any] | None = None
                    pre_screenshot_draw_state_error: str | None = None
                    if args.draw_state:
                        try:
                            pre_screenshot_draw_state = runtime.client.request(
                                "gx_draw_state"
                            )
                        except (OSError, ProtocolError) as exc:
                            pre_screenshot_draw_state_error = str(exc)
                    try:
                        screenshot = runtime.client.request("screenshot", path=ppm_name)
                        sample["screenshot"] = screenshot
                        height = int(screenshot.get("height", 0))
                        mean_luma = float(screenshot.get("mean_luma", 0.0))
                        if height < args.min_height or mean_luma < args.min_luma:
                            sample["accepted"] = False
                            sample["reject_reason"] = (
                                f"height={height} mean_luma={mean_luma:g}"
                            )
                        else:
                            sample["accepted"] = True
                            if getattr(args, "ram_dump", None) or getattr(
                                args, "ram_dump_deref", None
                            ):
                                try:
                                    ram_dump = sample_ram_dumps(
                                        runtime.client,
                                        args.ram_dump,
                                        "runtime",
                                        getattr(args, "ram_dump_deref", []),
                                    )
                                    ram_dump_path = output_dir / f"{stem}.ram-dump.json"
                                    ram_dump_path.write_text(
                                        json.dumps(ram_dump, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["ram_dump_path"] = str(ram_dump_path)
                                except (OSError, ProtocolError) as exc:
                                    sample["ram_dump_error"] = str(exc)
                            if args.draw_state:
                                try:
                                    draw_state = pre_screenshot_draw_state
                                    if draw_state is None:
                                        if pre_screenshot_draw_state_error is not None:
                                            raise ProtocolError(
                                                pre_screenshot_draw_state_error
                                            )
                                        draw_state = runtime.client.request(
                                            "gx_draw_state"
                                        )
                                    draw_state_path = output_dir / f"{stem}.draw-state.json"
                                    draw_state_path.write_text(
                                        json.dumps(draw_state, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["draw_state_path"] = str(draw_state_path)
                                except (OSError, ProtocolError) as exc:
                                    sample["draw_state_error"] = str(exc)
                            if args.pc_seen:
                                pc_seen = []
                                for pc in args.pc_seen:
                                    try:
                                        pc_seen.append(runtime.client.request("pc_seen", pc=pc))
                                    except (OSError, ProtocolError) as exc:
                                        pc_seen.append(
                                            {"ok": False, "pc": pc, "error": str(exc)}
                                        )
                                pc_seen_path = output_dir / f"{stem}.pc-seen.json"
                                pc_seen_path.write_text(
                                    json.dumps(pc_seen, indent=2),
                                    encoding="utf-8",
                                )
                                sample["pc_seen_path"] = str(pc_seen_path)
                            if args.block_dump_count:
                                try:
                                    block_dump = runtime.client.request(
                                        "block_dump", count=args.block_dump_count
                                    )
                                    block_dump_path = output_dir / f"{stem}.block-dump.json"
                                    block_dump_path.write_text(
                                        json.dumps(block_dump, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["block_dump_path"] = str(block_dump_path)
                                except (OSError, ProtocolError) as exc:
                                    sample["block_dump_error"] = str(exc)
                            if args.fifo_dump_count:
                                try:
                                    fifo_dump = runtime.client.request(
                                        "fifo_dump", count=args.fifo_dump_count
                                    )
                                    fifo_dump_path = output_dir / f"{stem}.fifo-dump.json"
                                    fifo_dump_path.write_text(
                                        json.dumps(fifo_dump, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["fifo_dump_path"] = str(fifo_dump_path)
                                except (OSError, ProtocolError) as exc:
                                    sample["fifo_dump_error"] = str(exc)
                            if getattr(args, "watch_dump_count", 0):
                                try:
                                    watch_dump = runtime.client.request(
                                        "watch_dump", count=args.watch_dump_count
                                    )
                                    watch_dump_path = output_dir / f"{stem}.watch-dump.json"
                                    watch_dump_path.write_text(
                                        json.dumps(watch_dump, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["watch_dump_path"] = str(watch_dump_path)
                                    sample["watch_dump_stderr_path"] = str(
                                        output_dir / "runtime-capture.err.log"
                                    )
                                except (OSError, ProtocolError) as exc:
                                    sample["watch_dump_error"] = str(exc)
                            if args.gpr_probe_dump_count:
                                try:
                                    gpr_probe_dump = runtime.client.request(
                                        "gpr_probe_dump",
                                        count=args.gpr_probe_dump_count,
                                    )
                                    gpr_probe_path = output_dir / f"{stem}.gpr-probe.json"
                                    gpr_probe_path.write_text(
                                        json.dumps(gpr_probe_dump, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["gpr_probe_dump_path"] = str(gpr_probe_path)
                                    if (
                                        args.gpr_probe_memory
                                        or args.gpr_probe_memory_deref
                                    ):
                                        gpr_probe_memory = sample_gpr_probe_memory(
                                            runtime.client,
                                            gpr_probe_dump,
                                            args.gpr_probe_memory,
                                            "runtime",
                                            args.gpr_probe_memory_deref,
                                        )
                                        gpr_probe_memory_path = (
                                            output_dir / f"{stem}.gpr-probe-memory.json"
                                        )
                                        gpr_probe_memory_path.write_text(
                                            json.dumps(gpr_probe_memory, indent=2),
                                            encoding="utf-8",
                                        )
                                        sample["gpr_probe_memory_path"] = str(
                                            gpr_probe_memory_path
                                        )
                                except (OSError, ProtocolError) as exc:
                                    sample["gpr_probe_dump_error"] = str(exc)
                            if args.convert_png:
                                png_path = output_dir / f"{stem}.png"
                                subprocess.run(
                                    [args.magick, str(output_dir / ppm_name), str(png_path)],
                                    check=True,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL,
                                )
                                sample["png_path"] = str(png_path)
                    except (OSError, ProtocolError, subprocess.SubprocessError) as exc:
                        sample["accepted"] = False
                        sample["error"] = str(exc)
                    samples.append(sample)
                    item["samples"].append(sample)

                poll.write(json.dumps(item, separators=(",", ":")) + "\n")
                poll.flush()
                if not pending:
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        "runtime publication sweep did not reach all targets within "
                        f"{args.wait_timeout:g}s (last_pub_count={last_pub}, "
                        f"generation={last_generation}, pending={pending})"
                    )
                time.sleep(args.poll_interval)

        if pending:
            raise ProtocolError(f"runtime publication sweep missing targets: {pending}")

        summary = {
            "ok": True,
            "output_dir": str(output_dir),
            "poll_path": str(poll_path),
            "targets": targets,
            "samples": samples,
            "last_pub_count": last_pub,
            "last_generation": last_generation,
        }
        summary_path = output_dir / "runtime-pub-sweep.summary.json"
        summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        summary["summary_path"] = str(summary_path)
        print(json.dumps(summary, separators=(",", ":")))
        return 0
    finally:
        runtime.close()


def dolphin_region_dir(region: str) -> str:
    if region in ("NTSC_U", "USA"):
        return "USA"
    if region in ("NTSC_J", "JAP", "JPN"):
        return "JAP"
    if region in ("PAL", "EUR"):
        return "EUR"
    raise ProtocolError(f"unknown GameCube IPL region {region!r}")


def start_dolphin_ipl_oracle(args: argparse.Namespace) -> DolphinIplInstance:
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    user_dir = output_dir / "dolphin-user"
    gc_dir = user_dir / "GC"
    region_dir = gc_dir / dolphin_region_dir(args.region)
    region_dir.mkdir(parents=True, exist_ok=True)
    gc_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(local_path(args.ipl), region_dir / "IPL.bin")
    shutil.copy2(local_path(args.dsp_rom), gc_dir / "dsp_rom.bin")
    shutil.copy2(local_path(args.dsp_coef), gc_dir / "dsp_coef.bin")

    stdout = (output_dir / "dolphin-ipl.out.log").open("wb")
    stderr = (output_dir / "dolphin-ipl.err.log").open("wb")
    env = os.environ.copy()
    env["GCN_TRACE_TCP_PORT"] = str(args.dolphin_port)
    if args.efb_copy_dump:
        env["GCN_TRACE_EFB_COPY_DUMP"] = host_path(output_dir)
        env["GCN_TRACE_EFB_COPY_DUMP_EVERY"] = str(args.efb_copy_dump_every)
    if args.efb_source_dump:
        env["GCN_TRACE_EFB_SOURCE_DUMP"] = host_path(output_dir)
        env["GCN_TRACE_EFB_SOURCE_DUMP_EVERY"] = str(args.efb_source_dump_every)
        if args.efb_source_dump_addr:
            env["GCN_TRACE_EFB_SOURCE_DUMP_ADDR"] = args.efb_source_dump_addr
    if args.sw_draw_state:
        env["GCN_TRACE_SW_DRAW_STATE"] = "1"
        env["GCN_TRACE_SW_DRAW_MIN_AREA"] = str(args.sw_draw_min_area)
        env["GCN_TRACE_SW_DRAW_MIN_Y"] = str(args.sw_draw_min_y)
    if getattr(args, "xf_context_state", False):
        env["GCN_TRACE_XF_CONTEXT_STATE"] = "1"
    if args.gpr_probe_pc:
        env["GCN_TRACE_PROBE_PCS"] = ",".join(f"0x{pc:08X}" for pc in args.gpr_probe_pc)
    if getattr(args, "gpr_probe_inline_memory", None):
        env["GCN_TRACE_PROBE_MEM"] = ",".join(args.gpr_probe_inline_memory)
    command = [
        local_path(args.dolphin_exe),
        "-u",
        host_path(user_dir),
        "--video_backend",
        args.video_backend,
    ]
    if args.cpu_core is not None:
        command.extend(["--config", f"Dolphin.Core.CPUCore={args.cpu_core}"])
    if args.no_memcard_a:
        command.extend(["--config", "Dolphin.Core.SlotA=255"])
    command.extend([
        f"--boot-gc-ipl={args.region}",
        f"--ipl-disc={host_path(args.disc)}",
    ])
    process = subprocess.Popen(
        command,
        cwd=local_path(output_dir),
        env=env,
        stdout=stdout,
        stderr=stderr,
    )
    client = wait_json(
        args.dolphin_port,
        process,
        timeout=args.tcp_ready_timeout,
        name="Dolphin",
        request_timeout=args.request_timeout,
    )
    return DolphinIplInstance(process, client, stdout, stderr)


def cmd_dolphin_ipl_capture(args: argparse.Namespace) -> int:
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    poll_path = output_dir / "dolphin-ipl-capture.poll.jsonl"
    if "/" in args.screenshot_name or "\\" in args.screenshot_name:
        raise ProtocolError("--screenshot-name must be a file name, not a path")
    ppm_path = output_dir / args.screenshot_name
    if ppm_path.suffix.lower() != ".ppm":
        raise ProtocolError("--screenshot-name must end in .ppm")

    dolphin = start_dolphin_ipl_oracle(args)
    summary: dict[str, Any] = {}
    try:
        deadline = time.monotonic() + args.wait_timeout
        last_pub = 0
        final_screenshot: dict[str, Any] | None = None
        pad_pulses = list(args.pad_pulse)
        if args.pad_pulse_pub is not None:
            pad_pulses.append(
                {
                    "pub": args.pad_pulse_pub,
                    "buttons": args.pad_pulse_buttons,
                    "polls": args.pad_pulse_polls,
                    "stick_x": args.pad_pulse_stick_x,
                    "stick_y": args.pad_pulse_stick_y,
                }
            )
        pad_pulses.sort(key=lambda pulse: pulse["pub"])
        pad_pulse_active = False
        pad_pulse_index = 0
        pad_pulse_polls_left = 0
        with poll_path.open("w", encoding="utf-8") as poll:
            for i in range(args.max_polls):
                item: dict[str, Any] = {
                    "time": time.time(),
                    "i": i,
                    "process_exited": dolphin.process.poll() is not None,
                }
                try:
                    item["boot"] = dolphin.client.request("boot_state")
                except (OSError, ProtocolError) as exc:
                    item["boot_error"] = str(exc)
                try:
                    xfb = dolphin.client.request("xfb_pub_count")
                    item["xfb"] = xfb
                    last_pub = int(xfb.get("pub_count", 0))
                except (OSError, ProtocolError) as exc:
                    item["xfb_error"] = str(exc)

                if pad_pulse_index < len(pad_pulses):
                    pulse = pad_pulses[pad_pulse_index]
                    if not pad_pulse_active and last_pub >= pulse["pub"]:
                        try:
                            item["pad_pulse_press"] = dolphin.client.request(
                                "set_input",
                                buttons=pulse["buttons"],
                                stick_x=pulse["stick_x"],
                                stick_y=pulse["stick_y"],
                            )
                            item["pad_pulse_index"] = pad_pulse_index
                            pad_pulse_active = True
                            pad_pulse_polls_left = max(1, pulse["polls"])
                        except (OSError, ProtocolError) as exc:
                            item["pad_pulse_error"] = str(exc)
                            pad_pulse_index += 1
                    elif pad_pulse_active:
                        pad_pulse_polls_left -= 1
                        if pad_pulse_polls_left <= 0:
                            try:
                                item["pad_pulse_release"] = dolphin.client.request(
                                    "set_input", reset=1
                                )
                            except (OSError, ProtocolError) as exc:
                                item["pad_pulse_release_error"] = str(exc)
                            pad_pulse_active = False
                            pad_pulse_index += 1

                should_try_screenshot = False
                if args.target_pub is not None and last_pub >= args.target_pub:
                    should_try_screenshot = True
                elif args.target_pub is None and last_pub > 0:
                    should_try_screenshot = i % args.screenshot_every == 0

                if should_try_screenshot:
                    try:
                        screenshot = dolphin.client.request(
                            "screenshot_ppm", path=args.screenshot_name
                        )
                        item["screenshot"] = screenshot
                        height = int(screenshot.get("height", 0))
                        mean_luma = float(screenshot.get("mean_luma", 0.0))
                        pub_seq = int(screenshot.get("pub_seq", 0))
                        if (
                            height >= args.min_height
                            and mean_luma >= args.min_luma
                            and pub_seq >= args.min_screenshot_pub
                        ):
                            final_screenshot = screenshot
                            if getattr(args, "ram_dump", None) or getattr(
                                args, "ram_dump_deref", None
                            ):
                                try:
                                    item["ram_dump"] = sample_ram_dumps(
                                        dolphin.client,
                                        args.ram_dump,
                                        "Dolphin",
                                        getattr(args, "ram_dump_deref", []),
                                    )
                                except (OSError, ProtocolError) as exc:
                                    item["ram_dump_error"] = str(exc)
                            if args.sw_draw_state:
                                try:
                                    item["sw_draw_state"] = dolphin.client.request(
                                        "sw_draw_state"
                                    )
                                except (OSError, ProtocolError) as exc:
                                    item["sw_draw_state_error"] = str(exc)
                            if getattr(args, "xf_context_state", False):
                                try:
                                    item["xf_context_state"] = dolphin.client.request(
                                        "xf_context_state",
                                        count=args.xf_context_count,
                                    )
                                except (OSError, ProtocolError) as exc:
                                    item["xf_context_state_error"] = str(exc)
                            if args.gpr_probe_dump_count:
                                try:
                                    item["gpr_probe_dump"] = dolphin.client.request(
                                        "gpr_probe_dump",
                                        count=args.gpr_probe_dump_count,
                                    )
                                    if (
                                        args.gpr_probe_memory
                                        or args.gpr_probe_memory_deref
                                    ):
                                        item["gpr_probe_memory"] = sample_gpr_probe_memory(
                                            dolphin.client,
                                            item["gpr_probe_dump"],
                                            args.gpr_probe_memory,
                                            "Dolphin",
                                            args.gpr_probe_memory_deref,
                                        )
                                except (OSError, ProtocolError) as exc:
                                    item["gpr_probe_dump_error"] = str(exc)
                    except (OSError, ProtocolError) as exc:
                        item["screenshot_error"] = str(exc)

                poll.write(json.dumps(item, separators=(",", ":")) + "\n")
                poll.flush()
                if final_screenshot is not None:
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        "Dolphin IPL capture did not produce a qualifying "
                        f"screenshot within {args.wait_timeout:g}s "
                        f"(last_pub_count={last_pub})"
                    )
                time.sleep(args.poll_interval)

        if final_screenshot is None:
            raise ProtocolError("Dolphin IPL capture did not produce a qualifying screenshot")

        sw_draw_state_path: pathlib.Path | None = None
        xf_context_state_path: pathlib.Path | None = None
        gpr_probe_dump_path: pathlib.Path | None = None
        gpr_probe_memory_path: pathlib.Path | None = None
        ram_dump_path: pathlib.Path | None = None
        if getattr(args, "ram_dump", None) or getattr(args, "ram_dump_deref", None):
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "ram_dump" in item:
                        ram_dump_path = output_dir / "dolphin-ipl-capture.ram-dump.json"
                        ram_dump_path.write_text(
                            json.dumps(item["ram_dump"], indent=2),
                            encoding="utf-8",
                        )
        if args.sw_draw_state:
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "sw_draw_state" in item:
                        sw_draw_state_path = output_dir / "dolphin-ipl-capture.sw-draw-state.json"
                        sw_draw_state_path.write_text(
                            json.dumps(item["sw_draw_state"], indent=2),
                            encoding="utf-8",
                        )
        if getattr(args, "xf_context_state", False):
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "xf_context_state" in item:
                        xf_context_state_path = (
                            output_dir / "dolphin-ipl-capture.xf-context-state.json"
                        )
                        xf_context_state_path.write_text(
                            json.dumps(item["xf_context_state"], indent=2),
                            encoding="utf-8",
                        )
        if args.gpr_probe_dump_count:
            with poll_path.open("r", encoding="utf-8") as poll:
                for line in poll:
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if "gpr_probe_dump" in item:
                        gpr_probe_dump_path = output_dir / "dolphin-ipl-capture.gpr-probe.json"
                        gpr_probe_dump_path.write_text(
                            json.dumps(item["gpr_probe_dump"], indent=2),
                            encoding="utf-8",
                        )
                    if "gpr_probe_memory" in item:
                        gpr_probe_memory_path = (
                            output_dir / "dolphin-ipl-capture.gpr-probe-memory.json"
                        )
                        gpr_probe_memory_path.write_text(
                            json.dumps(item["gpr_probe_memory"], indent=2),
                            encoding="utf-8",
                        )

        png_path: pathlib.Path | None = None
        if args.convert_png:
            png_path = ppm_path.with_suffix(".png")
            subprocess.run(
                [args.magick, str(ppm_path), str(png_path)],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

        summary = {
            "ok": True,
            "output_dir": str(output_dir),
            "poll_path": str(poll_path),
            "screenshot": final_screenshot,
            "ppm_path": str(ppm_path),
            "png_path": None if png_path is None else str(png_path),
            "sw_draw_state_path": None
            if sw_draw_state_path is None
            else str(sw_draw_state_path),
            "gpr_probe_dump_path": None
            if gpr_probe_dump_path is None
            else str(gpr_probe_dump_path),
            "xf_context_state_path": None
            if xf_context_state_path is None
            else str(xf_context_state_path),
            "gpr_probe_memory_path": None
            if gpr_probe_memory_path is None
            else str(gpr_probe_memory_path),
            "ram_dump_path": None if ram_dump_path is None else str(ram_dump_path),
            "target_pub": args.target_pub,
            "last_pub_count": last_pub,
        }
        print(json.dumps(summary, separators=(",", ":")))
        return 0
    finally:
        dolphin.close(timeout=args.quit_timeout)


def cmd_dolphin_ipl_sweep(args: argparse.Namespace) -> int:
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    poll_path = output_dir / "dolphin-ipl-sweep.poll.jsonl"
    if "/" in args.screenshot_prefix or "\\" in args.screenshot_prefix:
        raise ProtocolError("--screenshot-prefix must be a file prefix, not a path")

    targets = sorted(set(args.sample_pub))
    if not targets:
        raise ProtocolError("at least one --sample-pub is required")
    pending = list(targets)
    samples: list[dict[str, Any]] = []

    dolphin = start_dolphin_ipl_oracle(args)
    try:
        deadline = time.monotonic() + args.wait_timeout
        last_pub = 0
        with poll_path.open("w", encoding="utf-8") as poll:
            for i in range(args.max_polls):
                item: dict[str, Any] = {
                    "time": time.time(),
                    "i": i,
                    "process_exited": dolphin.process.poll() is not None,
                }
                try:
                    item["boot"] = dolphin.client.request("boot_state")
                except (OSError, ProtocolError) as exc:
                    item["boot_error"] = str(exc)
                try:
                    xfb = dolphin.client.request("xfb_pub_count")
                    item["xfb"] = xfb
                    last_pub = int(xfb.get("pub_count", 0))
                except (OSError, ProtocolError) as exc:
                    item["xfb_error"] = str(exc)

                item["samples"] = []
                while pending and last_pub >= pending[0] and i % args.screenshot_every == 0:
                    target = pending.pop(0)
                    stem = f"{args.screenshot_prefix}-pub{target:04d}"
                    ppm_name = f"{stem}.ppm"
                    sample: dict[str, Any] = {
                        "target_pub": target,
                        "observed_pub_count": last_pub,
                        "ppm_path": str(output_dir / ppm_name),
                    }
                    try:
                        screenshot = dolphin.client.request("screenshot_ppm", path=ppm_name)
                        sample["screenshot"] = screenshot
                        height = int(screenshot.get("height", 0))
                        mean_luma = float(screenshot.get("mean_luma", 0.0))
                        pub_seq = int(screenshot.get("pub_seq", 0))
                        if (
                            height < args.min_height
                            or mean_luma < args.min_luma
                            or pub_seq < args.min_screenshot_pub
                        ):
                            sample["accepted"] = False
                            sample["reject_reason"] = (
                                f"height={height} mean_luma={mean_luma:g} pub_seq={pub_seq}"
                            )
                        else:
                            sample["accepted"] = True
                            if getattr(args, "ram_dump", None) or getattr(
                                args, "ram_dump_deref", None
                            ):
                                try:
                                    ram_dump = sample_ram_dumps(
                                        dolphin.client,
                                        args.ram_dump,
                                        "Dolphin",
                                        getattr(args, "ram_dump_deref", []),
                                    )
                                    ram_dump_path = output_dir / f"{stem}.ram-dump.json"
                                    ram_dump_path.write_text(
                                        json.dumps(ram_dump, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["ram_dump_path"] = str(ram_dump_path)
                                except (OSError, ProtocolError) as exc:
                                    sample["ram_dump_error"] = str(exc)
                            if args.sw_draw_state:
                                try:
                                    sw_draw_state = dolphin.client.request("sw_draw_state")
                                    sw_draw_state_path = output_dir / f"{stem}.sw-draw-state.json"
                                    sw_draw_state_path.write_text(
                                        json.dumps(sw_draw_state, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["sw_draw_state_path"] = str(sw_draw_state_path)
                                except (OSError, ProtocolError) as exc:
                                    sample["sw_draw_state_error"] = str(exc)
                            if getattr(args, "xf_context_state", False):
                                try:
                                    xf_context_state = dolphin.client.request(
                                        "xf_context_state",
                                        count=args.xf_context_count,
                                    )
                                    xf_context_state_path = (
                                        output_dir / f"{stem}.xf-context-state.json"
                                    )
                                    xf_context_state_path.write_text(
                                        json.dumps(xf_context_state, indent=2),
                                        encoding="utf-8",
                                    )
                                    sample["xf_context_state_path"] = str(
                                        xf_context_state_path
                                    )
                                except (OSError, ProtocolError) as exc:
                                    sample["xf_context_state_error"] = str(exc)
                            if args.convert_png:
                                png_path = output_dir / f"{stem}.png"
                                subprocess.run(
                                    [args.magick, str(output_dir / ppm_name), str(png_path)],
                                    check=True,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL,
                                )
                                sample["png_path"] = str(png_path)
                    except (OSError, ProtocolError, subprocess.SubprocessError) as exc:
                        sample["accepted"] = False
                        sample["error"] = str(exc)
                    samples.append(sample)
                    item["samples"].append(sample)

                poll.write(json.dumps(item, separators=(",", ":")) + "\n")
                poll.flush()
                if not pending:
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        "Dolphin IPL sweep did not reach all targets within "
                        f"{args.wait_timeout:g}s (last_pub_count={last_pub}, "
                        f"pending={pending})"
                    )
                time.sleep(args.poll_interval)

        if pending:
            raise ProtocolError(f"Dolphin IPL sweep missing targets: {pending}")

        summary = {
            "ok": True,
            "output_dir": str(output_dir),
            "poll_path": str(poll_path),
            "targets": targets,
            "samples": samples,
            "last_pub_count": last_pub,
        }
        summary_path = output_dir / "dolphin-ipl-sweep.summary.json"
        summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        summary["summary_path"] = str(summary_path)
        print(json.dumps(summary, separators=(",", ":")))
        return 0
    finally:
        dolphin.close(timeout=args.quit_timeout)


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


def parse_ram_dump_spec(spec: str) -> tuple[int, int]:
    fields = spec.split(":")
    if len(fields) != 2:
        raise argparse.ArgumentTypeError("RAM dump must be ADDRESS:LENGTH")
    try:
        address, length = (int(field, 0) for field in fields)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 <= address <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("RAM dump address must fit in 32 bits")
    if length <= 0:
        raise argparse.ArgumentTypeError("RAM dump length must be positive")
    if length > 65536:
        raise argparse.ArgumentTypeError("RAM dump length must be <= 65536")
    return address, length


def parse_ram_dump_deref_spec(spec: str) -> tuple[int, int, int]:
    fields = spec.split(":")
    if len(fields) != 3:
        raise argparse.ArgumentTypeError(
            "RAM deref dump must be POINTER_ADDRESS:READ_OFFSET:LENGTH"
        )
    try:
        pointer_address, read_offset, length = (int(field, 0) for field in fields)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 <= pointer_address <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("pointer address must fit in 32 bits")
    if length <= 0:
        raise argparse.ArgumentTypeError("RAM deref dump length must be positive")
    if length > 65536:
        raise argparse.ArgumentTypeError("RAM deref dump length must be <= 65536")
    return pointer_address, read_offset, length


def parse_gpr_probe_memory(spec: str) -> tuple[int | None, int, int, int]:
    fields = spec.split(":")
    if len(fields) == 3:
        pc: int | None = None
        gpr_s, offset_s, length_s = fields
    elif len(fields) == 4:
        pc_s, gpr_s, offset_s, length_s = fields
        try:
            pc = int(pc_s, 0)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(str(exc)) from exc
        if not 0 <= pc <= 0xFFFFFFFF:
            raise argparse.ArgumentTypeError("PC must fit in 32 bits")
    else:
        raise argparse.ArgumentTypeError(
            "GPR probe memory must be GPR:OFFSET:LENGTH or PC:GPR:OFFSET:LENGTH"
        )
    try:
        gpr, offset, length = int(gpr_s, 0), int(offset_s, 0), int(length_s, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 <= gpr < 32:
        raise argparse.ArgumentTypeError("GPR must be in 0..31")
    if length <= 0:
        raise argparse.ArgumentTypeError("range length must be positive")
    if length > 4096:
        raise argparse.ArgumentTypeError("range length must be <= 4096")
    return pc, gpr, offset, length


def parse_gpr_probe_memory_deref(
    spec: str,
) -> tuple[int | None, int, int, int, int]:
    fields = spec.split(":")
    if len(fields) == 4:
        pc: int | None = None
        gpr_s, ptr_offset_s, read_offset_s, length_s = fields
    elif len(fields) == 5:
        pc_s, gpr_s, ptr_offset_s, read_offset_s, length_s = fields
        try:
            pc = int(pc_s, 0)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(str(exc)) from exc
        if not 0 <= pc <= 0xFFFFFFFF:
            raise argparse.ArgumentTypeError("PC must fit in 32 bits")
    else:
        raise argparse.ArgumentTypeError(
            "GPR probe deref memory must be "
            "GPR:PTR_OFFSET:READ_OFFSET:LENGTH or "
            "PC:GPR:PTR_OFFSET:READ_OFFSET:LENGTH"
        )
    try:
        gpr = int(gpr_s, 0)
        ptr_offset = int(ptr_offset_s, 0)
        read_offset = int(read_offset_s, 0)
        length = int(length_s, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not 0 <= gpr < 32:
        raise argparse.ArgumentTypeError("GPR must be in 0..31")
    if length <= 0:
        raise argparse.ArgumentTypeError("range length must be positive")
    if length > 4096:
        raise argparse.ArgumentTypeError("range length must be <= 4096")
    return pc, gpr, ptr_offset, read_offset, length


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


def tcp_memory(client: JsonTcp, address: int, length: int, name: str) -> bytes:
    """Read guest RAM over the runtime/Dolphin JSON TCP `read_ram` command.

    Both sides now expose the same shape for this diagnostic path: hex bytes,
    capped to 64 KiB per request. This is deliberately separate from the GDB
    memory helper below so attract-demo visual probes do not need Dolphin's GDB
    stub once the process is already running with GCN_TRACE_TCP_PORT.
    """
    data = bytearray()
    while len(data) < length:
        chunk_len = min(65536, length - len(data))
        reply = client.request("read_ram", addr=address + len(data), len=chunk_len)
        if not reply.get("ok", False):
            raise ProtocolError(f"{name} read_ram failed: {reply}")
        chunk = bytes.fromhex(reply["hex"])
        if len(chunk) != chunk_len:
            raise ProtocolError(
                f"{name} returned a short memory read at 0x{address + len(data):08X}: "
                f"{len(chunk)} != {chunk_len}"
            )
        data.extend(chunk)
    return bytes(data)


def guest_ram_range_name(address: int, length: int) -> str | None:
    if length <= 0:
        return None
    end = address + length
    if end > 0x100000000:
        return None
    ranges = (
        ("mem1", 0x80000000, 0x01800000),
        ("mem1_uncached", 0xC0000000, 0x01800000),
        ("mem2", 0x90000000, 0x04000000),
        ("mem2_uncached", 0xD0000000, 0x04000000),
        ("locked_l1", 0xE0000000, 0x00040000),
    )
    for range_name, base, size in ranges:
        if address >= base and end <= base + size:
            return range_name
    return None


def gpr_probe_records(dump: dict[str, Any]) -> list[dict[str, Any]]:
    records = dump.get("records")
    if isinstance(records, list):
        return records
    entries = dump.get("entries")
    if isinstance(entries, list):
        return entries
    return []


def sample_gpr_probe_memory(
    client: JsonTcp,
    gpr_probe_dump: dict[str, Any],
    specs: list[tuple[int | None, int, int, int]],
    name: str,
    deref_specs: list[tuple[int | None, int, int, int, int]] | None = None,
    max_unique: int = 1024,
) -> dict[str, Any]:
    records = gpr_probe_records(gpr_probe_dump)
    windows: list[dict[str, Any]] = []
    seen: set[tuple[Any, ...]] = set()
    errors = 0
    truncated = False
    deref_specs = [] if deref_specs is None else deref_specs

    def result() -> dict[str, Any]:
        return {
            "ok": errors == 0,
            "kind": "gpr_probe_memory",
            "source_records": len(records),
            "specs": [
                {
                    "pc": None if pc is None else f"0x{pc:08X}",
                    "gpr": gpr,
                    "offset": offset,
                    "len": length,
                }
                for pc, gpr, offset, length in specs
            ],
            "deref_specs": [
                {
                    "pc": None if pc is None else f"0x{pc:08X}",
                    "gpr": gpr,
                    "ptr_offset": ptr_offset,
                    "read_offset": read_offset,
                    "len": length,
                }
                for pc, gpr, ptr_offset, read_offset, length in deref_specs
            ],
            "windows": windows,
            "window_count": len(windows),
            "truncated": truncated,
            "errors": errors,
        }

    for rec in records:
        rec_pc = int(rec.get("pc", 0)) & 0xFFFFFFFF
        gprs = rec.get("gpr", [])
        if not isinstance(gprs, list):
            continue
        for want_pc, gpr, offset, length in specs:
            if want_pc is not None and rec_pc != want_pc:
                continue
            if gpr >= len(gprs):
                continue
            base = int(gprs[gpr]) & 0xFFFFFFFF
            addr = (base + offset) & 0xFFFFFFFF
            key = (want_pc, rec_pc, gpr, offset, addr)
            if key in seen:
                continue
            seen.add(key)
            if len(windows) >= max_unique:
                truncated = True
                return result()
            entry = {
                "seq": rec.get("seq"),
                "block": rec.get("block"),
                "pc": f"0x{rec_pc:08X}",
                "gpr": gpr,
                "base": f"0x{base:08X}",
                "offset": offset,
                "addr": f"0x{addr:08X}",
                "len": length,
            }
            try:
                data = tcp_memory(client, addr, length, name)
                entry["ok"] = True
                entry["sha256"] = hashlib.sha256(data).hexdigest()
                entry["hex"] = data.hex()
            except (OSError, ProtocolError) as exc:
                errors += 1
                entry["ok"] = False
                entry["error"] = str(exc)
            windows.append(entry)
        for want_pc, gpr, ptr_offset, read_offset, length in deref_specs:
            if want_pc is not None and rec_pc != want_pc:
                continue
            if gpr >= len(gprs):
                continue
            base = int(gprs[gpr]) & 0xFFFFFFFF
            ptr_addr = (base + ptr_offset) & 0xFFFFFFFF
            ptr_key = ("deref-ptr", want_pc, rec_pc, gpr, ptr_offset, ptr_addr)
            if ptr_key in seen:
                continue
            if len(windows) >= max_unique:
                truncated = True
                return result()
            entry = {
                "kind": "deref",
                "seq": rec.get("seq"),
                "block": rec.get("block"),
                "pc": f"0x{rec_pc:08X}",
                "gpr": gpr,
                "base": f"0x{base:08X}",
                "ptr_offset": ptr_offset,
                "ptr_addr": f"0x{ptr_addr:08X}",
                "read_offset": read_offset,
                "len": length,
            }
            try:
                ptr_bytes = tcp_memory(client, ptr_addr, 4, name)
                pointer = int.from_bytes(ptr_bytes, "big")
                addr = (pointer + read_offset) & 0xFFFFFFFF
                range_name = guest_ram_range_name(addr, length)
                key = (
                    "deref",
                    want_pc,
                    rec_pc,
                    gpr,
                    ptr_offset,
                    read_offset,
                    pointer,
                    addr,
                )
                if key in seen:
                    continue
                seen.add(ptr_key)
                seen.add(key)
                if len(windows) >= max_unique:
                    truncated = True
                    return result()
                entry["pointer"] = f"0x{pointer:08X}"
                entry["addr"] = f"0x{addr:08X}"
                if range_name is None:
                    entry["ok"] = False
                    entry["skipped"] = True
                    entry["error"] = "pointer target is outside guest RAM ranges"
                    windows.append(entry)
                    continue
                entry["range"] = range_name
                data = tcp_memory(client, addr, length, name)
                entry["ok"] = True
                entry["sha256"] = hashlib.sha256(data).hexdigest()
                entry["hex"] = data.hex()
            except (OSError, ProtocolError) as exc:
                if ptr_key in seen:
                    continue
                seen.add(ptr_key)
                if len(windows) >= max_unique:
                    truncated = True
                    continue
                errors += 1
                entry["ok"] = False
                entry["error"] = str(exc)
            windows.append(entry)
    return result()


def sample_ram_dumps(
    client: JsonTcp,
    specs: list[tuple[int, int]],
    name: str,
    deref_specs: list[tuple[int, int, int]] | None = None,
) -> dict[str, Any]:
    windows: list[dict[str, Any]] = []
    errors = 0
    deref_specs = [] if deref_specs is None else deref_specs
    for address, length in specs:
        entry: dict[str, Any] = {
            "addr": f"0x{address:08X}",
            "len": length,
            "range": guest_ram_range_name(address, length),
        }
        try:
            data = tcp_memory(client, address, length, name)
            entry["ok"] = True
            entry["sha256"] = hashlib.sha256(data).hexdigest()
            entry["hex"] = data.hex()
        except (OSError, ProtocolError) as exc:
            errors += 1
            entry["ok"] = False
            entry["error"] = str(exc)
        windows.append(entry)
    for pointer_address, read_offset, length in deref_specs:
        entry = {
            "kind": "deref",
            "ptr_addr": f"0x{pointer_address:08X}",
            "read_offset": read_offset,
            "len": length,
        }
        try:
            ptr_bytes = tcp_memory(client, pointer_address, 4, name)
            pointer = int.from_bytes(ptr_bytes, "big")
            address = (pointer + read_offset) & 0xFFFFFFFF
            entry["pointer"] = f"0x{pointer:08X}"
            entry["addr"] = f"0x{address:08X}"
            entry["range"] = guest_ram_range_name(address, length)
            if entry["range"] is None:
                entry["ok"] = False
                entry["skipped"] = True
                entry["error"] = "pointer target is outside guest RAM ranges"
                windows.append(entry)
                continue
            data = tcp_memory(client, address, length, name)
            entry["ok"] = True
            entry["sha256"] = hashlib.sha256(data).hexdigest()
            entry["hex"] = data.hex()
        except (OSError, ProtocolError) as exc:
            errors += 1
            entry["ok"] = False
            entry["error"] = str(exc)
        windows.append(entry)
    return {
        "ok": errors == 0,
        "kind": "ram_dump",
        "direct_specs": [
            {"addr": f"0x{address:08X}", "len": length}
            for address, length in specs
        ],
        "deref_specs": [
            {
                "ptr_addr": f"0x{pointer_address:08X}",
                "read_offset": read_offset,
                "len": length,
            }
            for pointer_address, read_offset, length in deref_specs
        ],
        "windows": windows,
        "window_count": len(windows),
        "errors": errors,
    }


def first_byte_difference(a: bytes, b: bytes) -> int | None:
    for i, (byte_a, byte_b) in enumerate(zip(a, b)):
        if byte_a != byte_b:
            return i
    if len(a) != len(b):
        return min(len(a), len(b))
    return None


def wait_for_xfb_pub(
    client: JsonTcp, target: int | None, timeout: float, name: str
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] | None = None
    while True:
        last = client.request("xfb_pub_count")
        pub = int(last.get("pub_count", 0))
        if target is None or pub >= target:
            return last
        if time.monotonic() >= deadline:
            raise TimeoutError(f"{name} pub_count {pub} did not reach {target}")
        time.sleep(0.025)


def sample_tcp_ranges(
    client: JsonTcp, ranges: list[tuple[int, int]], name: str
) -> dict[str, Any]:
    status = client.request("xfb_pub_count")
    samples = {
        (address, length): tcp_memory(client, address, length, name)
        for address, length in ranges
    }
    return {"status": status, "samples": samples}


def wait_and_sample_tcp_ranges(
    runtime: JsonTcp, dolphin: JsonTcp, args: argparse.Namespace
) -> tuple[dict[str, Any], dict[str, Any]]:
    deadline = time.monotonic() + args.wait_timeout
    runtime_result: dict[str, Any] | None = None
    dolphin_result: dict[str, Any] | None = None
    last_runtime: dict[str, Any] | None = None
    last_dolphin: dict[str, Any] | None = None
    while runtime_result is None or dolphin_result is None:
        if runtime_result is None:
            last_runtime = runtime.request("xfb_pub_count")
            runtime_pub = int(last_runtime.get("pub_count", 0))
            if args.runtime_pub is None or runtime_pub >= args.runtime_pub:
                runtime_result = sample_tcp_ranges(runtime, args.memory, "runtime")
        if dolphin_result is None:
            last_dolphin = dolphin.request("xfb_pub_count")
            dolphin_pub = int(last_dolphin.get("pub_count", 0))
            if args.dolphin_pub is None or dolphin_pub >= args.dolphin_pub:
                dolphin_result = sample_tcp_ranges(dolphin, args.memory, "dolphin")
        if runtime_result is not None and dolphin_result is not None:
            return runtime_result, dolphin_result
        if time.monotonic() >= deadline:
            runtime_pub = 0 if last_runtime is None else int(last_runtime.get("pub_count", 0))
            dolphin_pub = 0 if last_dolphin is None else int(last_dolphin.get("pub_count", 0))
            raise TimeoutError(
                "publication wait timed out: "
                f"runtime {runtime_pub}/{args.runtime_pub}, "
                f"dolphin {dolphin_pub}/{args.dolphin_pub}"
            )
        time.sleep(0.025)
    raise AssertionError("unreachable")


def cmd_tcp_state_diff(args: argparse.Namespace) -> int:
    """Compare live guest RAM ranges over runtime and Dolphin TCP sockets.

    This is the visual-parity companion to xfb-diff: after the EFB-source
    captures identify a publication/copy boundary, use this command to sample
    candidate scene-state ranges at matched publication thresholds without
    bringing up Dolphin's GDB stub.
    """
    runtime = JsonTcp(args.runtime_port, timeout=args.timeout)
    dolphin = JsonTcp(args.dolphin_port, timeout=args.timeout)
    try:
        runtime_result, dolphin_result = wait_and_sample_tcp_ranges(runtime, dolphin, args)
        runtime_status = runtime_result["status"]
        dolphin_status = dolphin_result["status"]

        all_equal = True
        for address, length in args.memory:
            runtime_bytes = runtime_result["samples"][(address, length)]
            dolphin_bytes = dolphin_result["samples"][(address, length)]
            first = first_byte_difference(runtime_bytes, dolphin_bytes)
            equal = first is None
            all_equal = all_equal and equal
            entry: dict[str, Any] = {
                "kind": "memory_range",
                "label": args.label,
                "address": f"0x{address:08X}",
                "length": length,
                "runtime_pub_count": runtime_status.get("pub_count"),
                "runtime_generation": runtime_status.get("generation"),
                "dolphin_pub_count": dolphin_status.get("pub_count"),
                "equal": equal,
                "runtime_sha256": hashlib.sha256(runtime_bytes).hexdigest(),
                "dolphin_sha256": hashlib.sha256(dolphin_bytes).hexdigest(),
            }
            if first is not None:
                window_start = max(0, first - args.context)
                window_end = min(length, first + args.context)
                entry["first_difference"] = {
                    "offset": first,
                    "address": f"0x{address + first:08X}",
                    "runtime_byte": runtime_bytes[first],
                    "dolphin_byte": dolphin_bytes[first],
                    "runtime_context": runtime_bytes[window_start:window_end].hex(),
                    "dolphin_context": dolphin_bytes[window_start:window_end].hex(),
                    "context_start_offset": window_start,
                }
            print(json.dumps(entry, separators=(",", ":")))

        summary = {
            "kind": "summary",
            "label": args.label,
            "runtime_pub_count": runtime_status.get("pub_count"),
            "runtime_generation": runtime_status.get("generation"),
            "dolphin_pub_count": dolphin_status.get("pub_count"),
            "ranges": len(args.memory),
            "result": "pass" if all_equal else "fail",
        }
        print(json.dumps(summary, separators=(",", ":")))
        return 0 if all_equal else 1
    finally:
        runtime.close()
        dolphin.close()


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


def add_runtime_checkpoint_snapshot_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--checkpoint-pc",
        type=lambda value: int(value, 0),
        help="arm GCN_CHECKPOINT_PC before launch; useful for one-shot early probes",
    )
    parser.add_argument(
        "--checkpoint-gpr",
        type=int,
        help="optional GCN_CHECKPOINT_GPR condition for --checkpoint-pc",
    )
    parser.add_argument(
        "--checkpoint-gpr-value",
        type=lambda value: int(value, 0),
        help="optional GCN_CHECKPOINT_GPR_VALUE condition for --checkpoint-pc",
    )
    parser.add_argument(
        "--checkpoint-lr",
        type=lambda value: int(value, 0),
        help="optional GCN_CHECKPOINT_LR condition for --checkpoint-pc",
    )
    parser.add_argument(
        "--snapshot-save",
        help="write GCN_SNAPSHOT_SAVE when the checkpoint is reached",
    )
    parser.add_argument(
        "--snapshot-exit",
        action="store_true",
        help=(
            "exit the runtime after writing --snapshot-save; capture commands "
            "return success once the snapshot exists"
        ),
    )


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

    runtime_capture = sub.add_parser(
        "runtime-capture",
        help=(
            "launch gcnrecomp headless and capture a runtime TCP screenshot "
            "at an XFB publication threshold"
        ),
    )
    add_runtime_args(runtime_capture)
    add_runtime_checkpoint_snapshot_args(runtime_capture)
    runtime_capture.add_argument(
        "--backend",
        default="software",
        help="GCN_GX_BACKEND for the capture run; empty string leaves runtime default",
    )
    runtime_capture.add_argument(
        "--snapshot-load",
        help="optional GCN_SNAPSHOT_LOAD path for starting from a known state",
    )
    runtime_capture.add_argument(
        "--no-memcard-a",
        action="store_true",
        help="leave runtime EXI slot A empty, matching Dolphin SlotA=None",
    )
    runtime_capture.add_argument(
        "--target-pub",
        type=int,
        default=1640,
        help="capture once runtime xfb_pub_count is at least this value",
    )
    runtime_capture.add_argument("--min-height", type=int, default=240)
    runtime_capture.add_argument("--min-luma", type=float, default=40.0)
    runtime_capture.add_argument(
        "--screenshot-name", default="runtime-capture.ppm"
    )
    runtime_capture.add_argument("--screenshot-every", type=int, default=1)
    runtime_capture.add_argument("--poll-interval", type=float, default=0.05)
    runtime_capture.add_argument("--wait-timeout", type=float, default=240.0)
    runtime_capture.add_argument("--max-polls", type=int, default=100000)
    runtime_capture.add_argument(
        "--max-blocks",
        type=lambda value: int(value, 0),
        default=0,
        help="gcn_boot block budget argument; 0 means unbounded until TCP quit",
    )
    runtime_capture.add_argument("--tcp-ready-timeout", type=float, default=30.0)
    runtime_capture.add_argument(
        "--request-timeout",
        type=float,
        default=60.0,
        help="per TCP diagnostic request timeout after the debug port is ready",
    )
    runtime_capture.add_argument("--quit-timeout", type=float, default=3.0)
    runtime_capture.add_argument(
        "--sync-gx",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="run with GCN_GX_PIPELINE=0 for deterministic capture timing",
    )
    runtime_capture.add_argument(
        "--draw-state",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="dump runtime gx_draw_state at the accepted screenshot frame",
    )
    runtime_capture.add_argument(
        "--draw-min-y",
        type=int,
        help=(
            "with --draw-state, retain lower-screen draws whose bbox reaches "
            "this Y even when their area is small"
        ),
    )
    runtime_capture.add_argument(
        "--tev-census",
        action="store_true",
        help="also enable full GCN_GX_TEV_CENSUS draw config buckets (slow)",
    )
    runtime_capture.add_argument(
        "--skip-draw-pc",
        help="diagnostic: set GCN_GX_SKIP_DRAW_PC to skip draws stamped with this PC",
    )
    runtime_capture.add_argument(
        "--pc-seen",
        type=lambda value: int(value, 0),
        action="append",
        default=[],
        help="query pc_seen for this guest PC at the accepted screenshot frame; repeatable",
    )
    runtime_capture.add_argument(
        "--block-dump-count",
        type=int,
        default=0,
        help="dump this many recent block-ring entries at the accepted screenshot frame",
    )
    runtime_capture.add_argument(
        "--fifo-dump-count",
        type=int,
        default=0,
        help="dump this many recent gather-pipe FIFO ring entries at the accepted screenshot frame",
    )
    runtime_capture.add_argument(
        "--watch-range",
        help=(
            "set runtime GCN_WATCH=<lo_hex>:<hi_hex> to record guest RAM writes "
            "into that address range"
        ),
    )
    runtime_capture.add_argument(
        "--watch-dump-count",
        type=int,
        default=0,
        help=(
            "request a runtime watch_dump at the accepted screenshot frame; "
            "the ring text is written to runtime-capture.err.log"
        ),
    )
    runtime_capture.add_argument(
        "--gpr-probe-pc",
        type=lambda value: int(value, 0),
        action="append",
        default=[],
        help=(
            "set GCN_PROBE_PCS for this guest block-entry PC; repeatable. "
            "Use with --gpr-probe-dump-count to persist snapshots."
        ),
    )
    runtime_capture.add_argument(
        "--gpr-probe-dump-count",
        type=int,
        default=0,
        help="dump this many env-gated guest GPR probe entries at the accepted screenshot frame",
    )
    runtime_capture.add_argument(
        "--gpr-probe-inline-memory",
        action="append",
        default=[],
        help=(
            "snapshot RAM at probe time as PC:GPR:OFFSET:LENGTH, or deref as "
            "PC:GPR:PTR_OFFSET:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    runtime_capture.add_argument(
        "--gpr-probe-memory",
        type=parse_gpr_probe_memory,
        action="append",
        default=[],
        help=(
            "after dumping GPR probe records, read unique guest RAM windows as "
            "GPR:OFFSET:LENGTH or PC:GPR:OFFSET:LENGTH; repeatable"
        ),
    )
    runtime_capture.add_argument(
        "--gpr-probe-memory-deref",
        type=parse_gpr_probe_memory_deref,
        action="append",
        default=[],
        help=(
            "after dumping GPR probe records, read a big-endian pointer from "
            "GPR+PTR_OFFSET, then read guest RAM at pointer+READ_OFFSET as "
            "GPR:PTR_OFFSET:READ_OFFSET:LENGTH or "
            "PC:GPR:PTR_OFFSET:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    runtime_capture.add_argument(
        "--ram-dump",
        type=parse_ram_dump_spec,
        action="append",
        default=[],
        help="dump absolute guest RAM at the accepted screenshot as ADDRESS:LENGTH; repeatable",
    )
    runtime_capture.add_argument(
        "--ram-dump-deref",
        type=parse_ram_dump_deref_spec,
        action="append",
        default=[],
        help=(
            "read a big-endian pointer at POINTER_ADDRESS, then dump "
            "pointer+READ_OFFSET as POINTER_ADDRESS:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    runtime_capture.add_argument(
        "--efb-copy-dump",
        action="store_true",
        help="write runtime EFB/XFB copy payloads and metadata into --output-dir",
    )
    runtime_capture.add_argument("--efb-copy-dump-every", type=int, default=1)
    runtime_capture.add_argument(
        "--efb-source-dump",
        action="store_true",
        help="write runtime sampled EFB-source PPMs for copied regions into --output-dir",
    )
    runtime_capture.add_argument("--efb-source-dump-every", type=int, default=1)
    runtime_capture.add_argument(
        "--efb-source-dump-addr",
        help="only dump EFB-source PPMs for copies whose destination address matches this value",
    )
    runtime_capture.add_argument(
        "--convert-png",
        action="store_true",
        help="also convert the PPM capture to PNG with ImageMagick",
    )
    runtime_capture.add_argument("--magick", default="magick")
    runtime_capture.set_defaults(func=cmd_runtime_capture)

    runtime_sweep = sub.add_parser(
        "runtime-pub-sweep",
        help=(
            "launch gcnrecomp once and sample runtime TCP screenshots and "
            "diagnostic rings at multiple XFB publication thresholds"
        ),
    )
    add_runtime_args(runtime_sweep)
    add_runtime_checkpoint_snapshot_args(runtime_sweep)
    runtime_sweep.add_argument(
        "--backend",
        default="software",
        help="GCN_GX_BACKEND for the capture run; empty string leaves runtime default",
    )
    runtime_sweep.add_argument(
        "--snapshot-load",
        help="optional GCN_SNAPSHOT_LOAD path for starting from a known state",
    )
    runtime_sweep.add_argument(
        "--no-memcard-a",
        action="store_true",
        help="leave runtime EXI slot A empty, matching Dolphin SlotA=None",
    )
    runtime_sweep.add_argument(
        "--sample-pub",
        type=int,
        action="append",
        required=True,
        help="sample once runtime xfb_pub_count reaches this threshold; repeatable",
    )
    runtime_sweep.add_argument("--min-height", type=int, default=240)
    runtime_sweep.add_argument("--min-luma", type=float, default=40.0)
    runtime_sweep.add_argument("--screenshot-prefix", default="runtime-sweep")
    runtime_sweep.add_argument("--screenshot-every", type=int, default=1)
    runtime_sweep.add_argument("--poll-interval", type=float, default=0.05)
    runtime_sweep.add_argument("--wait-timeout", type=float, default=600.0)
    runtime_sweep.add_argument("--max-polls", type=int, default=100000)
    runtime_sweep.add_argument(
        "--max-blocks",
        type=lambda value: int(value, 0),
        default=0,
        help="gcn_boot block budget argument; 0 means unbounded until TCP quit",
    )
    runtime_sweep.add_argument("--tcp-ready-timeout", type=float, default=30.0)
    runtime_sweep.add_argument(
        "--request-timeout",
        type=float,
        default=60.0,
        help="per TCP diagnostic request timeout after the debug port is ready",
    )
    runtime_sweep.add_argument("--quit-timeout", type=float, default=3.0)
    runtime_sweep.add_argument(
        "--sync-gx",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="run with GCN_GX_PIPELINE=0 for deterministic capture timing",
    )
    runtime_sweep.add_argument(
        "--draw-state",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="dump runtime gx_draw_state at every accepted sample",
    )
    runtime_sweep.add_argument(
        "--draw-min-y",
        type=int,
        help=(
            "with --draw-state, retain lower-screen draws whose bbox reaches "
            "this Y even when their area is small"
        ),
    )
    runtime_sweep.add_argument(
        "--tev-census",
        action="store_true",
        help="also enable full GCN_GX_TEV_CENSUS draw config buckets (slow)",
    )
    runtime_sweep.add_argument(
        "--skip-draw-pc",
        help="diagnostic: set GCN_GX_SKIP_DRAW_PC to skip draws stamped with this PC",
    )
    runtime_sweep.add_argument(
        "--pc-seen",
        type=lambda value: int(value, 0),
        action="append",
        default=[],
        help="query pc_seen for this guest PC at every accepted sample; repeatable",
    )
    runtime_sweep.add_argument(
        "--block-dump-count",
        type=int,
        default=0,
        help="dump this many recent block-ring entries at every accepted sample",
    )
    runtime_sweep.add_argument(
        "--fifo-dump-count",
        type=int,
        default=0,
        help="dump this many recent gather-pipe FIFO ring entries at every accepted sample",
    )
    runtime_sweep.add_argument(
        "--watch-range",
        help=(
            "set runtime GCN_WATCH=<lo_hex>:<hi_hex> to record guest RAM writes "
            "into that address range"
        ),
    )
    runtime_sweep.add_argument(
        "--watch-dump-count",
        type=int,
        default=0,
        help=(
            "request a runtime watch_dump at every accepted sample; the ring "
            "text is written to runtime-capture.err.log"
        ),
    )
    runtime_sweep.add_argument(
        "--gpr-probe-pc",
        type=lambda value: int(value, 0),
        action="append",
        default=[],
        help=(
            "set GCN_PROBE_PCS for this guest block-entry PC; repeatable. "
            "Use with --gpr-probe-dump-count to persist snapshots."
        ),
    )
    runtime_sweep.add_argument(
        "--gpr-probe-dump-count",
        type=int,
        default=0,
        help="dump this many env-gated guest GPR probe entries at every accepted sample",
    )
    runtime_sweep.add_argument(
        "--gpr-probe-inline-memory",
        action="append",
        default=[],
        help=(
            "snapshot RAM at probe time as PC:GPR:OFFSET:LENGTH, or deref as "
            "PC:GPR:PTR_OFFSET:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    runtime_sweep.add_argument(
        "--gpr-probe-memory",
        type=parse_gpr_probe_memory,
        action="append",
        default=[],
        help=(
            "after dumping GPR probe records, read unique guest RAM windows as "
            "GPR:OFFSET:LENGTH or PC:GPR:OFFSET:LENGTH; repeatable"
        ),
    )
    runtime_sweep.add_argument(
        "--gpr-probe-memory-deref",
        type=parse_gpr_probe_memory_deref,
        action="append",
        default=[],
        help=(
            "after dumping GPR probe records, read a big-endian pointer from "
            "GPR+PTR_OFFSET, then read guest RAM at pointer+READ_OFFSET as "
            "GPR:PTR_OFFSET:READ_OFFSET:LENGTH or "
            "PC:GPR:PTR_OFFSET:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    runtime_sweep.add_argument(
        "--ram-dump",
        type=parse_ram_dump_spec,
        action="append",
        default=[],
        help="dump absolute guest RAM at every accepted sample as ADDRESS:LENGTH; repeatable",
    )
    runtime_sweep.add_argument(
        "--ram-dump-deref",
        type=parse_ram_dump_deref_spec,
        action="append",
        default=[],
        help=(
            "read a big-endian pointer at POINTER_ADDRESS, then dump "
            "pointer+READ_OFFSET as POINTER_ADDRESS:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    runtime_sweep.add_argument(
        "--efb-copy-dump",
        action="store_true",
        help="write runtime EFB/XFB copy payloads and metadata into --output-dir",
    )
    runtime_sweep.add_argument("--efb-copy-dump-every", type=int, default=1)
    runtime_sweep.add_argument(
        "--efb-source-dump",
        action="store_true",
        help="write runtime sampled EFB-source PPMs for copied regions into --output-dir",
    )
    runtime_sweep.add_argument("--efb-source-dump-every", type=int, default=1)
    runtime_sweep.add_argument(
        "--efb-source-dump-addr",
        help="only dump EFB-source PPMs for copies whose destination address matches this value",
    )
    runtime_sweep.add_argument(
        "--convert-png",
        action="store_true",
        help="also convert the PPM captures to PNG with ImageMagick",
    )
    runtime_sweep.add_argument("--magick", default="magick")
    runtime_sweep.set_defaults(func=cmd_runtime_pub_sweep)

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

    dolphin_ipl_capture = sub.add_parser(
        "dolphin-ipl-capture",
        help=(
            "launch the patched Dolphin NoGUI IPL oracle and capture a TCP PPM "
            "screenshot at a target XFB publication count"
        ),
    )
    dolphin_ipl_capture.add_argument("--dolphin-exe", required=True)
    dolphin_ipl_capture.add_argument("--ipl", required=True)
    dolphin_ipl_capture.add_argument("--disc", required=True)
    dolphin_ipl_capture.add_argument("--dsp-rom", required=True)
    dolphin_ipl_capture.add_argument("--dsp-coef", required=True)
    dolphin_ipl_capture.add_argument("--output-dir", required=True)
    dolphin_ipl_capture.add_argument("--dolphin-port", type=int, default=4588)
    dolphin_ipl_capture.add_argument(
        "--region",
        choices=("NTSC_U", "NTSC_J", "PAL", "USA", "JAP", "JPN", "EUR"),
        default="NTSC_U",
    )
    dolphin_ipl_capture.add_argument(
        "--video-backend", default="Software Renderer"
    )
    dolphin_ipl_capture.add_argument(
        "--cpu-core",
        type=int,
        help=(
            "optional Dolphin.Core.CPUCore override; use 0 for Interpreter "
            "when sw_draw_state needs guest PC attribution"
        ),
    )
    dolphin_ipl_capture.add_argument(
        "--no-memcard-a",
        action="store_true",
        help="force Dolphin.Core.SlotA=None for the no-memory-card attract route",
    )
    dolphin_ipl_capture.add_argument(
        "--target-pub",
        type=int,
        default=1740,
        help=(
            "capture once Dolphin reaches this XFB publication count; use 0 "
            "with --min-luma/--min-height to accept the first qualifying frame"
        ),
    )
    dolphin_ipl_capture.add_argument("--min-height", type=int, default=240)
    dolphin_ipl_capture.add_argument("--min-luma", type=float, default=0.0)
    dolphin_ipl_capture.add_argument("--min-screenshot-pub", type=int, default=0)
    dolphin_ipl_capture.add_argument(
        "--screenshot-name", default="dolphin-ipl-capture.ppm"
    )
    dolphin_ipl_capture.add_argument("--screenshot-every", type=int, default=5)
    dolphin_ipl_capture.add_argument("--poll-interval", type=float, default=0.05)
    dolphin_ipl_capture.add_argument("--wait-timeout", type=float, default=180.0)
    dolphin_ipl_capture.add_argument("--max-polls", type=int, default=100000)
    dolphin_ipl_capture.add_argument("--tcp-ready-timeout", type=float, default=30.0)
    dolphin_ipl_capture.add_argument(
        "--request-timeout",
        type=float,
        default=60.0,
        help="per TCP diagnostic request timeout after the trace port is ready",
    )
    dolphin_ipl_capture.add_argument("--quit-timeout", type=float, default=3.0)
    dolphin_ipl_capture.add_argument(
        "--efb-copy-dump",
        action="store_true",
        help="write Dolphin EFB/XFB copy payloads and metadata into --output-dir",
    )
    dolphin_ipl_capture.add_argument("--efb-copy-dump-every", type=int, default=1)
    dolphin_ipl_capture.add_argument(
        "--efb-source-dump",
        action="store_true",
        help="write Dolphin sampled EFB-source PPMs for copied regions into --output-dir",
    )
    dolphin_ipl_capture.add_argument("--efb-source-dump-every", type=int, default=1)
    dolphin_ipl_capture.add_argument(
        "--efb-source-dump-addr",
        help="only dump EFB-source PPMs for copies whose destination address matches this value",
    )
    dolphin_ipl_capture.add_argument(
        "--sw-draw-state",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="dump the Dolphin software-renderer lower/large triangle draw ring",
    )
    dolphin_ipl_capture.add_argument(
        "--sw-draw-min-area",
        type=int,
        default=8192,
        help="minimum scissored triangle bbox area retained by --sw-draw-state",
    )
    dolphin_ipl_capture.add_argument(
        "--sw-draw-min-y",
        type=int,
        default=180,
        help="retain smaller --sw-draw-state triangles only if they reach this Y",
    )
    dolphin_ipl_capture.add_argument(
        "--xf-context-state",
        action="store_true",
        help="dump the Dolphin XF write context ring beside the screenshot",
    )
    dolphin_ipl_capture.add_argument(
        "--xf-context-count",
        type=int,
        default=8192,
        help="number of newest Dolphin XF context records to dump",
    )
    dolphin_ipl_capture.add_argument(
        "--gpr-probe-pc",
        type=lambda value: int(value, 0),
        action="append",
        default=[],
        help=(
            "set Dolphin GCN_TRACE_PROBE_PCS for this guest PC; repeatable. "
            "Use with --cpu-core 0 so the interpreter samples the registers"
        ),
    )
    dolphin_ipl_capture.add_argument(
        "--gpr-probe-dump-count",
        type=int,
        default=0,
        help="dump the newest N Dolphin GPR probe records beside the screenshot",
    )
    dolphin_ipl_capture.add_argument(
        "--gpr-probe-inline-memory",
        action="append",
        default=[],
        help=(
            "snapshot RAM at probe time as PC:GPR:OFFSET:LENGTH, or deref as "
            "PC:GPR:PTR_OFFSET:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    dolphin_ipl_capture.add_argument(
        "--gpr-probe-memory",
        type=parse_gpr_probe_memory,
        action="append",
        default=[],
        help=(
            "after dumping GPR probe records, read unique guest RAM windows as "
            "GPR:OFFSET:LENGTH or PC:GPR:OFFSET:LENGTH; repeatable"
        ),
    )
    dolphin_ipl_capture.add_argument(
        "--gpr-probe-memory-deref",
        type=parse_gpr_probe_memory_deref,
        action="append",
        default=[],
        help=(
            "after dumping GPR probe records, read a big-endian pointer from "
            "GPR+PTR_OFFSET, then read guest RAM at pointer+READ_OFFSET as "
            "GPR:PTR_OFFSET:READ_OFFSET:LENGTH or "
            "PC:GPR:PTR_OFFSET:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    dolphin_ipl_capture.add_argument(
        "--ram-dump",
        type=parse_ram_dump_spec,
        action="append",
        default=[],
        help="dump absolute guest RAM at the accepted screenshot as ADDRESS:LENGTH; repeatable",
    )
    dolphin_ipl_capture.add_argument(
        "--ram-dump-deref",
        type=parse_ram_dump_deref_spec,
        action="append",
        default=[],
        help=(
            "read a big-endian pointer at POINTER_ADDRESS, then dump "
            "pointer+READ_OFFSET as POINTER_ADDRESS:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    dolphin_ipl_capture.add_argument(
        "--pad-pulse-pub",
        type=int,
        help="when Dolphin reaches this XFB publication count, pulse pad 0",
    )
    dolphin_ipl_capture.add_argument(
        "--pad-pulse",
        type=parse_pad_pulse,
        action="append",
        default=[],
        help=(
            "repeatable pad pulse as PUB:BUTTONS[:POLLS[:STICK_X:STICK_Y]]; "
            "for example 1850:0x0100:25 then 2050:0x0101:25"
        ),
    )
    dolphin_ipl_capture.add_argument(
        "--pad-pulse-buttons",
        type=lambda value: int(value, 0),
        default=0x0100,
        help="pad buttons used by --pad-pulse-pub; default is A",
    )
    dolphin_ipl_capture.add_argument(
        "--pad-pulse-polls",
        type=int,
        default=30,
        help="number of capture polls to hold --pad-pulse-buttons before release",
    )
    dolphin_ipl_capture.add_argument("--pad-pulse-stick-x", type=int, default=0x80)
    dolphin_ipl_capture.add_argument("--pad-pulse-stick-y", type=int, default=0x80)
    dolphin_ipl_capture.add_argument(
        "--convert-png",
        action="store_true",
        help="also convert the PPM capture to PNG with ImageMagick",
    )
    dolphin_ipl_capture.add_argument("--magick", default="magick")
    dolphin_ipl_capture.set_defaults(func=cmd_dolphin_ipl_capture)

    dolphin_ipl_sweep = sub.add_parser(
        "dolphin-ipl-sweep",
        help=(
            "launch one patched Dolphin NoGUI IPL oracle and capture TCP PPM "
            "screenshots at multiple XFB publication counts"
        ),
    )
    dolphin_ipl_sweep.add_argument("--dolphin-exe", required=True)
    dolphin_ipl_sweep.add_argument("--ipl", required=True)
    dolphin_ipl_sweep.add_argument("--disc", required=True)
    dolphin_ipl_sweep.add_argument("--dsp-rom", required=True)
    dolphin_ipl_sweep.add_argument("--dsp-coef", required=True)
    dolphin_ipl_sweep.add_argument("--output-dir", required=True)
    dolphin_ipl_sweep.add_argument("--dolphin-port", type=int, default=4588)
    dolphin_ipl_sweep.add_argument(
        "--region",
        choices=("NTSC_U", "NTSC_J", "PAL", "USA", "JAP", "JPN", "EUR"),
        default="NTSC_U",
    )
    dolphin_ipl_sweep.add_argument("--video-backend", default="Software Renderer")
    dolphin_ipl_sweep.add_argument(
        "--cpu-core",
        type=int,
        help="optional Dolphin.Core.CPUCore override; use 0 for interpreter probes",
    )
    dolphin_ipl_sweep.add_argument(
        "--no-memcard-a",
        action="store_true",
        help="force Dolphin.Core.SlotA=None for the no-memory-card attract route",
    )
    dolphin_ipl_sweep.add_argument(
        "--sample-pub",
        type=int,
        action="append",
        default=[],
        help="XFB publication count to capture; repeatable",
    )
    dolphin_ipl_sweep.add_argument("--screenshot-prefix", default="dolphin-ipl-sweep")
    dolphin_ipl_sweep.add_argument("--min-height", type=int, default=240)
    dolphin_ipl_sweep.add_argument("--min-luma", type=float, default=0.0)
    dolphin_ipl_sweep.add_argument("--min-screenshot-pub", type=int, default=0)
    dolphin_ipl_sweep.add_argument("--screenshot-every", type=int, default=5)
    dolphin_ipl_sweep.add_argument("--poll-interval", type=float, default=0.05)
    dolphin_ipl_sweep.add_argument("--wait-timeout", type=float, default=180.0)
    dolphin_ipl_sweep.add_argument("--max-polls", type=int, default=100000)
    dolphin_ipl_sweep.add_argument("--tcp-ready-timeout", type=float, default=30.0)
    dolphin_ipl_sweep.add_argument(
        "--request-timeout",
        type=float,
        default=60.0,
        help="per TCP diagnostic request timeout after the trace port is ready",
    )
    dolphin_ipl_sweep.add_argument("--quit-timeout", type=float, default=3.0)
    dolphin_ipl_sweep.add_argument(
        "--sw-draw-state",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="dump the Dolphin software-renderer lower/large triangle draw ring per sample",
    )
    dolphin_ipl_sweep.add_argument("--sw-draw-min-area", type=int, default=8192)
    dolphin_ipl_sweep.add_argument("--sw-draw-min-y", type=int, default=180)
    dolphin_ipl_sweep.add_argument(
        "--xf-context-state",
        action="store_true",
        help="dump the Dolphin XF write context ring per accepted sample",
    )
    dolphin_ipl_sweep.add_argument(
        "--xf-context-count",
        type=int,
        default=8192,
        help="number of newest Dolphin XF context records to dump per sample",
    )
    dolphin_ipl_sweep.add_argument("--gpr-probe-pc", type=lambda value: int(value, 0),
                                   action="append", default=[])
    dolphin_ipl_sweep.add_argument("--gpr-probe-inline-memory", action="append", default=[])
    dolphin_ipl_sweep.add_argument(
        "--ram-dump",
        type=parse_ram_dump_spec,
        action="append",
        default=[],
        help="dump absolute guest RAM at every accepted sample as ADDRESS:LENGTH; repeatable",
    )
    dolphin_ipl_sweep.add_argument(
        "--ram-dump-deref",
        type=parse_ram_dump_deref_spec,
        action="append",
        default=[],
        help=(
            "read a big-endian pointer at POINTER_ADDRESS, then dump "
            "pointer+READ_OFFSET as POINTER_ADDRESS:READ_OFFSET:LENGTH; repeatable"
        ),
    )
    dolphin_ipl_sweep.add_argument("--efb-copy-dump", action="store_true")
    dolphin_ipl_sweep.add_argument("--efb-copy-dump-every", type=int, default=1)
    dolphin_ipl_sweep.add_argument("--efb-source-dump", action="store_true")
    dolphin_ipl_sweep.add_argument("--efb-source-dump-every", type=int, default=1)
    dolphin_ipl_sweep.add_argument("--efb-source-dump-addr")
    dolphin_ipl_sweep.add_argument(
        "--convert-png",
        action="store_true",
        help="also convert PPM captures to PNG with ImageMagick",
    )
    dolphin_ipl_sweep.add_argument("--magick", default="magick")
    dolphin_ipl_sweep.set_defaults(func=cmd_dolphin_ipl_sweep)

    tcp_state_diff = sub.add_parser(
        "tcp-state-diff",
        help=(
            "compare live runtime and Dolphin guest RAM over JSON TCP "
            "`read_ram`; useful for correlating XFB/EFB-copy divergence "
            "with scene-state bytes"
        ),
    )
    tcp_state_diff.add_argument("--runtime-port", type=int, required=True)
    tcp_state_diff.add_argument("--dolphin-port", type=int, required=True)
    tcp_state_diff.add_argument("--timeout", type=float, default=10.0)
    tcp_state_diff.add_argument(
        "--wait-timeout",
        type=float,
        default=120.0,
        help="seconds to wait for requested publication thresholds",
    )
    tcp_state_diff.add_argument(
        "--runtime-pub",
        type=int,
        help="wait until runtime xfb_pub_count is at least this value",
    )
    tcp_state_diff.add_argument(
        "--dolphin-pub",
        type=int,
        help="wait until Dolphin xfb_pub_count is at least this value",
    )
    tcp_state_diff.add_argument(
        "--memory",
        type=parse_range,
        action="append",
        required=True,
        help="guest memory range ADDRESS:LENGTH (repeatable)",
    )
    tcp_state_diff.add_argument(
        "--context",
        type=int,
        default=16,
        help="bytes of context around the first difference in each direction",
    )
    tcp_state_diff.add_argument(
        "--label",
        default="tcp-state-diff",
        help="freeform label echoed into JSON output",
    )
    tcp_state_diff.set_defaults(func=cmd_tcp_state_diff)

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
    except (OSError, ProtocolError, subprocess.SubprocessError, TimeoutError) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
