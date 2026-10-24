#!/usr/bin/env bash
# falcon-boot: build the corpus's UEFI shell ISO for AArch64.
#
# The EDK II shell is published as a bare EFI binary (Debian's efi-shell-aa64),
# and p2 holds ISOs and disk images, so the corpus row is that binary as
# \EFI\BOOT\BOOTAA64.EFI in an El Torito EFI boot image: the shape of every
# other ISO's EFI boot. The package is pinned by digest and every timestamp
# and serial in the output is fixed, so the result is the same file on every
# host and corpus.tsv can pin it. Run it in the harness
# (tools/coverage/harness.sh): another mtools or xorriso lays the image out
# differently, and the pin is the harness's.
#
# Usage: tools/coverage/mk-shell-iso.sh [OUT]
#   OUT   default $FALCON_CORPUS/efi-shell-aa64-2025.02.iso

set -euo pipefail

cd "$(dirname "$0")/../.."

PKG=efi-shell-aa64_2025.02-8+deb13u1_all.deb
SHA256=cf80c543b7b4595257e6a7481768466a352bc6f20927592e38f58a0c1ce3df85
URL=https://deb.debian.org/debian/pool/main/e/edk2/$PKG
OUT=${1:-${FALCON_CORPUS:-/srv/falcon/corpus}/efi-shell-aa64-2025.02.iso}
# 2025-02-01, the shell's release: the date on every file and on the volume.
EPOCH=1738368000
# mtools and xorriso both date what they make by it.
export SOURCE_DATE_EPOCH=$EPOCH
# FAT dates are local time.
export TZ=UTC

for t in curl bsdtar mformat mmd mcopy xorriso; do
    command -v "$t" >/dev/null || { echo "ERROR: $t not found" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -sSL --fail --max-time 300 -o "$work/$PKG" "$URL"
echo "$SHA256  $work/$PKG" | sha256sum -c --quiet - ||
    { echo "ERROR: $PKG is not the pinned build" >&2; exit 1; }
bsdtar -xOf "$work/$PKG" 'data.tar*' |
    bsdtar -xOf - ./usr/share/efi-shell-aa64/shellaa64.efi > "$work/shell.efi"
touch -d "@$EPOCH" "$work/shell.efi"

# The boot image: a FAT the size the shell needs plus room, a fixed serial
# and label, and mcopy -m keeping the fixed mtime.
mkdir -p "$work/root"
size_k=$(( ($(stat -c%s "$work/shell.efi") / 1024) + 512 ))
truncate -s "${size_k}K" "$work/root/efiboot.img"
mformat -i "$work/root/efiboot.img" -N 5eed5e11 -v SHELL ::
mmd -i "$work/root/efiboot.img" ::/EFI ::/EFI/BOOT
mcopy -m -i "$work/root/efiboot.img" "$work/shell.efi" ::/EFI/BOOT/BOOTAA64.EFI
touch -d "@$EPOCH" "$work/root/efiboot.img" "$work/root"

mkdir -p "$(dirname "$OUT")"
xorriso -as mkisofs -quiet -r -J -V EFI_SHELL_AA64 \
    --modification-date="$(date -u -d "@$EPOCH" +%Y%m%d%H%M%S00)" \
    -e efiboot.img -no-emul-boot -o "$OUT" "$work/root"
sha256sum "$OUT"
