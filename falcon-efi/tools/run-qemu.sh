#!/usr/bin/env bash
# falcon-boot QEMU + OVMF test rig (docs/DESIGN.md Section 10).
#
# Stages build/bin/falcon-boot.efi as the removable-media default loader on a
# vvfat-backed ESP and boots it under OVMF with a visible display window.
# Serial output mirrors the firmware console for logging; OVMF's debug port
# is captured to build/ovmf-debug.log.
#
# Usage: falcon-efi/tools/run-qemu.sh [--secboot] [--stick] [--headless]
#                                     [qemu args...]
#   --secboot   Secure Boot OVMF, with the real chain staged: shim as the
#               removable-media default loader, our SIGNED loader under the
#               next-stage name shim has compiled in, and a varstore that
#               trusts the test MOK. Needs `make sign` first (which needs
#               falcon-efi/tools/mk-test-mok.sh). See the SECBOOT block below.
#   --stick     boot build/test-stick.img and nothing else: no vvfat ESP, no
#               separate exFAT fixture, just the one raw disk with a real GPT on
#               it (falcon-efi/tools/mk-test-stick.sh). This is the shape the
#               personal OS path needs, because p3 only exists as a partition of
#               a partitioned disk. Combines with --secboot, which then supplies
#               the MokList varstore; the stick image itself is the same file
#               either way, since the shim chain on its p1 is staged at build
#               time rather than here.
#   --headless  no window, serial to build/serial.log, QEMU monitor on the
#               unix socket build/mon.sock. This is the scriptable shape:
#               drive the menu with "sendkey" through the monitor and assert
#               on the log. Runs in the background is the caller's business;
#               this still execs qemu.
#
# Env overrides, for booting something other than the rig's own fixtures
# (this is how the mkstick acceptance run boots a stick the TOOL built, with
# the MOK that RUN generated, rather than the rig's throwaway key):
#   STICK_IMG   the --stick disk image      (default build/test-stick.img)
#   MOK_VARS    the varstore template --secboot starts from
#               (default build/mok-test/OVMF_VARS.secboot.mok.fd).
#               mk-test-mok.sh writes a second one beside it,
#               OVMF_VARS.secboot.moksb.fd: the same enrolled MOK plus
#               MokSBState, which is how permissive mode (DESIGN Section
#               7.4) is booted. Point MOK_VARS at it, and VARS_FILE
#               somewhere new, to get a machine where shim validates
#               nothing.
#   VARS_FILE   the writable per-run varstore copy. Set it when MOK_VARS
#               changes: the copy is only made when the destination is
#               absent, so an existing build/OVMF_VARS.secboot.fd from an
#               earlier run would silently supply the OLD MokList.
#   SERIAL_LOG  --headless serial log        (default build/serial.log)
#   MON_SOCK    --headless monitor socket    (default build/mon.sock)

set -euo pipefail

cd "$(dirname "$0")/../.."

. "$(dirname "$0")/ovmf.sh"
# Enough for the loader and the fixture; booting a real distro ISO wants more,
# because its bootloader pulls a kernel and a large initramfs into memory
# before ExitBootServices.
QEMU_MEM="${QEMU_MEM:-512}"
BUILD=build
ESP="$BUILD/esp"
EFI_BIN="$BUILD/bin/falcon-boot.efi"

CODE=$(ovmf_find CODE plain || true)
VARS_SRC=$(ovmf_find VARS plain || true)
VARS="${VARS_FILE:-$BUILD/OVMF_VARS.fd}"

HEADLESS=0
SECBOOT=0
STICK=0
STICK_IMG="${STICK_IMG:-$BUILD/test-stick.img}"

# Secure Boot chain inputs. Fedora's shim-x64 16.1 is the one the spike
# recommends shipping (dual-signed by the MS 2011 and 2023 UEFI CAs, smallest
# vendor trust surface, image-loader protocol present); its compiled-in
# next-stage and MokManager names are grubx64.efi and mmx64.efi, which is why
# our loader is staged under a GRUB name here.
SHIM_BINS="$BUILD/shim/bins"
SHIM_EFI="$SHIM_BINS/fedora-16.1-shimx64.efi"
MM_EFI="$SHIM_BINS/fedora-16.1-mmx64.efi"
EFI_SIGNED="$BUILD/bin/falcon-boot-signed.efi"
MOK_VARS="${MOK_VARS:-$BUILD/mok-test/OVMF_VARS.secboot.mok.fd}"

while [[ $# -gt 0 ]]; do
    case "$1" in
    --secboot)
        SECBOOT=1
        CODE=$(ovmf_find CODE secboot || true)
        # Not the stock varstore: the one mk-test-mok.sh seeded with our test
        # MOK. The stock file is already enrolled with the MS and Red Hat keys
        # (so shim itself passes db unmodified); only MOK has to be added, and
        # only for artifacts we signed ourselves.
        VARS_SRC="$MOK_VARS"
        VARS="${VARS_FILE:-$BUILD/OVMF_VARS.secboot.fd}"
        ESP="$BUILD/esp-secboot"
        shift
        ;;
    --stick)
        STICK=1
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

# In stick mode the disk image already carries its own ESP and its own copy of
# the shim chain, so none of the vvfat staging below applies. The only thing
# --secboot still contributes is the varstore that trusts the test MOK.
if [[ "$STICK" -eq 1 ]]; then
    [[ -f "$STICK_IMG" ]] || {
        echo "ERROR: $STICK_IMG missing (run: make test-stick)" >&2; exit 1; }
    if [[ "$SECBOOT" -eq 1 ]]; then
        [[ -f "$MOK_VARS" ]] || {
            echo "ERROR: $MOK_VARS missing (run: falcon-efi/tools/mk-test-mok.sh)" >&2
            exit 1; }
    fi
elif [[ "$SECBOOT" -eq 1 ]]; then
    [[ -f "$EFI_SIGNED" ]] || {
        echo "ERROR: $EFI_SIGNED not built (run: make sign)" >&2; exit 1; }
    [[ -f "$MOK_VARS" ]] || {
        echo "ERROR: $MOK_VARS missing (run: falcon-efi/tools/mk-test-mok.sh)" >&2; exit 1; }
    if [[ ! -f "$SHIM_EFI" || ! -f "$MM_EFI" ]]; then
        echo "== fetching the pinned shim packages"
        falcon-efi/tools/shim-spike/fetch-shims.sh
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

# What the firmware is given to boot from.
#
# Stick mode is one raw disk and nothing else, on purpose: a vvfat ESP beside
# it would give the loader a second, unpartitioned filesystem to find and would
# make "which partition did p3 come from" ambiguous in exactly the test that
# exists to answer it.
#
# Otherwise it is the vvfat ESP plus, if built, the whole-device exFAT fixture
# (falcon-core/tools/mk-test-exfat.sh) standing in for the stick's ISO
# partition. That one is optional: without it the loader boots and finds no
# volume, which is itself a case worth being able to run.
DRIVES=()
if [[ "$STICK" -eq 1 ]]; then
    DRIVES=(-drive "format=raw,file=$STICK_IMG")
else
    DRIVES=(-drive "format=raw,file=fat:rw:$ESP")
    if [[ -f "$BUILD/test-exfat.img" ]]; then
        DRIVES+=(-drive "format=raw,file=$BUILD/test-exfat.img")
    fi
fi

ACCEL=()
if [[ -w /dev/kvm ]]; then
    ACCEL=(-enable-kvm -cpu host)
else
    echo "note: /dev/kvm not writable, running under TCG (slow)" >&2
fi

CONSOLE=(-serial stdio)
if [[ "$HEADLESS" -eq 1 ]]; then
    # Overridable so two scripted runs (say a rig test and a mkstick
    # acceptance run) can be in flight at once without overwriting each
    # other's log or fighting over one monitor socket.
    SERIAL_LOG="${SERIAL_LOG:-$BUILD/serial.log}"
    MON_SOCK="${MON_SOCK:-$BUILD/mon.sock}"
    rm -f "$SERIAL_LOG" "$MON_SOCK"
    CONSOLE=(
        -display none
        -serial "file:$SERIAL_LOG"
        -monitor "unix:$MON_SOCK,server,nowait"
    )
fi

exec qemu-system-x86_64 \
    -name falcon-boot \
    -machine q35 \
    -m "$QEMU_MEM" \
    "${ACCEL[@]}" \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    "${DRIVES[@]}" \
    -net none \
    "${CONSOLE[@]}" \
    -debugcon file:"$BUILD/ovmf-debug.log" -global isa-debugcon.iobase=0x402 \
    "$@"
