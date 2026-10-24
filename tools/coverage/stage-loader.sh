#!/usr/bin/env bash
# falcon-boot: write the current loaders into harness sticks: the Secure Boot
# chain into p1, and falcon-bios's three stages where a stick keeps them.
#
# Every stick tools/coverage/mk-case-stick.sh builds carries the chains the
# product ships: Fedora's shim as \EFI\BOOT\BOOTX64.EFI, its MokManager, and
# our loader signed with the test MOK under shim's next-stage name, and the
# same three for AArch64 under BOOTAA64.EFI. Staging it
# separately from building the stick is what lets a loader change be measured
# against the whole corpus without copying a single ISO again: this rewrites
# three files in a 64 MiB FAT, through mtools, with no sudo.
#
# The BIOS stages go on the same way, with `mkstick bios` on the image file:
# stage 1 in LBA 0, stage 2 at the GPT's place in a FALCON-BIOS partition,
# stage 3 in p1's reserved sectors where its boot record says (DESIGN Section
# 3.3). So the bios columns boot the case stick as the only disk. p1 is the
# partition whose record says boot, found with tools/rig-part.sh.
#
# The same chain boots in every Secure Boot state the harness runs: shim
# with Secure Boot off loads the next stage without verifying it, which is the
# SB-off experience of a real stick too.
#
# Usage: tools/coverage/stage-loader.sh STICK.img...

set -euo pipefail

cd "$(dirname "$0")/../.."
. tools/coverage/states.sh
. tools/rig-part.sh

# The dual-signed shim (tools/coverage/states.sh), so one stick boots in all
# three states.
states_prepare
SHIM_EFI=$SHIM_DUAL
MM_EFI=build/shim/bins/fedora-16.1-mmx64.efi
EFI_PLAIN=build/bin/falcon-boot.efi
EFI_SIGNED=build/bin/falcon-boot-signed.efi
MOK_CER=build/mok-test/falcon-test-mok.cer
MKSTICK=build/bin/falcon-boot-mkstick
STAGE1=build/bin/falcon-bios-stage1.bin
STAGE2=build/bin/falcon-bios-stage2.bin
STAGE3=build/bin/falcon-bios-stage3.bin
# The AArch64 chain beside it, for the a64 columns: shimaa64 dual-signed the
# same way, its MokManager, and the aa64 loader.
MM_EFI_AA64=build/shim/bins/fedora-16.1-mmaa64.efi
EFI_PLAIN_AA64=build/bin/falcon-boot-aa64.efi
EFI_SIGNED_AA64=build/bin/falcon-boot-aa64-signed.efi

for f in "$MM_EFI" "$EFI_SIGNED"; do
    [[ -f "$f" ]] || { echo "ERROR: $f missing (make sign; fetch-shims.sh)" >&2; exit 1; }
done
for f in "$SHIM_DUAL_AA64" "$MM_EFI_AA64" "$EFI_SIGNED_AA64"; do
    [[ -f "$f" ]] || { echo "ERROR: $f missing (make aa64 sign; fetch-shims.sh)" >&2; exit 1; }
done
if [[ "$EFI_PLAIN_AA64" -nt "$EFI_SIGNED_AA64" ]]; then
    echo "ERROR: $EFI_SIGNED_AA64 is older than $EFI_PLAIN_AA64 (run: make sign)" >&2
    exit 1
fi
for f in "$MKSTICK" "$STAGE1" "$STAGE2" "$STAGE3"; do
    [[ -f "$f" ]] || { echo "ERROR: $f missing (make cli bios)" >&2; exit 1; }
done
# A signed loader older than the unsigned one is a previous build, and
# measuring it would report on code that is no longer in the tree.
if [[ "$EFI_PLAIN" -nt "$EFI_SIGNED" ]]; then
    echo "ERROR: $EFI_SIGNED is older than $EFI_PLAIN (run: make sign)" >&2
    exit 1
fi

for img in "$@"; do
    # A stick made before the boot record existed has nowhere for stage 3;
    # it is rebuilt, not patched (delete it and run-corpus.sh remakes it).
    start=$(rig_part_start "$img" boot) || {
        echo "ERROR: $img has no boot record; rebuild it with mk-case-stick.sh" >&2
        exit 1
    }
    fat="$img@@$(( start * 512 ))"
    mmd -D s -i "$fat" ::/EFI 2>/dev/null || true
    mmd -D s -i "$fat" ::/EFI/BOOT 2>/dev/null || true
    mcopy -o -i "$fat" "$SHIM_EFI" ::/EFI/BOOT/BOOTX64.EFI
    mcopy -o -i "$fat" "$MM_EFI" ::/EFI/BOOT/mmx64.efi
    mcopy -o -i "$fat" "$EFI_SIGNED" ::/EFI/BOOT/grubx64.efi
    mcopy -o -i "$fat" "$SHIM_DUAL_AA64" ::/EFI/BOOT/BOOTAA64.EFI
    mcopy -o -i "$fat" "$MM_EFI_AA64" ::/EFI/BOOT/mmaa64.efi
    mcopy -o -i "$fat" "$EFI_SIGNED_AA64" ::/EFI/BOOT/grubaa64.efi
    [[ -f "$MOK_CER" ]] && mcopy -o -i "$fat" "$MOK_CER" ::/falcon-boot.cer
    "$MKSTICK" --quiet bios --reserve --no-backup --stage1 "$STAGE1" \
        --stage2 "$STAGE2" --stage3 "$STAGE3" "$img"
done
