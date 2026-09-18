#!/usr/bin/env bash
# falcon-boot: the hybrid ISO the bless tool's view is tested against.
#
# falcon-cli/src/sign/isoprobe.c reads an ISO through two helper processes and
# unions what they report: bsdtar for ISO9660 plus Rock Ridge, 7-Zip for UDF.
# Every test that builds a view from listing text in memory covers the parsing
# and the union and none of the plumbing, which is most of the file: spawning
# the helpers, picking the 7-Zip binary, building an extraction argv, reading a
# member through a pipe, and writing one out to disk.
#
# So this is a real image with both namespaces, made small enough to keep in
# build/ and shaped like an Arch install medium so the kernel-path table has
# something to find.
#
#   /arch/boot/x86_64/vmlinuz-linux     what iso_kernel_path looks up
#   /arch/boot/x86_64/initramfs-linux.img
#   /arch/boot/grub/grub.cfg
#   /EFI/BOOT/BOOTX64.EFI               a plausible loader to extract
#   /readme.txt                         known bytes, for a byte-for-byte read
#   /MixedCase/Thing.TXT                the case folding, against a real
#                                       namespace rather than a crafted string
#
# ISO9660 only, and that is not a shortfall. 7-Zip reads ISO9660 when an image
# has no UDF, so both helpers list this image and both reader paths run, which
# is what the plumbing needs. The Arch build of xorriso carries no UDF support
# and neither genisoimage nor mkisofs is present, so a two-namespace image
# cannot be built here at all; the disagreement between namespaces (the
# oscdimg shape that made this module necessary) stays with the in-memory
# listing tests. That split is the right one anyway: the disagreement is a
# parsing question and this is a plumbing one.
#
# Needs xorriso. No sudo: an ISO is built from a directory, not mounted.

set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=build/test-isoview.iso
LABEL=FALCON_ISOVIEW

command -v xorriso >/dev/null || {
    echo "ERROR: xorriso not found" >&2; exit 1; }

mkdir -p build
rm -f "$OUT"

TREE=$(mktemp -d)
trap 'rm -rf "$TREE"' EXIT

mkdir -p "$TREE/arch/boot/x86_64" "$TREE/arch/boot/grub" \
         "$TREE/EFI/BOOT" "$TREE/MixedCase"

# Deterministic content, so a test can assert on the bytes rather than only on
# the size. The kernel is padded past a single pipe read so probe_read's loop
# runs more than once.
printf 'falcon-test vmlinuz\n' > "$TREE/arch/boot/x86_64/vmlinuz-linux"
dd if=/dev/zero bs=1024 count=96 status=none \
    >> "$TREE/arch/boot/x86_64/vmlinuz-linux"
printf 'falcon-test initramfs\n' > "$TREE/arch/boot/x86_64/initramfs-linux.img"
printf 'set timeout=5\n' > "$TREE/arch/boot/grub/grub.cfg"
printf 'MZ falcon-test loader\n' > "$TREE/EFI/BOOT/BOOTX64.EFI"
printf 'falcon-boot isoview fixture\n' > "$TREE/readme.txt"
printf 'mixed case entry\n' > "$TREE/MixedCase/Thing.TXT"

xorriso -as mkisofs -quiet -V "$LABEL" -r -J -o "$OUT" "$TREE"

iso_n=$(bsdtar -tf "$OUT" 2>/dev/null | grep -c . || true)
udf_n=$(7z l -ba -slt "$OUT" 2>/dev/null | grep -c '^Path = ' || true)

echo "created $OUT ($(stat -c %s "$OUT") bytes, ISO9660, label $LABEL)"
echo "  bsdtar lists:  $iso_n entries"
echo "  7-Zip lists:   $udf_n entries"
