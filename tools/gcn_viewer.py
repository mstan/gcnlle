#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Interactive viewer for the gcnrecomp runtime: streams the live XFB through
# the TCP debug server's `screenshot` command into a Tk window and maps the
# keyboard onto the SI pad via `set_input`. The runtime itself stays headless
# (debug_server.c is the only I/O surface); this is a pure client — no
# runtime state lives here, closing the window does not disturb the boot.
#
#   python tools/gcn_viewer.py [--port N] [--scale N]
#
# Controls (GameCube pad; GCN_SI_PAD_* bits from runtime/include/si/si.h):
#   Arrow keys ..... main stick        X .... A        Z .... B
#   W/A/S/D ........ D-pad             C .... X        V .... Y
#   I/J/K/L ........ C-stick           Enter/Space ... Start
#   Q .... L trigger (full)            E .... R trigger (full)
#   Esc .. quit viewer
import argparse
import json
import os
import socket
import sys
import tempfile
import time
import tkinter as tk

# GCN_SI_PAD_* button bits (runtime/include/si/si.h).
PAD_LEFT, PAD_RIGHT, PAD_DOWN, PAD_UP = 0x0001, 0x0002, 0x0004, 0x0008
PAD_Z, PAD_R, PAD_L = 0x0010, 0x0020, 0x0040
PAD_A, PAD_B, PAD_X, PAD_Y, PAD_START = 0x0100, 0x0200, 0x0400, 0x0800, 0x1000

BUTTON_KEYS = {
    "x": PAD_A, "z": PAD_B, "c": PAD_X, "v": PAD_Y,
    "Return": PAD_START, "space": PAD_START,
    "q": PAD_L, "e": PAD_R,
    "w": PAD_UP, "s": PAD_DOWN, "a": PAD_LEFT, "d": PAD_RIGHT,
}
STICK_KEYS = {  # main stick: keysym -> (dx, dy) in pad units from center 0x80
    "Up": (0, 127), "Down": (0, -127), "Left": (-127, 0), "Right": (127, 0),
}
CSTICK_KEYS = {
    "i": (0, 127), "k": (0, -127), "j": (-127, 0), "l": (127, 0),
}


class DebugLink:
    """One persistent JSON-over-newline connection to debug_server.c."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.next_id = 1

    def request(self, cmd, **fields):
        req = {"id": self.next_id, "cmd": cmd}
        self.next_id += 1
        req.update(fields)
        self.sock.sendall((json.dumps(req) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("debug server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode(errors="replace"))


class Viewer:
    FRAME_MS = 15  # poll as close to 60 fps as the pipeline allows

    def __init__(self, link, scale):
        self.link = link
        self.scale = scale
        # The server fopen()s + fwrite()s the PPM before replying, so one
        # file reused every frame is race-free from this client's view.
        self.ppm = os.path.join(tempfile.gettempdir(),
                                f"gcn_viewer_{os.getpid()}.ppm").replace("\\", "/")
        self.pressed = set()      # debounce Windows key auto-repeat
        self.buttons = 0
        self.stick = [0x80, 0x80]
        self.cstick = [0x80, 0x80]
        self.frames = 0
        self.fps = 0.0
        self.fps_t0 = time.perf_counter()

        self.root = tk.Tk()
        self.root.title("gcnrecomp — connecting…")
        self.root.configure(bg="black")
        self.label = tk.Label(self.root, bg="black")
        self.label.pack()
        self.status = tk.Label(self.root, fg="#888", bg="black", anchor="w",
                               font=("Consolas", 9),
                               text="arrows=stick  X=A Z=B C=X V=Y  Enter=Start"
                                    "  WASD=dpad  IJKL=cstick  Q/E=L/R  Esc=quit")
        self.status.pack(fill="x")
        self.img = None
        self.root.bind("<KeyPress>", self.on_press)
        self.root.bind("<KeyRelease>", self.on_release)
        self.root.protocol("WM_DELETE_WINDOW", self.quit)

    # ---- input ------------------------------------------------------------
    def send_input(self):
        try:
            self.link.request("set_input", buttons=self.buttons,
                              stick_x=self.stick[0], stick_y=self.stick[1],
                              substick_x=self.cstick[0], substick_y=self.cstick[1],
                              trigger_l=255 if self.buttons & PAD_L else 0,
                              trigger_r=255 if self.buttons & PAD_R else 0)
        except (ConnectionError, OSError):
            self.quit()

    def apply_key(self, keysym, down):
        changed = False
        if keysym in BUTTON_KEYS:
            new = (self.buttons | BUTTON_KEYS[keysym]) if down else \
                  (self.buttons & ~BUTTON_KEYS[keysym])
            changed = new != self.buttons
            self.buttons = new
        for table, vec in ((STICK_KEYS, self.stick), (CSTICK_KEYS, self.cstick)):
            if keysym in table:
                dx, dy = table[keysym]
                nx = max(0, min(255, 0x80 + (dx if down else 0)))
                ny = max(0, min(255, 0x80 + (dy if down else 0)))
                # Only override the axis this key drives.
                if dx: changed |= vec[0] != nx; vec[0] = nx
                if dy: changed |= vec[1] != ny; vec[1] = ny
        if changed:
            self.send_input()

    def on_press(self, ev):
        if ev.keysym == "Escape":
            self.quit()
            return
        if ev.keysym in self.pressed:   # Windows auto-repeat: presses only
            return
        self.pressed.add(ev.keysym)
        self.apply_key(ev.keysym, True)

    def on_release(self, ev):
        self.pressed.discard(ev.keysym)
        self.apply_key(ev.keysym, False)

    # ---- video ------------------------------------------------------------
    def poll_frame(self):
        try:
            r = self.link.request("screenshot", path=self.ppm)
        except (ConnectionError, OSError):
            self.quit()
            return
        if r.get("ok"):
            try:
                img = tk.PhotoImage(file=self.ppm)   # Tk reads P6 PPM natively
                # Aspect: the IPL switches VI modes per screen — some scenes
                # scan out a half-height field (e.g. 592x224), others a full
                # 448-line frame. Double the vertical zoom for fields so both
                # display at the same, correct 4:3-ish proportions.
                w, h = r.get("width", 0), r.get("height", 1)
                ydouble = 2 if h * 2 <= w else 1
                if self.scale > 1 or ydouble > 1:
                    img = img.zoom(self.scale, self.scale * ydouble)
                self.img = img                        # keep a ref or Tk drops it
                self.label.configure(image=img)
            except tk.TclError:
                pass                                  # transient decode issue
            self.frames += 1
            now = time.perf_counter()
            if now - self.fps_t0 >= 1.0:
                self.fps = self.frames / (now - self.fps_t0)
                self.frames = 0
                self.fps_t0 = now
                self.root.title(
                    f"gcnrecomp — {r.get('width')}x{r.get('height')}"
                    f" @ {self.fps:.0f} fps (viewer)  luma {r.get('mean_luma')}")
        else:
            self.root.title(f"gcnrecomp — no XFB yet ({r.get('error', '?')})")
        self.root.after(self.FRAME_MS, self.poll_frame)

    # ---- lifecycle ----------------------------------------------------------
    def quit(self):
        try:
            os.unlink(self.ppm)
        except OSError:
            pass
        self.root.destroy()

    def run(self):
        self.root.after(0, self.poll_frame)
        self.root.mainloop()


def main():
    ap = argparse.ArgumentParser(description="gcnrecomp live viewer / pad client")
    ap.add_argument("--port", type=int, default=4380, help="debug server port")
    ap.add_argument("--scale", type=int, default=2, help="integer zoom (default 2)")
    args = ap.parse_args()
    try:
        link = DebugLink(args.port)
    except OSError as e:
        print(f"cannot connect to debug server on 127.0.0.1:{args.port}: {e}\n"
              f"start the runtime first, e.g.\n"
              f"  GCN_DEBUG_PORT={args.port} ... gcn_boot.exe _work/bs2.bin 0",
              file=sys.stderr)
        return 1
    Viewer(link, max(1, args.scale)).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
