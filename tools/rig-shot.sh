#!/usr/bin/env bash
# falcon-boot: read the loader's screen out of a headless rig run.
#
# The loader draws through Simple Text Output, which OVMF routes to the
# graphics console and NOT to the serial line. build/serial.log therefore
# carries a handful of ANSI control codes and nothing a person can read, so a
# headless run has no way to check what the menu SAYS without this.
#
# QEMU's monitor will dump the framebuffer as a PPM. Nothing on this machine
# converts one (no ImageMagick, no PIL), and a PNG is what a reader can open,
# so the converter is inlined below: PNG is a zlib stream plus three CRC'd
# chunks, which is less code than depending on something that has to be
# installed first.
#
# Usage: tools/rig-shot.sh NAME [OUTDIR]
#   NAME    basename for the .ppm and .png (default "shot")
#   OUTDIR  where to write them (default build/shots)
#
# Env: MON_SOCK  QEMU monitor socket (default build/mon.sock, as run-qemu.sh
#                --headless creates it).

set -euo pipefail

NAME="${1:-shot}"
OUT="${2:-build/shots}"
MON_SOCK="${MON_SOCK:-build/mon.sock}"

[[ -S "$MON_SOCK" ]] || { echo "ERROR: no monitor socket at $MON_SOCK" >&2; exit 1; }
command -v socat >/dev/null || { echo "ERROR: socat not found" >&2; exit 1; }

mkdir -p "$OUT"
PPM="$OUT/$NAME.ppm"
PNG="$OUT/$NAME.png"

# Absolute, because the path is interpreted by QEMU and not by this shell.
echo "screendump $(readlink -f "$OUT")/$NAME.ppm" |
    socat - UNIX-CONNECT:"$MON_SOCK" >/dev/null
sleep 0.5
[[ -s "$PPM" ]] || { echo "ERROR: no framebuffer written to $PPM" >&2; exit 1; }

python3 - "$PPM" "$PNG" <<'PY'
import sys, zlib, struct

def read_ppm(path):
    d = open(path, 'rb').read()
    if d[:2] != b'P6':
        raise SystemExit('not a binary PPM: ' + path)
    # Width, height and maxval, any of which a writer may separate with
    # comments or arbitrary whitespace.
    fields, i = [], 2
    while len(fields) < 3:
        while d[i:i+1].isspace():
            i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] not in (b'\n', b'\r'):
                i += 1
            continue
        j = i
        while not d[j:j+1].isspace():
            j += 1
        fields.append(int(d[i:j]))
        i = j
    i += 1                                  # the single separator after maxval
    w, h, _ = fields
    return w, h, d[i:i + w * h * 3]

def write_png(path, w, h, rgb):
    # Filter type 0 on every scanline: the framebuffer is small and this is
    # not a place to spend effort on compression.
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(tag, data):
        body = tag + data
        return (struct.pack('>I', len(data)) + body +
                struct.pack('>I', zlib.crc32(body) & 0xffffffff))
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)

w, h, rgb = read_ppm(sys.argv[1])
write_png(sys.argv[2], w, h, rgb)
print('%dx%d -> %s' % (w, h, sys.argv[2]))
PY
