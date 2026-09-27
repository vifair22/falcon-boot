#!/usr/bin/env bash
# falcon-boot: make the station rig's FreeDOS fixture disk.
#
# FreeDOS 1.4's Lite USB edition (FD14-LiteUSB.zip, FD14LITE.img inside) is
# already a bootable hard disk: an MBR with FreeDOS's boot code, one active
# FAT16 primary partition and the kernel on it. What it boots to is the
# installer, because FDAUTO.BAT ends by calling SETUP.BAT when that file
# exists. Renaming SETUP.BAT is the whole change, and FDAUTO.BAT then falls
# through to a plain C:\> prompt. The installer's files stay on the disk, so
# `SETUP.OLD` is still there if someone wants the full install.
#
# The zip is checked against the SHA-256 the release publishes in verify.txt
# before anything is taken out of it.
#
# Usage: mk-freedos-fixture.sh FD14-LiteUSB.zip verify.txt OUT.img
# Needs python3 and mtools.

set -euo pipefail

[[ $# -eq 3 ]] || { echo "usage: $0 FD14-LiteUSB.zip verify.txt OUT.img" >&2; exit 1; }
ZIP=$1
VERIFY=$2
OUT=$3

want=$(awk -v f="$(basename "$ZIP")" 'length($1) == 64 && $2 == f { print $1 }' "$VERIFY")
have=$(sha256sum "$ZIP" | cut -d' ' -f1)
[[ -n $want ]] || { echo "ERROR: no SHA-256 for $(basename "$ZIP") in $VERIFY" >&2; exit 1; }
[[ $want == "$have" ]] || { echo "ERROR: $ZIP sha256 $have, published $want" >&2; exit 1; }
echo "$(basename "$ZIP"): sha256 matches verify.txt"

python3 - "$ZIP" "$OUT" <<'EOF'
import shutil, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z, z.open("FD14LITE.img") as src, \
        open(sys.argv[2], "wb") as dst:
    shutil.copyfileobj(src, dst)
EOF

# The partition's start comes from the MBR rather than a constant, so a
# point release that moves it does not silently edit the wrong bytes.
start=$(python3 -c '
import struct, sys
mbr = open(sys.argv[1], "rb").read(512)
print(struct.unpack_from("<I", mbr, 446 + 8)[0])' "$OUT")
part="$OUT@@$((start * 512))"

export MTOOLS_SKIP_CHECK=1
mren -i "$part" ::/SETUP.BAT ::/SETUP.OLD
mdir -i "$part" ::/ | tr -d '\r'
echo "wrote $OUT"
