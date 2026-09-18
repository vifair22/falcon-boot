#!/usr/bin/env bash
# Boot the EDK2 UEFI Shell under OVMF with a scripted startup.nsh, headless,
# and capture the shell's output on the serial log. Used to compare what the
# firmware surfaces for a real QEMU CD-ROM against what it surfaces for the
# virtual CD falcon-boot publishes.
#
# Usage: falcon-efi/tools/win-spike/run-shell.sh <tag> <nsh-file> [extra qemu
# args...]
#   <tag>       names the per-run output files under build/win-spike/
#   <nsh-file>  the startup.nsh to run (copied into the shell's ESP)
#
# Output: build/win-spike/<tag>.serial.log, build/win-spike/<tag>.debugcon.log

set -euo pipefail

cd "$(dirname "$0")/../../.."

TAG=${1:?usage: run-shell.sh <tag> <nsh> [qemu args...]}
NSH=${2:?usage: run-shell.sh <tag> <nsh> [qemu args...]}
shift 2

D=build/win-spike
OVMF_DIR="${OVMF_DIR:-/usr/share/edk2/OvmfX64}"
ESP=$D/$TAG-esp
VARS=$D/$TAG-VARS.fd

rm -rf "$ESP"
mkdir -p "$ESP/EFI/BOOT"
cp "$OVMF_DIR/Shell.efi" "$ESP/EFI/BOOT/BOOTX64.EFI"
cp "$NSH" "$ESP/startup.nsh"
for extra in "$D/$TAG-extra"/*; do
    [[ -e $extra ]] && cp -r "$extra" "$ESP/"
done
cp "$OVMF_DIR/OVMF_VARS.fd" "$VARS"
chmod u+w "$VARS"

ACCEL=()
[[ -w /dev/kvm ]] && ACCEL=(-enable-kvm -cpu host)

exec qemu-system-x86_64 \
    -name falcon-win-shell \
    -machine q35 \
    -m 4096 -smp 2 \
    "${ACCEL[@]}" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_DIR/OVMF_CODE.fd" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive format=raw,file=fat:rw:"$ESP",id=esp \
    -net none \
    -display none \
    -serial "file:$D/$TAG.serial.log" \
    -monitor "unix:$D/$TAG.mon.sock,server,nowait" \
    -debugcon "file:$D/$TAG.debugcon.log" -global isa-debugcon.iobase=0x402 \
    "$@"
