#!/usr/bin/env bash
# airies-boot QEMU + OVMF test rig (docs/DESIGN.md Section 10).
#
# Stages build/bin/airies-boot.efi as the removable-media default loader on a
# vvfat-backed ESP and boots it under OVMF with a visible display window.
# Serial output mirrors the firmware console for logging; OVMF's debug port
# is captured to build/ovmf-debug.log.
#
# Usage: tools/run-qemu.sh [--secboot] [--headless] [extra qemu args...]
#   --secboot   Secure Boot OVMF, with the real chain staged: shim as the
#               removable-media default loader, our SIGNED loader under the
#               next-stage name shim has compiled in, and a varstore that
#               trusts the test MOK. Needs `make sign` first (which needs
#               tools/mk-test-mok.sh). See the SECBOOT block below.
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
SECBOOT=0

# Secure Boot chain inputs. Fedora's shim-x64 16.1 is the one the spike
# recommends shipping (dual-signed by the MS 2011 and 2023 UEFI CAs, smallest
# vendor trust surface, image-loader protocol present); its compiled-in
# next-stage and MokManager names are grubx64.efi and mmx64.efi, which is why
# our loader is staged under a GRUB name here.
SHIM_BINS="$BUILD/shim/bins"
SHIM_EFI="$SHIM_BINS/fedora-16.1-shimx64.efi"
MM_EFI="$SHIM_BINS/fedora-16.1-mmx64.efi"
EFI_SIGNED="$BUILD/bin/airies-boot-signed.efi"
MOK_VARS="$BUILD/mok-test/OVMF_VARS.secboot.mok.fd"

while [[ $# -gt 0 ]]; do
    case "$1" in
    --secboot)
        SECBOOT=1
        CODE="$OVMF_DIR/OVMF_CODE.secboot.fd"
        # Not the stock varstore: the one mk-test-mok.sh seeded with our test
        # MOK. The stock file is already enrolled with the MS and Red Hat keys
        # (so shim itself passes db unmodified); only MOK has to be added, and
        # only for artifacts we signed ourselves.
        VARS_SRC="$MOK_VARS"
        VARS="$BUILD/OVMF_VARS.secboot.fd"
        ESP="$BUILD/esp-secboot"
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

[[ -f "$CODE" ]] || { echo "ERROR: OVMF not found at $CODE (set OVMF_DIR)" >&2; exit 1; }

if [[ "$SECBOOT" -eq 1 ]]; then
    [[ -f "$EFI_SIGNED" ]] || {
        echo "ERROR: $EFI_SIGNED not built (run: make sign)" >&2; exit 1; }
    [[ -f "$MOK_VARS" ]] || {
        echo "ERROR: $MOK_VARS missing (run: tools/mk-test-mok.sh)" >&2; exit 1; }
    if [[ ! -f "$SHIM_EFI" || ! -f "$MM_EFI" ]]; then
        echo "== fetching the pinned shim packages"
        tools/shim-spike/fetch-shims.sh
    fi

    # Rebuilt from scratch: a stale BOOTX64.EFI here would silently boot the
    # unsigned loader directly and make the whole run meaningless.
    rm -rf "$ESP"
    mkdir -p "$ESP/EFI/BOOT"
    cp "$SHIM_EFI" "$ESP/EFI/BOOT/BOOTX64.EFI"
    cp "$MM_EFI" "$ESP/EFI/BOOT/mmx64.efi"
    cp "$EFI_SIGNED" "$ESP/EFI/BOOT/grubx64.efi"
else
    [[ -f "$EFI_BIN" ]] || { echo "ERROR: $EFI_BIN not built (run make first)" >&2; exit 1; }
    mkdir -p "$ESP/EFI/BOOT"
    cp "$EFI_BIN" "$ESP/EFI/BOOT/BOOTX64.EFI"
fi

# Writable per-checkout copy of the varstore so MOK/SB enrollment state
# persists across runs but never touches the system-wide file. Delete it to
# factory-reset the virtual NVRAM.
[[ -f "$VARS" ]] || cp "$VARS_SRC" "$VARS"
chmod u+w "$VARS"

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
