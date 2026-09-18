#!/usr/bin/env bash
#
# patch-winjump.sh - inject the WinPE jump binary into a Windows install ISO's
# \sources\boot.wim, Linux-side, and overlay the result back into the ISO in
# place at its original byte length.
#
# This is the rig-side stand-in for what DESIGN Section 6.3 will have the
# bless tool do. The difference is deliberate and is the whole reason this
# proof needs no loader changes: bless will compute an override list and serve
# the patched WIM from a sidecar, leaving the user's ISO untouched, whereas
# this script writes the same bytes straight into the ISO. The bytes WinPE
# sees are identical either way, so what boots here is what will boot there.
#
# The three injected entries, all in \Windows\System32 of one WIM image:
#   winpeshl.exe        <- our jump binary (replaces Microsoft's)
#   winpeshl_orig.exe   <- Microsoft's, preserved for the jump binary to run
#   falcon-winjump.cfg  <- where the ISO lives and how big it is
#
# Size discipline, from falcon-efi/tools/win-spike/patch-bootwim.sh which proved
# it: a WIM self-describes its length in its header and ignores trailing bytes,
# so the patched WIM is zero-padded back to the original's exact length and the
# overlay disturbs no ISO 9660 or UDF metadata at all. The script refuses rather
# than truncates if the patched WIM does not fit.
#
# Usage:
#   patch-winjump.sh --img DISK.img --rel Windows/win11.iso \
#                    --jump build/bin/winjump.exe [--image N] [--recompress]
#   patch-winjump.sh --iso PATH.iso --rel Windows/win11.iso \
#                    --jump build/bin/winjump.exe [--image N] [--recompress]
#
# --img mounts the falcon p2 image and patches the ISO named by --rel inside
# it; --iso patches an ISO file directly. --rel is what goes into the config
# either way, because it is the path the jump binary will look for from
# inside WinPE.

set -euo pipefail

cd "$(dirname "$0")/../../.."

die() { printf 'patch-winjump: %s\n' "$*" >&2; exit 1; }

IMG="" ISO="" REL="" JUMP="build/bin/winjump.exe" IMAGE=2 RECOMPRESS=0
WORK="build/win-spike/work"

while [ $# -gt 0 ]; do
    case "$1" in
        --img)        IMG="$2"; shift 2;;
        --iso)        ISO="$2"; shift 2;;
        --rel)        REL="$2"; shift 2;;
        --jump)       JUMP="$2"; shift 2;;
        --image)      IMAGE="$2"; shift 2;;
        --recompress) RECOMPRESS=1; shift;;
        --workdir)    WORK="$2"; shift 2;;
        -h|--help)    sed -n '2,36p' "$0"; exit 0;;
        *)            die "unknown argument: $1";;
    esac
done

[ -n "$REL" ]    || die "missing --rel (the ISO's path relative to p2's root)"
[ -f "$JUMP" ]   || die "no such jump binary: $JUMP (run: make winjump)"
command -v wimupdate   >/dev/null || die "wimlib not found (need wimupdate)"
command -v wimoptimize >/dev/null || die "wimlib wimoptimize not found"
command -v python3     >/dev/null || die "python3 not found"

mkdir -p "$WORK"
MNT="$WORK/p2mnt"
LOOP=""

cleanup() {
    mountpoint -q "$MNT" 2>/dev/null && sudo umount "$MNT" || true
    [ -n "$LOOP" ] && sudo losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT

if [ -n "$IMG" ]; then
    [ -f "$IMG" ] || die "no such image: $IMG"
    mkdir -p "$MNT"
    LOOP=$(sudo losetup --find --show --partscan "$IMG")
    sudo udevadm settle 2>/dev/null || sleep 1
    sudo mount -o uid="$(id -u)",gid="$(id -g)" "${LOOP}p1" "$MNT"
    ISO="$MNT/$REL"
fi

[ -n "$ISO" ] || die "need --img or --iso"
[ -f "$ISO" ] || die "no such ISO: $ISO"

ISO_SIZE=$(stat -c%s "$ISO")
printf 'ISO: %s (%s bytes)\n' "$ISO" "$ISO_SIZE"

# 1. Pull boot.wim out and record the extent it occupies in the ISO. Windows
#    media keeps it in one contiguous, sector-aligned run; the WIM header is
#    matched rather than the magic alone so a second WIM cannot be picked up.
ISOMNT="$WORK/isomnt"
mkdir -p "$ISOMNT"
sudo mount -o loop,ro "$ISO" "$ISOMNT"
[ -f "$ISOMNT/sources/boot.wim" ] || { sudo umount "$ISOMNT"; die "no \\sources\\boot.wim"; }
ORIG_LEN=$(stat -c%s "$ISOMNT/sources/boot.wim")
cp "$ISOMNT/sources/boot.wim" "$WORK/boot.wim"
sudo umount "$ISOMNT"
rmdir "$ISOMNT"
chmod u+w "$WORK/boot.wim"

ISO_OFF=$(python3 - "$ISO" "$WORK/boot.wim" <<'PY'
import sys, mmap
iso, wim = sys.argv[1], sys.argv[2]
head = open(wim, 'rb').read(4096)
with open(iso, 'rb') as f:
    mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    i = mm.find(b'MSWIM\x00\x00\x00')
    while i != -1:
        if i % 2048 == 0 and mm[i:i + 4096] == head:
            print(i)
            break
        i = mm.find(b'MSWIM\x00\x00\x00', i + 8)
    else:
        sys.exit("could not locate boot.wim extent in ISO")
    mm.close()
PY
)
printf 'boot.wim: ISO offset %s, length %s, image %s\n' "$ISO_OFF" "$ORIG_LEN" "$IMAGE"
[ $((ISO_OFF % 2048)) -eq 0 ] || die "boot.wim extent not sector-aligned"

# 2. Preserve Microsoft's winpeshl.exe before overwriting it. It is the thing
#    the jump binary hands control back to, so losing it means no Setup.
rm -rf "$WORK/orig"
mkdir -p "$WORK/orig"
wimlib-imagex extract "$WORK/boot.wim" "$IMAGE" \
    /Windows/System32/winpeshl.exe --dest-dir="$WORK/orig" \
    --no-acls >/dev/null
[ -f "$WORK/orig/winpeshl.exe" ] || die "could not extract winpeshl.exe from image $IMAGE"
printf 'original winpeshl.exe: %s bytes\n' "$(stat -c%s "$WORK/orig/winpeshl.exe")"

# 3. The config. ASCII, key=value; the size is the tiebreak the jump binary
#    uses when more than one volume offers the same relative path.
cat > "$WORK/falcon-winjump.cfg" <<EOF
# falcon-boot winjump configuration, generated by
# falcon-efi/tools/win-spike/patch-winjump.sh
iso=$REL
size=$ISO_SIZE
EOF

# 4. Inject. A wimupdate 'add' onto an existing path replaces it in place.
cat > "$WORK/update.txt" <<EOF
add $WORK/orig/winpeshl.exe /Windows/System32/winpeshl_orig.exe
add $JUMP /Windows/System32/winpeshl.exe
add $WORK/falcon-winjump.cfg /Windows/System32/falcon-winjump.cfg
EOF
wimupdate "$WORK/boot.wim" "$IMAGE" --no-acls < "$WORK/update.txt"

# 5. Reclaim slack so the result fits back in the original footprint.
if [ "$RECOMPRESS" -eq 1 ]; then
    wimoptimize --recompress "$WORK/boot.wim"
else
    wimoptimize "$WORK/boot.wim"
fi

NEW_LEN=$(stat -c%s "$WORK/boot.wim")
printf 'patched boot.wim: %s bytes (original %s, delta %s)\n' \
    "$NEW_LEN" "$ORIG_LEN" "$((NEW_LEN - ORIG_LEN))"
[ "$NEW_LEN" -le "$ORIG_LEN" ] || \
    die "patched WIM ($NEW_LEN) exceeds the original extent ($ORIG_LEN); retry with --recompress"

PAD=$((ORIG_LEN - NEW_LEN))
if [ "$PAD" -gt 0 ]; then
    truncate -s "$ORIG_LEN" "$WORK/boot.wim"
    printf 'padded %s trailing zero bytes -> %s\n' "$PAD" "$(stat -c%s "$WORK/boot.wim")"
fi
wimverify "$WORK/boot.wim" >/dev/null || die "padded WIM fails wimverify"
printf 'wimverify: OK\n'

# 6. Overlay in place.
dd if="$WORK/boot.wim" of="$ISO" bs=2048 seek=$((ISO_OFF / 2048)) \
   conv=notrunc status=none
sync
printf 'done: overlaid patched boot.wim into %s\n' "$ISO"
