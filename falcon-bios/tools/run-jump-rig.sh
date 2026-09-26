#!/usr/bin/env bash
# falcon jump's rig (docs/DESIGN.md Section 14.9): every route booted
# headless under SeaBIOS and judged from the serial log.
#
# One case per route, each a JUMP.INI naming it with a one-second countdown,
# so nothing waits for a key:
#
#   hdd       an IDE disk carrying falcon-bios (build/test-bios.img): its
#             stage 3 loads off drive 0x80
#   cd        the rig's memprobe disc in an IDE CD drive: it boots and
#             reports the memory map it was handed
#   usb-uhci  the rig stick (a GPT falcon stick with the stick's stages)
#   usb-ohci  on each controller: the stick's own stage 3 loads off the
#   usb-ehci  drive the reverse thunk serves
#   cf        a CompactFlash card in the PC Card bridge falcon-pcc.c models,
#             carrying falcon-bios: its stage 3 loads off the card
#   mem       memtest86+, when MEMTEST names a build with the boot protocol
#             header: the handoff to it
#
# The cf case needs the QEMU build-qemu.sh makes; the rest the system's.
#
# Usage: falcon-bios/tools/run-jump-rig.sh [case...]   (default: all)
# Env:   MEMTEST  memtest86+ for the mem case (skipped without it)
#        QEMU     the system's qemu-system-i386 (default: on PATH)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/jump-rig
QEMU=${QEMU:-qemu-system-i386}
QPCC=build/qemu-pcc/build/qemu-system-i386
TIMEOUT=45

mkdir -p "$R"
make --no-print-directory bios test-bios-img test-bios-probe >/dev/null

# The rig stick: the GPT rig disk with the stick's three stages written
# where mkstick bios puts them.
cp build/test-bios-gpt.img "$R/stick.img"
dd if=build/bin/falcon-bios-stage1.bin of="$R/stick.img" bs=440 count=1 \
    conv=notrunc status=none
dd if=build/bin/falcon-bios-stage2.bin of="$R/stick.img" bs=512 seek=34 \
    conv=notrunc status=none
dd if=build/bin/falcon-bios-stage3.bin of="$R/stick.img" bs=512 seek=98 \
    conv=notrunc status=none

floppy() {    # floppy DEFAULT
    printf 'default=%s\ntimeout=1\n' "$1" > "$R/JUMP.INI"
    make --no-print-directory jump JUMP_INI="$R/JUMP.INI" \
        ${MEMTEST:+MEMTEST=$MEMTEST} >/dev/null
    cp build/bin/falcon-jump.img "$R/jump.img"
}

boot() {      # boot QEMU-BINARY ARGS...
    local q=$1
    shift
    cp build/test-bios.img "$R/disk.img"
    timeout "$TIMEOUT" "$q" -L /usr/share/qemu -machine pc -cpu pentium3 \
        -m 128 -display none -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img" \
        -drive file="$R/disk.img",format=raw,if=ide,index=0 \
        -serial file:"$R/serial.log" -monitor none "$@" \
        >/dev/null 2>&1 || true
    tr -d '\r' < "$R/serial.log" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g' \
        > "$R/serial.txt"
}

# PASS when the log carries `want`, the boot the case is for.
judge() {     # judge CASE WANT
    if grep -aq -- "$2" "$R/serial.txt"; then
        printf 'PASS  %-9s %s\n' "$1" "$(grep -ao -- "$2.\{0,40\}" \
            "$R/serial.txt" | tail -1)"
    else
        printf 'FAIL  %-9s %s\n' "$1" "$(grep -aoE \
            'failed while.{0,60}|there is no.{0,50}|none of.{0,50}' \
            "$R/serial.txt" | tail -1)"
        FAILED=1
    fi
    cp "$R/serial.txt" "$R/$1.log"
}

usb() {       # usb CASE HC-DEVICES...
    local c=$1
    shift
    floppy usb
    boot "$QEMU" "$@" -drive if=none,id=stick,format=raw,file="$R/stick.img" \
        -device usb-storage,bus=hc.0,drive=stick
    judge "$c" 'boot drive 0x8[1-9a-f], sector size 512'
}

FAILED=0
CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] || CASES=(hdd cd usb-uhci usb-ohci usb-ehci cf mem)

for c in "${CASES[@]}"; do
    case $c in
    hdd)
        floppy hdd
        boot "$QEMU"
        judge hdd 'boot drive 0x80, sector size 512'
        ;;
    cd)
        floppy cd
        boot "$QEMU" -drive file=build/memprobe.iso,media=cdrom,if=ide,index=2
        judge cd 'MEMPROBE DONE'
        ;;
    usb-uhci) usb usb-uhci -device piix3-usb-uhci,id=hc ;;
    usb-ohci) usb usb-ohci -device pci-ohci,id=hc ;;
    usb-ehci) usb usb-ehci -device usb-ehci,id=hc ;;
    cf)
        if [[ ! -x "$QPCC" ]]; then
            echo "SKIP  cf        no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)"
            continue
        fi
        cp build/test-bios.img "$R/cf.img"
        floppy cf
        boot "$QPCC" -drive if=none,id=cf,format=raw,file="$R/cf.img" \
            -device falcon-pcc,drive=cf
        judge cf 'boot drive 0x81, sector size 512'
        ;;
    mem)
        if [[ -z "${MEMTEST:-}" ]]; then
            echo "SKIP  mem       no MEMTEST"
            continue
        fi
        floppy memtest
        boot "$QEMU"
        judge mem 'entering the kernel'
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
done
exit "$FAILED"
