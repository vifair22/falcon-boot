#!/usr/bin/env python3
"""falcon-boot: drive a QEMU guest's human monitor for the station fixtures.

The fixture guests (FreeDOS, Windows 9x) say nothing on serial, so the only
things a script can do with them are look at the screen and press keys. This
is those two, over the HMP unix socket QEMU is started with
(`-monitor unix:SOCK,server,nowait`), and stdlib only so it runs anywhere the
harness image's python3 does.

  vmctl.py cmd   SOCK 'hmp command'      run one monitor command, print reply
  vmctl.py shot  SOCK OUT.png            screendump, written as PNG
  vmctl.py keys  SOCK KEY [KEY...]       sendkey each, 150 ms apart
  vmctl.py type  SOCK 'text'             sendkey a string (a-z 0-9 space . \\ :)
  vmctl.py share SOCK R G B              fraction of the screen in that colour
  vmctl.py desktop SOCK                  exit 0 when the screen is a 9x desktop
  vmctl.py still SOCK STATE              exit 0 when the screen matches the one
                                         saved in STATE last time (under 0.5%
                                         of pixels differ), then save this one

`desktop` is how an unattended Windows 9x install is known to be finished:
the taskbar, a full-width band of button grey (192,192,192) along the bottom
of the screen that no Setup screen has. The flat teal (0,128,128) of the
default wallpaper is the fallback, for a screen where something covers the
taskbar; it is not the main test because 98's Active Desktop replaces the
teal with a blue logo wallpaper a minute or so after the desktop appears.
"""

import os
import socket
import struct
import sys
import tempfile
import time
import zlib

TEAL = (0, 128, 128)
GREY = (192, 192, 192)


def hmp(sock_path, command, timeout=30.0):
    """Send one HMP command and return what the monitor printed for it."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    buf = b""
    # The banner ends with the first prompt.
    while not buf.endswith(b"(qemu) "):
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    s.sendall(command.encode() + b"\n")
    out = b""
    while not out.endswith(b"(qemu) "):
        chunk = s.recv(4096)
        if not chunk:
            break
        out += chunk
    s.close()
    text = out.decode(errors="replace").replace("\r", "")
    # Drop the echoed command line and the trailing prompt.
    lines = [l for l in text.split("\n") if not l.startswith("(qemu)")]
    lines = [l for l in lines if command not in l]
    return "\n".join(l for l in lines if l.strip())


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 header: magic, width, height, maxval, each whitespace separated,
    # with no comments in what QEMU writes.
    parts = data.split(maxsplit=4)
    if parts[0] != b"P6":
        raise ValueError("not a P6 ppm: %s" % path)
    w, h, maxval = int(parts[1]), int(parts[2]), int(parts[3])
    if maxval != 255:
        raise ValueError("unsupported maxval %d" % maxval)
    pix = parts[4]
    return w, h, pix[: w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def grab(sock_path):
    """Screendump to a temporary PPM and return (w, h, rgb)."""
    fd, tmp = tempfile.mkstemp(suffix=".ppm")
    os.close(fd)
    try:
        hmp(sock_path, "screendump %s" % tmp)
        # The dump is written by the QEMU process; give it a moment to land.
        for _ in range(20):
            if os.path.getsize(tmp) > 0:
                break
            time.sleep(0.1)
        return read_ppm(tmp)
    finally:
        os.unlink(tmp)


def share(rgb, colour):
    target = bytes(colour)
    n = len(rgb) // 3
    if n == 0:
        return 0.0
    # Sample every 7th pixel; exact counts are not needed to tell a desktop
    # from a Setup screen, and a full pass over 800x600 in python is slow.
    hits = total = 0
    for i in range(0, n, 7):
        total += 1
        if rgb[i * 3:i * 3 + 3] == target:
            hits += 1
    return hits / total


def taskbar(w, h, rgb):
    """Share of the row 6 pixels above the bottom edge that is button grey.

    The taskbar is 28 pixels tall at every resolution 9x starts in, and six
    up from the bottom is inside it and below the buttons' bevel on its
    top edge; the Start button and the tray are grey there too, apart from
    their text.
    """
    y = h - 6
    row = rgb[y * w * 3:(y + 1) * w * 3]
    grey = bytes(GREY)
    return sum(1 for x in range(w) if row[x * 3:x * 3 + 3] == grey) / w


KEYMAP = {" ": "spc", ".": "dot", "\\": "backslash", ":": "shift-semicolon",
          "-": "minus", "/": "slash", "_": "shift-minus"}


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    op, sock = argv[1], argv[2]
    if op == "cmd":
        print(hmp(sock, argv[3]))
    elif op == "shot":
        w, h, rgb = grab(sock)
        write_png(argv[3], w, h, rgb)
        print("%s %dx%d" % (argv[3], w, h))
    elif op == "keys":
        for k in argv[3:]:
            hmp(sock, "sendkey %s" % k)
            time.sleep(0.15)
    elif op == "type":
        for ch in argv[3]:
            k = KEYMAP.get(ch)
            if k is None:
                k = ("shift-" + ch.lower()) if ch.isupper() else ch
            hmp(sock, "sendkey %s" % k)
            time.sleep(0.08)
    elif op == "share":
        _, _, rgb = grab(sock)
        print("%.3f" % share(rgb, tuple(int(x) for x in argv[3:6])))
    elif op == "desktop":
        w, h, rgb = grab(sock)
        frac = share(rgb, TEAL)
        bar = taskbar(w, h, rgb)
        print("%.3f teal, %.3f taskbar" % (frac, bar))
        return 0 if bar > 0.6 or frac > 0.35 else 1
    elif op == "still":
        w, h, rgb = grab(sock)
        state = argv[3]
        prev = open(state, "rb").read() if os.path.exists(state) else b""
        with open(state, "wb") as f:
            f.write(rgb)
        if len(prev) != len(rgb):
            print("1.000")
            return 1
        # A blinking caret or a spinning hourglass is a few hundred pixels;
        # a page that moved on is most of the screen.
        n = len(rgb) // 3
        diff = sum(1 for i in range(0, n, 3)
                   if rgb[i * 3:i * 3 + 3] != prev[i * 3:i * 3 + 3])
        frac = diff / ((n + 2) // 3)
        print("%.4f" % frac)
        return 0 if frac < 0.005 else 1
    else:
        sys.stderr.write("unknown op %s\n" % op)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
