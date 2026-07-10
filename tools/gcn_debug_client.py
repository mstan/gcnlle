#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Minimal client for the gcnrecomp runtime TCP debug server (debug_server.c).
# JSON-over-newline: one request object per line, one response line back.
#
#   python tools/gcn_debug_client.py [--port N] <cmd> [key=value ...]
#
# key=value args become JSON fields (0x/decimal ints stay ints, else strings),
# so every server command is reachable, e.g.:
#   gcn_debug_client.py regs
#   gcn_debug_client.py read_ram addr=0x80000000 len=64
#   gcn_debug_client.py mmio_dump addr=0xCC00202C count=8
#   gcn_debug_client.py quit
import json, socket, sys

def parse_kv(tok):
    k, _, v = tok.partition("=")
    try:
        return k, int(v, 0)
    except ValueError:
        return k, v

def main():
    args = sys.argv[1:]
    port = 4380
    if args and args[0] == "--port":
        port = int(args[1]); args = args[2:]
    if not args:
        print("usage: gcn_debug_client.py [--port N] <cmd> [key=value ...]"); return 2
    req = {"id": 1, "cmd": args[0]}
    for tok in args[1:]:
        k, v = parse_kv(tok); req[k] = v

    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk: break
        buf += chunk
    line = buf.split(b"\n", 1)[0].decode(errors="replace")
    try:
        print(json.dumps(json.loads(line), indent=2))
    except json.JSONDecodeError:
        print(line)
    s.close()

if __name__ == "__main__":
    sys.exit(main())
