#!/usr/bin/env bash
# Boot the shim-plumbing probe under OVMF, headless, with a hard timeout.
#
# Stages an ESP per shim variant in build/shim/esp-<variant>/: the shim as the
# removable-media default BOOTX64.EFI, the probe under whatever second-stage
# name that shim has compiled in, and the vendor's MokManager beside it. Then
# runs QEMU with no display and the serial console into a log, so the whole
# thing is scriptable.
#
# The "none" variant stages the probe itself as BOOTX64.EFI. That run is the
# baseline the shim runs are diffed against: it records the firmware's own
# gBS->LoadImage pointer, and a different pointer under shim is the in-place
# boot-services patch.
#
# Usage: run-probe.sh VARIANT [plain|secboot]
#   VARIANT: none | fedora-15.8 | fedora-16.1 | suse-16.1 | debian-16.1
#
# Each run gets a fresh copy of the OVMF varstore, so no state leaks between
# runs and a rejected image is a property of the shipped vars file, not of
# something an earlier run enrolled.
#
# Two env knobs let the same script run the MOK case (see mok-setup.sh):
#   PROBE_BIN      probe image to stage (default build/shim/probe.efi)
#   VARS_TEMPLATE  varstore to seed from, e.g. one with MokList pre-populated
#   TAG            suffix for the log/vars filenames so runs do not collide

set -euo pipefail

cd "$(dirname "$0")/../.."

OVMF_DIR="${OVMF_DIR:-/usr/share/edk2/OvmfX64}"
OUT=build/shim
BINS=$OUT/bins
PROBE="${PROBE_BIN:-$OUT/probe.efi}"
TIMEOUT="${TIMEOUT:-40}"
TAG="${TAG:-}"

variant="${1:?usage: run-probe.sh VARIANT [plain|secboot]}"
fw="${2:-plain}"

case "$fw" in
    plain)   CODE=$OVMF_DIR/OVMF_CODE.fd;         VARS_SRC="${VARS_TEMPLATE:-$OVMF_DIR/OVMF_VARS.fd}" ;;
    secboot) CODE=$OVMF_DIR/OVMF_CODE.secboot.fd; VARS_SRC="${VARS_TEMPLATE:-$OVMF_DIR/OVMF_VARS.secboot.fd}" ;;
    *) echo "ERROR: firmware must be plain or secboot" >&2; exit 2 ;;
esac

[[ -f $PROBE ]] || { echo "ERROR: $PROBE not built (make -C tools/shim-spike)" >&2; exit 1; }
[[ -f $CODE ]]  || { echo "ERROR: OVMF not found at $CODE" >&2; exit 1; }

ESP=$OUT/esp-$variant$TAG
rm -rf "$ESP"
mkdir -p "$ESP/EFI/BOOT"

# Second-stage name and MokManager name are compiled into each shim; these
# are the values read out of the binaries (see docs/notes/shim-spike.md).
case "$variant" in
    none)
        cp "$PROBE" "$ESP/EFI/BOOT/BOOTX64.EFI"
        ;;
    fedora-15.8|fedora-16.1|debian-16.1)
        cp "$BINS/$variant-shimx64.efi" "$ESP/EFI/BOOT/BOOTX64.EFI"
        cp "$PROBE" "$ESP/EFI/BOOT/grubx64.efi"
        [[ -f $BINS/$variant-mmx64.efi ]] && cp "$BINS/$variant-mmx64.efi" "$ESP/EFI/BOOT/mmx64.efi"
        ;;
    suse-16.1)
        cp "$BINS/suse-16.1-shim.efi" "$ESP/EFI/BOOT/BOOTX64.EFI"
        cp "$PROBE" "$ESP/EFI/BOOT/grub.efi"
        cp "$BINS/suse-16.1-MokManager.efi" "$ESP/EFI/BOOT/MokManager.efi"
        ;;
    *)
        echo "ERROR: unknown variant $variant" >&2; exit 2 ;;
esac

# Payloads for the probe's LoadImage test, when mok-setup.sh has built them.
for pay in pay-mok-sbat pay-mok-nosbat pay-unsigned; do
    [[ -f $OUT/$pay.efi ]] && cp "$OUT/$pay.efi" "$ESP/EFI/BOOT/$pay.efi"
done

VARS=$OUT/vars-$variant-$fw$TAG.fd
cp -f "$VARS_SRC" "$VARS"
chmod u+w "$VARS"

LOG=$OUT/log-$variant-$fw$TAG.txt
DBG=$OUT/ovmf-debug-$variant-$fw$TAG.log

ACCEL=()
[[ -w /dev/kvm ]] && ACCEL=(-enable-kvm -cpu host)

echo "== $variant on $fw OVMF${TAG:+ ($TAG)} -> $LOG"
set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
    -machine q35 -m 512 "${ACCEL[@]}" \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive format=raw,file=fat:rw:"$ESP" \
    -net none -display none \
    -serial "file:$LOG" \
    -debugcon "file:$DBG" -global isa-debugcon.iobase=0x402 \
    >/dev/null 2>&1
rc=$?
set -e
echo "   qemu exit $rc (124 = timed out, expected: the probe waits on a key)"
