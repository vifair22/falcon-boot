#!/usr/bin/env bash
# falcon-boot: write the current Secure Boot chain into p1 of harness sticks.
#
# Every stick tools/coverage/mk-case-stick.sh builds carries the chain the
# product ships: Fedora's shim as \EFI\BOOT\BOOTX64.EFI, its MokManager, and
# our loader signed with the test MOK under shim's next-stage name. Staging it
# separately from building the stick is what lets a loader change be measured
# against the whole corpus without copying a single ISO again: this rewrites
# three files in a 64 MiB FAT, through mtools, with no sudo.
#
# The same chain boots in every Secure Boot state the harness runs: shim
# with Secure Boot off loads the next stage without verifying it, which is the
# SB-off experience of a real stick too.
#
# Usage: tools/coverage/stage-loader.sh STICK.img...

set -euo pipefail

cd "$(dirname "$0")/../.."
. tools/coverage/states.sh

# The dual-signed shim (tools/coverage/states.sh), so one stick boots in all
# three states.
states_prepare
SHIM_EFI=$SHIM_DUAL
MM_EFI=build/shim/bins/fedora-16.1-mmx64.efi
EFI_PLAIN=build/bin/falcon-boot.efi
EFI_SIGNED=build/bin/falcon-boot-signed.efi
MOK_CER=build/mok-test/falcon-test-mok.cer

for f in "$MM_EFI" "$EFI_SIGNED"; do
    [[ -f "$f" ]] || { echo "ERROR: $f missing (make sign; fetch-shims.sh)" >&2; exit 1; }
done
# A signed loader older than the unsigned one is a previous build, and
# measuring it would report on code that is no longer in the tree.
if [[ "$EFI_PLAIN" -nt "$EFI_SIGNED" ]]; then
    echo "ERROR: $EFI_SIGNED is older than $EFI_PLAIN (run: make sign)" >&2
    exit 1
fi

for img in "$@"; do
    fat="$img@@1M"
    mmd -D s -i "$fat" ::/EFI 2>/dev/null || true
    mmd -D s -i "$fat" ::/EFI/BOOT 2>/dev/null || true
    mcopy -o -i "$fat" "$SHIM_EFI" ::/EFI/BOOT/BOOTX64.EFI
    mcopy -o -i "$fat" "$MM_EFI" ::/EFI/BOOT/mmx64.efi
    mcopy -o -i "$fat" "$EFI_SIGNED" ::/EFI/BOOT/grubx64.efi
    [[ -f "$MOK_CER" ]] && mcopy -o -i "$fat" "$MOK_CER" ::/falcon-boot.cer
done
