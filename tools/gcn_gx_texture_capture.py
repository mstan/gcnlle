#!/usr/bin/env python3
"""Capture and independently untile textures from a retained GX draw.

The runtime's gx_draw_state command identifies the guest MEM1 ranges consumed
by each TEV census configuration. This client reads all ranges in one debug-
server pump (so the guest cannot advance between chunks), verifies their
draw-time FNV-1a hashes, and writes raw bytes plus simple PPM channel views.
It intentionally has no image-library dependency.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import socket
from typing import Any


def request(port: int, obj: dict[str, Any]) -> dict[str, Any]:
    with socket.create_connection(("127.0.0.1", port), timeout=10.0) as sock:
        sock.sendall(json.dumps(obj, separators=(",", ":")).encode() + b"\n")
        return recv_json_line(sock)


def recv_json_line(sock: socket.socket) -> dict[str, Any]:
    buf = bytearray()
    while True:
        block = sock.recv(65536)
        if not block:
            raise RuntimeError("debug server closed before a complete response")
        buf.extend(block)
        nl = buf.find(b"\n")
        if nl >= 0:
            return json.loads(buf[:nl])


def fnv1a64(data: bytes) -> str:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def capture_ranges(port: int, textures: list[dict[str, Any]]) -> dict[int, bytes]:
    requests: list[tuple[int, int, int]] = []
    next_id = 1
    for texture in textures:
        remaining = int(texture["bytes"])
        address = int(texture["phys"])
        while remaining:
            length = min(remaining, 65536)
            requests.append((next_id, address, length))
            next_id += 1
            address += length
            remaining -= length

    # The server dispatches every newline already buffered before returning to
    # the guest. Read concurrently with its blocking sends by receiving after
    # one sendall of the complete request batch.
    payload = b"".join(
        json.dumps(
            {"id": req_id, "cmd": "read_ram", "addr": address, "len": length},
            separators=(",", ":"),
        ).encode()
        + b"\n"
        for req_id, address, length in requests
    )
    responses: dict[int, dict[str, Any]] = {}
    with socket.create_connection(("127.0.0.1", port), timeout=10.0) as sock:
        sock.settimeout(30.0)
        sock.sendall(payload)
        pending = bytearray()
        while len(responses) < len(requests):
            block = sock.recv(65536)
            if not block:
                raise RuntimeError("debug server closed during batched RAM read")
            pending.extend(block)
            while True:
                nl = pending.find(b"\n")
                if nl < 0:
                    break
                response = json.loads(pending[:nl])
                del pending[: nl + 1]
                responses[int(response["id"])] = response

    result: dict[int, bytearray] = {
        int(texture["unit"]): bytearray() for texture in textures
    }
    cursor = 0
    for texture in textures:
        remaining = int(texture["bytes"])
        unit = int(texture["unit"])
        while remaining:
            req_id, _address, length = requests[cursor]
            cursor += 1
            response = responses[req_id]
            if not response.get("ok"):
                raise RuntimeError(f"read_ram {req_id} failed: {response}")
            data = bytes.fromhex(response["hex"])
            if len(data) != length:
                raise RuntimeError(
                    f"read_ram {req_id}: expected {length}, received {len(data)}"
                )
            result[unit].extend(data)
            remaining -= length
    return {unit: bytes(data) for unit, data in result.items()}


def untile_ia8(data: bytes, width: int, height: int) -> tuple[bytes, bytes]:
    intensity = bytearray(width * height)
    alpha = bytearray(width * height)
    width_blocks = (width + 3) // 4
    for y in range(height):
        for x in range(width):
            block = ((y >> 2) * width_blocks + (x >> 2)) * 32
            offset = block + (((y & 3) * 4 + (x & 3)) * 2)
            dst = y * width + x
            alpha[dst] = data[offset]
            intensity[dst] = data[offset + 1]
    return bytes(intensity), bytes(alpha)


def untile_ia4(data: bytes, width: int, height: int) -> tuple[bytes, bytes]:
    """Decode GameCube IA4: 8x4/32-byte tiles, A in the high nibble."""
    intensity = bytearray(width * height)
    alpha = bytearray(width * height)
    width_blocks = (width + 7) // 8
    for y in range(height):
        for x in range(width):
            block = ((y >> 2) * width_blocks + (x >> 3)) * 32
            offset = block + ((y & 3) * 8 + (x & 7))
            value = data[offset]
            dst = y * width + x
            alpha[dst] = ((value >> 4) & 0xF) * 0x11
            intensity[dst] = (value & 0xF) * 0x11
    return bytes(intensity), bytes(alpha)


def untile_rgba8(
    data: bytes, width: int, height: int
) -> tuple[bytes, bytes, bytes, bytes]:
    channels = [bytearray(width * height) for _ in range(4)]
    width_blocks = (width + 3) // 4
    for y in range(height):
        for x in range(width):
            block = ((y >> 2) * width_blocks + (x >> 2)) * 64
            offset = block + (((y & 3) * 4 + (x & 3)) * 2)
            dst = y * width + x
            channels[3][dst] = data[offset]
            channels[0][dst] = data[offset + 1]
            channels[1][dst] = data[offset + 32]
            channels[2][dst] = data[offset + 33]
    return tuple(bytes(channel) for channel in channels)  # type: ignore[return-value]


def write_gray_ppm(path: pathlib.Path, width: int, height: int, plane: bytes) -> None:
    rgb = bytearray(len(plane) * 3)
    for index, value in enumerate(plane):
        rgb[index * 3 : index * 3 + 3] = bytes((value, value, value))
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)


def write_rgb_ppm(
    path: pathlib.Path, width: int, height: int, channels: tuple[bytes, bytes, bytes]
) -> None:
    rgb = bytearray(width * height * 3)
    for index, (red, green, blue) in enumerate(zip(*channels)):
        rgb[index * 3 : index * 3 + 3] = bytes((red, green, blue))
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + rgb)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--config-hash", required=True)
    parser.add_argument("--out-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()

    state = request(args.port, {"cmd": "gx_draw_state"})
    if not state.get("ok"):
        raise RuntimeError(state)
    config = next(
        (item for item in state["configs"] if item["hash"].lower() == args.config_hash.lower()),
        None,
    )
    if config is None:
        raise RuntimeError(f"GX config {args.config_hash} has not been observed")
    textures = [item for item in config["textures"] if item["valid"] and item["bytes"]]
    captures = capture_ranges(args.port, textures)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    report = {"config": config, "captures": []}
    for texture in textures:
        unit = int(texture["unit"])
        data = captures[unit]
        actual_hash = fnv1a64(data)
        expected_hash = texture["hash"].lower()
        (args.out_dir / f"unit{unit}.bin").write_bytes(data)
        entry = {
            "unit": unit,
            "expected_hash": expected_hash,
            "actual_hash": actual_hash,
            "hash_matches": actual_hash == expected_hash,
        }
        report["captures"].append(entry)
        width, height, fmt = (
            int(texture["width"]),
            int(texture["height"]),
            int(texture["fmt"]),
        )
        if fmt == 2:
            intensity, alpha = untile_ia4(data, width, height)
            write_gray_ppm(args.out_dir / f"unit{unit}-i.ppm", width, height, intensity)
            write_gray_ppm(args.out_dir / f"unit{unit}-a.ppm", width, height, alpha)
        elif fmt == 3:
            intensity, alpha = untile_ia8(data, width, height)
            write_gray_ppm(args.out_dir / f"unit{unit}-i.ppm", width, height, intensity)
            write_gray_ppm(args.out_dir / f"unit{unit}-a.ppm", width, height, alpha)
        elif fmt == 6:
            red, green, blue, alpha = untile_rgba8(data, width, height)
            write_rgb_ppm(
                args.out_dir / f"unit{unit}-rgb.ppm", width, height, (red, green, blue)
            )
            for name, plane in zip(("r", "g", "b", "a"), (red, green, blue, alpha)):
                write_gray_ppm(
                    args.out_dir / f"unit{unit}-{name}.ppm", width, height, plane
                )

    (args.out_dir / "capture.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report["captures"], indent=2))
    return 0 if all(item["hash_matches"] for item in report["captures"]) else 2


if __name__ == "__main__":
    raise SystemExit(main())
