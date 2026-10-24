#!/usr/bin/env bash
# falcon-boot: build the rig stick the coverage harness boots one ISO from.
#
# One stick per ISO, so the menu the harness drives has exactly one image row
# and that row is the first one: no p3 means no pinned personal-OS entry, and
# the firmware-setup row always sits last. The harness presses Enter (or the
# route's key) on row 0 and nothing needs to find anything on screen.
#
#   p1  ESP    FAT32  64 MiB  shim -> mmx64.efi -> our loader as grubx64.efi;
#                             the boot record, and BIOS stage 3 in its
#                             reserved sectors
#   p2  ISOs   exFAT  ISO + slack, basic-data typed, /.falcon (role library)
#
# The layout and the identity records are DESIGN Section 3's, the same as
# falcon-efi/tools/mk-test-stick.sh, minus p3/p4: nothing the harness measures
# involves the personal OS, and an absent shelf keeps the menu at one row.
# Both records carry one install ID, drawn per stick.
#
# Two outputs, both sparse:
#
#   OUT.img          p2 holds the ISO and nothing else. The drag-and-drop and
#                    chainload routes boot this one.
#   OUT.blessed.img  a reflink copy with the ISO blessed by the real sign tool
#                    and the test MOK (--blessed). The blessed route boots it.
#                    The wrap's cmdline gets console=ttyS0 appended, so a
#                    blessed boot is judged on the serial log as well as the
#                    network.
#
# p1 is written with mtools straight into the image, so tools/coverage/
# stage-loader.sh can refresh the loader in every stick without sudo, a loop
# device or rebuilding p2. That is also why the loader is not staged here,
# and nor are the BIOS stages: stage-loader.sh puts both on, so the bios
# columns boot the case stick alone, as a real stick boots (DESIGN Section
# 3.3). p1's reserved sectors are sized by mkstick's rule for them.
#
# Usage: tools/coverage/mk-case-stick.sh ISO OUT [--blessed] [--bless-arg ARG]...
#   ISO          the image to put on p2, under its own file name
#   OUT          output path without the .img suffix
#   --blessed    also produce OUT.blessed.img
#   --reuse      keep an existing OUT.img and only (re)make the blessed copy
#   --arm64      an arm64 ISO: the blessed wrap's serial console is virt's
#                ttyAMA0
#   --bless-arg  passed through to falcon-boot-sign (repeatable), e.g.
#                --bless-arg --kernel --bless-arg 'boot/vmlinuz*'
#
# Needs sgdisk, mformat, mkfs.exfat, losetup, mkstick and the BIOS stage 3
# (make cli bios) and passwordless sudo (loop mounting p2). Everything lands where OUT says, which by default is under
# build/coverage/ and therefore on whatever build/ is mounted from.

set -euo pipefail

cd "$(dirname "$0")/../.."
. tools/rig-part.sh

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

[[ $# -ge 2 ]] || { usage >&2; exit 1; }
ISO=$1
OUT=$2
shift 2

BLESSED=0
REUSE=0
ARM64=0
BLESS_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
    --blessed) BLESSED=1; shift ;;
    --reuse) REUSE=1; shift ;;
    --arm64) ARM64=1; shift ;;
    --bless-arg)
        [[ $# -ge 2 ]] || { echo "ERROR: --bless-arg needs a value" >&2; exit 1; }
        BLESS_ARGS+=("$2")
        shift 2
        ;;
    *) echo "ERROR: unknown option $1" >&2; exit 1 ;;
    esac
done

SIGN=build/bin/falcon-boot-sign
MKSTICK=build/bin/falcon-boot-mkstick
STAGE3=build/bin/falcon-bios-stage3.bin
MOK_KEY=build/mok-test/falcon-test-mok.key
MOK_CRT=build/mok-test/falcon-test-mok.crt

# The serial console for a blessed boot. ttyS0 is LAST, because the last
# console= is /dev/console: the initramfs's messages and its rescue shell go
# there and nowhere else, and a harness that cannot read "could not find the
# ISO" can only time out. The kernel still prints to both.
BLESS_CONSOLE="console=tty0 console=ttyS0,115200"
[[ $ARM64 -eq 1 ]] && BLESS_CONSOLE="console=tty0 console=ttyAMA0,115200"

[[ -f "$ISO" ]] || { echo "ERROR: $ISO not found" >&2; exit 1; }
for t in sgdisk mformat mkfs.exfat losetup; do
    command -v "$t" >/dev/null 2>&1 || { echo "ERROR: $t not found" >&2; exit 1; }
done
for f in "$MKSTICK" "$STAGE3"; do
    [[ -f "$f" ]] || { echo "ERROR: $f not built (run: make cli bios)" >&2; exit 1; }
done
if [[ "$BLESSED" -eq 1 ]]; then
    [[ -x "$SIGN" ]] || { echo "ERROR: $SIGN not built (run: make cli)" >&2; exit 1; }
    [[ -f "$MOK_KEY" ]] || {
        echo "ERROR: no test MOK (run: falcon-efi/tools/mk-test-mok.sh)" >&2; exit 1; }
fi

NAME=$(basename "$ISO")
IMG="$OUT.img"
mkdir -p "$(dirname "$OUT")"

LOOP=
MNT=
cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
        MNT=
    fi
    if [[ -n "$LOOP" ]]; then
        sudo losetup -d "$LOOP" 2>/dev/null || true
        LOOP=
    fi
}
trap cleanup EXIT

# p2 is partition 2 while it is being made, and the partition whose record
# says library once it has one (DESIGN Section 3.1).
mount_p2() {   # mount_p2 IMG [--format]
    local n=2

    [[ "${2:-}" == --format ]] || n=$(rig_part "$1" library)
    LOOP=$(sudo losetup --show -f -P "$1")
    sudo udevadm settle 2>/dev/null || sleep 1
    if [[ "${2:-}" == --format ]]; then
        sudo mkfs.exfat -n FALCON-ISO "${LOOP}p$n" >/dev/null
    fi
    MNT=$(mktemp -d)
    sudo mount -o uid="$(id -u)",gid="$(id -g)" "${LOOP}p$n" "$MNT"
}

unmount_p2() {
    sync
    sudo umount "$MNT"
    rmdir "$MNT"
    MNT=
    sudo losetup -d "$LOOP"
    LOOP=
}

# p2 gets the ISO plus room for a blessed wrap, whose embedded initramfs is
# the largest thing the sign tool writes (Ubuntu's is over 100 MiB), and for
# exFAT's own metadata.
ISO_MIB=$(( ($(stat -c%s "$ISO") + 1048575) / 1048576 ))
P2_MIB=$(( ISO_MIB + 512 ))
SIZE_MIB=$(( 1 + 64 + P2_MIB + 1 ))

rm -f "$OUT.blessed.img"
if [[ "$REUSE" -eq 0 || ! -f "$IMG" ]]; then
rm -f "$IMG"
truncate -s "${SIZE_MIB}M" "$IMG"
sgdisk \
    -n 1:1MiB:+64MiB -t 1:ef00 -c 1:"FALCON-ESP" \
    -n 2:0:0         -t 2:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 \
                     -c 2:"FALCON-ISO" \
    "$IMG" >/dev/null

# p1 starts at 1 MiB; mtools addresses a partition inside an image by byte
# offset. FAT32 because that is what a real ESP is, with reserved sectors
# for BIOS stage 3 and the marker (-R), and the boot record written into its
# boot sector by mkstick, which is how stage 2 and the loaders find it.
INSTALL_ID=$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')
mformat -i "$IMG@@1M" -F -v FALCONESP -T $((64 * 2048)) -h 64 -s 32 \
    -R "$("$MKSTICK" ident --reserved-sectors --stage3 "$STAGE3")" :: \
    >/dev/null
"$MKSTICK" --quiet ident "$IMG" --write-boot 1 --id "$INSTALL_ID" >/dev/null

mount_p2 "$IMG" --format
cp "$ISO" "$MNT/$NAME"
printf 'falcon 1 library %s\n' "$INSTALL_ID" > "$MNT/.falcon"
unmount_p2
echo "   $IMG: $NAME on p2 (${P2_MIB} MiB), install ID $INSTALL_ID"
fi

[[ "$BLESSED" -eq 1 ]] || exit 0

# The blessed copy shares every extent of the ISO with the plain one; only the
# wrap and the manifest are new bytes.
cp --reflink=auto "$IMG" "$OUT.blessed.img"
mount_p2 "$OUT.blessed.img"
if ! "$SIGN" "$MNT/$NAME" --stick "$MNT" --key "$MOK_KEY" --cert "$MOK_CRT" \
        --cmdline-extra "$BLESS_CONSOLE" "${BLESS_ARGS[@]}" \
        --work "$(dirname "$OUT")" --quiet; then
    unmount_p2
    rm -f "$OUT.blessed.img"
    echo "   bless refused or failed; no blessed stick for $NAME" >&2
    exit 2
fi
unmount_p2
echo "   $OUT.blessed.img: blessed"
