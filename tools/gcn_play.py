#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# gcn_play — interactive keyboard driver for a running gcn_boot instance.
#
# Opens a Tk window that shows the live guest framebuffer (polled from the debug
# server's `screenshot` command) and maps your keyboard to the GameCube pad,
# injected over the same TCP debug socket (set_input). It is NOT a real emulator
# display path — it's a poll-and-refresh remote control — so it runs at roughly
# the guest's own frame rate (a few fps in the debug build) and feels turn-based
# rather than smooth. Good enough to walk the IPL menus by hand.
#
#   python tools/gcn_play.py [--port 4390] [--zoom 2] [--fps 3]
#
# Controls (GC pad):
#   Arrow keys / D-pad      -> D-pad            (Up/Down/Left/Right)
#   W A S D                 -> control stick    (main analog stick)
#   I J K L                 -> C-stick          (sub analog stick)
#   Z                       -> A                 Space also = A
#   X                       -> B
#   C                       -> Z (shoulder)
#   Q / E                   -> L / R triggers (full)
#   Enter                   -> Start
#   Backspace               -> release everything to neutral
#   Esc                     -> quit the player (leaves gcn_boot running)
#
# Tk PhotoImage reads the P6 PPM the server writes directly, so no PIL is needed.
import argparse, json, os, socket, tempfile, tkinter as tk

# GC pad button bit masks (SI report / si.c set_input "buttons").
BTN = {
    'A': 0x0100, 'B': 0x0200, 'X': 0x0400, 'Y': 0x0800, 'Start': 0x1000,
    'Z': 0x0010, 'L': 0x0040, 'R': 0x0020,
    'Up': 0x0008, 'Down': 0x0004, 'Right': 0x0002, 'Left': 0x0001,
}

class Client:
    """One short-lived request per call — matches gcn_debug_client's protocol."""
    def __init__(self, port): self.port = port
    def cmd(self, obj):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=4)
            s.sendall((json.dumps(obj) + "\n").encode())
            buf = b""
            while b"\n" not in buf:
                c = s.recv(1 << 16)
                if not c: break
                buf += c
            s.close()
            return json.loads(buf.split(b"\n", 1)[0])
        except Exception as e:
            return {"ok": False, "error": str(e)}

class Player:
    def __init__(self, root, port, zoom, fps):
        self.c = Client(port)
        self.root = root
        self.zoom = zoom
        self.delay = max(60, int(1000 / max(1, fps)))
        self.held = set()          # currently-pressed logical keys
        self.pending_release = {}  # key -> after-id (Windows auto-repeat debounce)
        self.shot = os.path.join(tempfile.gettempdir(), "gcn_play_live.ppm")

        root.title("gcn_play — GameCube IPL (keyboard)")
        root.configure(bg="black")
        self.label = tk.Label(root, text="waiting for framebuffer...",
                              bg="black", fg="#8888aa", font=("Consolas", 14))
        self.label.pack(padx=4, pady=4)
        self.status = tk.Label(root, text="", bg="black", fg="#6688aa",
                              font=("Consolas", 9), anchor="w", justify="left")
        self.status.pack(fill="x")
        self.img = None

        # key press/release: track a logical-key set, recompute the pad each edit.
        self.keymap = {
            'Up': 'Up', 'Down': 'Down', 'Left': 'Left', 'Right': 'Right',
            'w': 'sU', 'a': 'sL', 's': 'sD', 'd': 'sR',
            'i': 'cU', 'j': 'cL', 'k': 'cD', 'l': 'cR',
            'z': 'A', 'space': 'A', 'x': 'B', 'c': 'Z',
            'q': 'L', 'e': 'R', 'Return': 'Start',
        }
        root.bind("<KeyPress>", self.on_press)
        root.bind("<KeyRelease>", self.on_release)
        root.bind("<BackSpace>", lambda e: self.neutral())
        root.bind("<Escape>", lambda e: root.destroy())
        self.refresh()

    def logical(self, event):
        k = event.keysym
        if len(k) == 1: k = k.lower()
        return self.keymap.get(k)

    def on_press(self, event):
        lk = self.logical(event)
        if lk is None: return
        aid = self.pending_release.pop(lk, None)
        if aid is not None: self.root.after_cancel(aid)  # cancel auto-repeat release
        if lk not in self.held:
            self.held.add(lk); self.send()

    def on_release(self, event):
        lk = self.logical(event)
        if lk is None: return
        # Windows repeats KeyRelease/KeyPress rapidly while held; defer the actual
        # release so a same-key re-press within ~70ms cancels it.
        def do():
            self.held.discard(lk); self.pending_release.pop(lk, None); self.send()
        self.pending_release[lk] = self.root.after(70, do)

    def neutral(self):
        for aid in self.pending_release.values(): self.root.after_cancel(aid)
        self.pending_release.clear(); self.held.clear()
        self.c.cmd({"cmd": "set_input", "reset": 1})

    def send(self):
        buttons = 0
        for name, bit in BTN.items():
            if name in self.held: buttons |= bit
        sx = sy = cx = cy = 0x80
        if 'sL' in self.held: sx = 0x00
        if 'sR' in self.held: sx = 0xFF
        if 'sD' in self.held: sy = 0x00
        if 'sU' in self.held: sy = 0xFF
        if 'cL' in self.held: cx = 0x00
        if 'cR' in self.held: cx = 0xFF
        if 'cD' in self.held: cy = 0x00
        if 'cU' in self.held: cy = 0xFF
        self.c.cmd({"cmd": "set_input", "buttons": buttons,
                    "stick_x": sx, "stick_y": sy, "substick_x": cx, "substick_y": cy})

    def refresh(self):
        r = self.c.cmd({"cmd": "screenshot", "path": self.shot})
        if r.get("ok"):
            try:
                img = tk.PhotoImage(file=self.shot)
                if self.zoom > 1: img = img.zoom(self.zoom)
                self.img = img
                self.label.configure(image=img, text="")
                self.status.configure(text=(
                    "block %s  luma %.0f  |  arrows/WASD move, Z=A X=B Enter=Start, "
                    "Backspace=neutral, Esc=quit"
                    % (self.c.cmd({"cmd": "ping"}).get("block", "?"),
                       r.get("mean_luma", 0))))
            except tk.TclError:
                pass
        elif "refused" in str(r.get("error", "")).lower():
            self.label.configure(text="gcn_boot not reachable on this port", image="")
        else:
            self.label.configure(text=r.get("error", "no framebuffer yet"), image="")
        self.root.after(self.delay, self.refresh)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4390)
    ap.add_argument("--zoom", type=int, default=2)
    ap.add_argument("--fps", type=int, default=3)
    a = ap.parse_args()
    root = tk.Tk()
    Player(root, a.port, a.zoom, a.fps)
    root.mainloop()

if __name__ == "__main__":
    main()
