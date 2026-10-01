#!/usr/bin/env bash
# falcon-boot: build the WIM fixture falcon-core/src/wim is tested against
# (DESIGN Section 6.3).
#
# wimlib writes it, LZX-compressed in 32 KiB chunks like Microsoft's own
# boot.wim, with two images and the second marked bootable, which is the
# shape Windows install media has. The tree gives the patcher what it looks
# for (Windows/System32/winpeshl.exe) among enough siblings that the
# directory's dentry list is more than a token one, and gives the
# decompressor both compressible text and incompressible random data (the
# latter makes wimlib store chunks raw), plus a file whose E8 bytes exercise
# LZX's call translation.
#
# Outputs: build/test-wim.wim, and build/test-wim.jump (the stand-in for the
# jump binary the patch installs).

set -euo pipefail

cd "$(dirname "$0")/../.."
command -v wimlib-imagex >/dev/null || {
    echo "ERROR: wimlib-imagex not found" >&2; exit 1; }

mkdir -p build
TREE=$(mktemp -d)
trap 'rm -rf "$TREE"' EXIT

mkdir -p "$TREE/Windows/System32/drivers" "$TREE/sources" "$TREE/Program Files"
printf 'the original winpeshl.exe\n' > "$TREE/Windows/System32/winpeshl.exe"
for i in $(seq -w 1 300); do
    printf 'file %s of System32\n' "$i" > "$TREE/Windows/System32/lib$i.dll"
done
python3 -c "
import sys
line = b'compressible line of text for the LZX literal and match paths\n'
sys.stdout.buffer.write((line * 4000)[:200 * 1024])" \
    > "$TREE/Windows/System32/big.txt"
head -c $((100 * 1024)) /dev/urandom > "$TREE/Windows/System32/random.bin"
# E8 is the x86 CALL opcode LZX translates; a run of them with offsets.
python3 -c "
import os, struct, sys
b = bytearray()
for i in range(20000):
    b += b'\xe8' + struct.pack('<i', (i * 7919) % 100000 - 50000)
sys.stdout.buffer.write(bytes(b))" > "$TREE/Windows/System32/calls.bin"
printf 'setup\n' > "$TREE/sources/setup.exe"
printf 'driver\n' > "$TREE/Windows/System32/drivers/disk.sys"

rm -f build/test-wim.wim
wimlib-imagex capture "$TREE" build/test-wim.wim "WinPE" --compress=LZX \
    --chunk-size=32768 >/dev/null
printf 'setup image\n' > "$TREE/sources/setup2.exe"
wimlib-imagex append "$TREE" build/test-wim.wim "Setup" --boot >/dev/null
printf 'MZ the jump binary stand-in\n' > build/test-wim.jump
wimlib-imagex info build/test-wim.wim | grep -E "Boot Index|Image Count|Compression"
