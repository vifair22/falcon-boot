#!/usr/bin/env bash
#
# patch-bootwim.sh - Linux-side injection of a driver + startnet hook into a
# Windows install ISO's \sources\boot.wim (WinPE), then a size-preserving
# overlay of the patched WIM back into the ISO in place.
#
# This is the "bless-time" WinPE patch feasibility spike for falcon-boot. It
# proves the whole preparation is Linux-only: no Windows host, no ISO rewrite.
#
# The design hinges on two measured facts (DESIGN Section 6.3):
#   1. wimlib rewrites boot.wim smaller than Microsoft ships it (garbage
#      collection of slack plus better LZX), so after injecting a small signed
#      driver the patched WIM is still <= the original byte length.
#   2. A WIM self-describes its real size in its 208-byte header; trailing
#      bytes are ignored. So the patched WIM can be zero-padded to the EXACT
#      original length and dropped into the ISO's contiguous boot.wim extent
#      without disturbing the ISO9660/UDF directory entries (the file length
#      they record is unchanged). No 7.7 GB rewrite.
#
# Requirements: wimlib-imagex (wimupdate, wimoptimize, wiminfo), python3,
# a loop-mount-capable environment (sudo mount -o loop,ro) OR pre-extracted
# boot.wim. cryptsetup/busybox-class appliance has ample RAM headroom: peak
# RSS measured at 23-105 MB, scratch need ~2x boot.wim (~1.2 GB).
#
# The injected driver MUST already be signed with a certificate WinPE trusts
# (e.g. WHQL-signed VirtIO). Secure Boot verifies winload.efi inside boot.wim,
# not the WIM container, and WinPE refuses unsigned kernel-mode drivers.
#
# Usage:
#   patch-bootwim.sh --iso IN.iso --driver drv.sys --startnet startnet.cmd \
#                    [--image N] [--recompress] [--out OUT.iso]
#
# With --out the ISO is copied first and patched in the copy; without it the
# input ISO is overlaid in place (make your own backup).

set -euo pipefail

die() { printf 'patch-bootwim: %s\n' "$*" >&2; exit 1; }

ISO="" DRIVER="" STARTNET="" IMAGE=1 RECOMPRESS=0 OUT="" WORK=""
DEST_DIR='/Windows/System32'

while [ $# -gt 0 ]; do
    case "$1" in
        --iso)       ISO="$2"; shift 2;;
        --driver)    DRIVER="$2"; shift 2;;
        --startnet)  STARTNET="$2"; shift 2;;
        --image)     IMAGE="$2"; shift 2;;
        --recompress) RECOMPRESS=1; shift;;
        --out)       OUT="$2"; shift 2;;
        --workdir)   WORK="$2"; shift 2;;
        -h|--help)   sed -n '2,40p' "$0"; exit 0;;
        *)           die "unknown argument: $1";;
    esac
done

[ -n "$ISO" ] || die "missing --iso"
[ -f "$ISO" ] || die "no such ISO: $ISO"
command -v wimupdate   >/dev/null || die "wimlib not found (need wimupdate/wimoptimize/wiminfo)"
command -v wimoptimize >/dev/null || die "wimlib wimoptimize not found"
command -v python3     >/dev/null || die "python3 not found"

WORK="${WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
cleanup() {
    if mountpoint -q "$WORK/mnt" 2>/dev/null; then sudo umount "$WORK/mnt" || true; fi
}
trap cleanup EXIT

TARGET="$ISO"
if [ -n "$OUT" ]; then
    printf 'copying %s -> %s\n' "$ISO" "$OUT"
    cp --reflink=auto "$ISO" "$OUT"
    TARGET="$OUT"
fi

# 1. Locate the boot.wim extent inside the ISO. Windows media is UDF-bridge;
#    both filesystem views share one contiguous data extent, so a single
#    overlay updates both. We find it by the WIM magic at a sector boundary
#    and confirm length against the extracted copy.
mkdir -p "$WORK/mnt"
sudo mount -o loop,ro "$TARGET" "$WORK/mnt"
[ -f "$WORK/mnt/sources/boot.wim" ] || die "no \\sources\\boot.wim in ISO"
ORIG_LEN=$(stat -c%s "$WORK/mnt/sources/boot.wim")
cp "$WORK/mnt/sources/boot.wim" "$WORK/boot.wim"
sudo umount "$WORK/mnt"

ISO_OFF=$(python3 - "$TARGET" "$WORK/boot.wim" "$ORIG_LEN" <<'PY'
import sys, mmap, os
iso, wim, olen = sys.argv[1], sys.argv[2], int(sys.argv[3])
head = open(wim, 'rb').read(4096)
f = open(iso, 'rb'); mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
i = mm.find(b'MSWIM\x00\x00\x00')
while i != -1:
    if i % 2048 == 0 and mm[i:i+4096] == head:
        sys.stdout.write(str(i)); break
    i = mm.find(b'MSWIM\x00\x00\x00', i + 8)
else:
    sys.exit("could not locate boot.wim extent in ISO")
mm.close()
PY
)
printf 'boot.wim: ISO offset %s, length %s\n' "$ISO_OFF" "$ORIG_LEN"

# 2. Inject. startnet.cmd already exists in WinPE; adding it again replaces it.
chmod u+w "$WORK/boot.wim"
UPD="$WORK/update.txt"; : > "$UPD"
if [ -n "$DRIVER" ]; then
    [ -f "$DRIVER" ] || die "no such driver: $DRIVER"
    printf 'add %s %s/%s\n' "$DRIVER" "$DEST_DIR" "$(basename "$DRIVER")" >> "$UPD"
fi
if [ -n "$STARTNET" ]; then
    [ -f "$STARTNET" ] || die "no such startnet: $STARTNET"
    printf 'add %s %s/startnet.cmd\n' "$STARTNET" "$DEST_DIR" >> "$UPD"
fi
[ -s "$UPD" ] || die "nothing to inject (need --driver and/or --startnet)"
wimupdate "$WORK/boot.wim" "$IMAGE" < "$UPD"

# 3. Reclaim slack. Plain optimize reuses existing compressed blobs and is
#    near-instant; --recompress spends CPU for ~6 MB more headroom.
if [ "$RECOMPRESS" -eq 1 ]; then
    wimoptimize --recompress "$WORK/boot.wim"
else
    wimoptimize "$WORK/boot.wim"
fi

NEW_LEN=$(stat -c%s "$WORK/boot.wim")
printf 'patched boot.wim: %s bytes (original %s, delta %s)\n' \
    "$NEW_LEN" "$ORIG_LEN" "$((NEW_LEN - ORIG_LEN))"
if [ "$NEW_LEN" -gt "$ORIG_LEN" ]; then
    die "patched WIM ($NEW_LEN) exceeds original extent ($ORIG_LEN); overlay impossible. Retry with --recompress or shrink the payload, or fall back to a full ISO rebuild."
fi

# 4. Zero-pad to the exact original length so the overlay is byte-for-byte
#    the same size; the WIM header ignores the trailing padding.
PAD=$((ORIG_LEN - NEW_LEN))
if [ "$PAD" -gt 0 ]; then
    dd if=/dev/zero bs=1 count="$PAD" status=none >> "$WORK/boot.wim"
    printf 'padded %s trailing zero bytes\n' "$PAD"
fi

# 5. Overlay in place. seek is in whole 2048-byte sectors (extent is aligned).
[ $((ISO_OFF % 2048)) -eq 0 ] || die "boot.wim extent not sector-aligned"
dd if="$WORK/boot.wim" of="$TARGET" bs=2048 seek=$((ISO_OFF / 2048)) \
   conv=notrunc status=progress
sync
printf 'done: overlaid patched boot.wim into %s\n' "$TARGET"
