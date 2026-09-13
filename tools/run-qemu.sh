#!/usr/bin/env bash
# airies-boot QEMU + OVMF test rig (docs/DESIGN.md Section 10).
#
# Stages build/bin/airies-boot.efi as the removable-media default loader on a
# vvfat-backed ESP and boots it under OVMF with a visible display window.
# Serial output mirrors the firmware console for logging; OVMF's debug port
# is captured to build/ovmf-debug.log.
#
# Usage: tools/run-qemu.sh [--secboot] [--headless] [extra qemu args...]
#   --secboot   use the Secure Boot OVMF build (enforcement testing, later
#               steps; needs certs enrolled in the vars file to be meaningful)
#   --headless  no window, serial to build/serial.log, QEMU monitor on the
#               unix socket build/mon.sock. This is the scriptable shape:
#               drive the menu with "sendkey" through the monitor and assert
#               on the log. Runs in the background is the caller's business;
#               this still execs qemu.

set -euo pipefail

cd "$(dirname "$0")/.."

OVMF_DIR="${OVMF_DIR:-/usr/share/edk2/OvmfX64}"
# Enough for the loader and the fixture; booting a real distro ISO wants more,
# because its bootloader pulls a kernel and a large initramfs into memory
# before ExitBootServices.
QEMU_MEM="${QEMU_MEM:-512}"
BUILD=build
ESP="$BUILD/esp"
EFI_BIN="$BUILD/bin/airies-boot.efi"

CODE="$OVMF_DIR/OVMF_CODE.fd"
VARS_SRC="$OVMF_DIR/OVMF_VARS.fd"
VARS="$BUILD/OVMF_VARS.fd"

HEADLESS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
    --secboot)
        CODE="$OVMF_DIR/OVMF_CODE.secboot.fd"
        VARS_SRC="$OVMF_DIR/OVMF_VARS.secboot.fd"
        VARS="$BUILD/OVMF_VARS.secboot.fd"
        shift
        ;;
    --headless)
        HEADLESS=1
        shift
        ;;
    *)
        break
        ;;
    esac
done

[[ -f "$EFI_BIN" ]] || { echo "ERROR: $EFI_BIN not built (run make first)" >&2; exit 1; }
[[ -f "$CODE" ]] || { echo "ERROR: OVMF not found at $CODE (set OVMF_DIR)" >&2; exit 1; }

mkdir -p "$ESP/EFI/BOOT"
cp "$EFI_BIN" "$ESP/EFI/BOOT/BOOTX64.EFI"

# Writable per-checkout copy of the varstore so MOK/SB enrollment state
# persists across runs but never touches the system-wide file. Delete it to
# factory-reset the virtual NVRAM.
[[ -f "$VARS" ]] || cp "$VARS_SRC" "$VARS"

# The exFAT test image (tools/mk-test-exfat.sh) stands in for the stick's ISO
# partition. Optional: without it the loader boots and finds no volume, which
# is itself a case worth being able to run.
TEST_DRIVE=()
if [[ -f "$BUILD/test-exfat.img" ]]; then
    TEST_DRIVE=(-drive "format=raw,file=$BUILD/test-exfat.img")
fi

ACCEL=()
if [[ -w /dev/kvm ]]; then
    ACCEL=(-enable-kvm -cpu host)
else
    echo "note: /dev/kvm not writable, running under TCG (slow)" >&2
fi

CONSOLE=(-serial stdio)
if [[ "$HEADLESS" -eq 1 ]]; then
    rm -f "$BUILD/serial.log" "$BUILD/mon.sock"
    CONSOLE=(
        -display none
        -serial "file:$BUILD/serial.log"
        -monitor "unix:$BUILD/mon.sock,server,nowait"
    )
fi

exec qemu-system-x86_64 \
    -name airies-boot \
    -machine q35 \
    -m "$QEMU_MEM" \
    "${ACCEL[@]}" \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive format=raw,file=fat:rw:"$ESP" \
    "${TEST_DRIVE[@]}" \
    -net none \
    "${CONSOLE[@]}" \
    -debugcon file:"$BUILD/ovmf-debug.log" -global isa-debugcon.iobase=0x402 \
    "$@"
