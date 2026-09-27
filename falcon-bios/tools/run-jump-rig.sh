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
#   cf3v      the same with a card that takes only 3.3 V, as the A21m's
#             SanDisk reports itself: the bridge powers it only when both
#             of its voltage registers ask for 3.3 V
#   mem       memtest86+, when MEMTEST names a build with the boot protocol
#             header: the handoff to it
#   giveup    the cf route to a card whose boot sector only calls INT 18h,
#             a medium with nothing to boot: jump's resident resets the
#             machine, which comes back to the floppy, rather than the BIOS
#             starting its next device on top of the resident (DESIGN 14.6)
#   s2crc     one byte of stage 2 flipped on the floppy: stage 2's own
#             CRC-32 stops it before it runs past its first sector
#   s3crc     one byte of the compressed stage 3 flipped: stage 2 reads it
#             twice and stops
#   stick3crc one byte of the rig stick's stage 3 flipped, the usb route
#             chosen: the stick's stage 2 reads it twice and stops
#   modcrc    one byte of UHCI.MOD flipped, the usb route chosen: the module
#             is read twice and refused
#   menu      the countdown interrupted: a five-second default=hdd, a Down
#             key two seconds in, then nothing may boot for six seconds;
#             Enter then boots the row Down moved to (the CD drive, empty in
#             QEMU's default machine), not the default. The keys go in
#             through QEMU's monitor.
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

floppy() {    # floppy DEFAULT [SECONDS]
    printf 'default=%s\ntimeout=%s\n' "$1" "${2:-1}" > "$R/JUMP.INI"
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
    log_text > "$R/serial.txt"
}

# Flips one byte of the floppy image, at a byte offset.
flip() {      # flip OFFSET
    local b
    b=$(od -An -tu1 -j "$1" -N1 "$R/jump.img" | tr -d ' ')
    printf "\\$(printf %03o $((b ^ 0xFF)))" |
        dd of="$R/jump.img" bs=1 seek="$1" conv=notrunc status=none
}

# The byte offset of a file's contents in the floppy image: where its bytes
# as built (the .gz the image recipe copies) sit.
file_off() {  # file_off BUILT-FILE
    python3 - "$R/jump.img" "$1" <<'PY'
import sys
img, f = (open(p, 'rb').read() for p in sys.argv[1:3])
print(img.index(f[:64]))
PY
}

log_text() {
    tr -d '\r' < "$R/serial.log" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}

monitor() {   # monitor COMMAND
    echo "$1" | socat - UNIX-CONNECT:"$R/mon.sock" >/dev/null 2>&1 || true
}

# The countdown, interrupted. The judgement is in three parts, each its
# own reason for a FAIL: the countdown ran, a key stopped it (no boot in the
# six seconds after, where an unstopped five-second count would have booted
# the disk), and Enter took the row the key moved to.
menu_case() {
    local i

    floppy hdd 5
    cp build/test-bios.img "$R/disk.img"
    rm -f "$R/mon.sock" "$R/serial.log"
    timeout "$TIMEOUT" "$QEMU" -L /usr/share/qemu -machine pc -cpu pentium3 \
        -m 128 -display none -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img" \
        -drive file="$R/disk.img",format=raw,if=ide,index=0 \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait >/dev/null 2>&1 &
    for i in $(seq 1 300); do
        log_text 2>/dev/null | grep -aq 'Booting in' && break
        sleep 0.1
    done
    sleep 2
    monitor "sendkey down"
    sleep 6
    log_text > "$R/serial.txt"
    local counted=0 booted=0
    grep -aq 'Booting in 5 s' "$R/serial.txt" && counted=1
    grep -aq 'booting: ' "$R/serial.txt" && booted=1
    monitor "sendkey ret"
    sleep 4
    monitor quit
    wait 2>/dev/null || true
    log_text > "$R/serial.txt"
    cp "$R/serial.txt" "$R/menu.log"

    if [[ $counted -eq 0 ]]; then
        echo "FAIL  menu      the countdown never started"
        FAILED=1
    elif [[ $booted -eq 1 ]]; then
        echo "FAIL  menu      a key did not stop the countdown"
        FAILED=1
    elif ! grep -aq 'booting: CD' "$R/serial.txt" ||
         grep -aq 'booting: Disk' "$R/serial.txt"; then
        echo "FAIL  menu      Enter did not boot the row the key moved to"
        FAILED=1
    else
        echo "PASS  menu      stopped by a key, Enter booted the next row"
    fi
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
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(hdd cd usb-uhci usb-ohci usb-ehci cf cf3v giveup mem s2crc s3crc
           stick3crc modcrc menu)

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
    cf|cf3v)
        if [[ ! -x "$QPCC" ]]; then
            printf 'SKIP  %-9s no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)\n' "$c"
            continue
        fi
        cp build/test-bios.img "$R/cf.img"
        floppy cf
        boot "$QPCC" -drive if=none,id=cf,format=raw,file="$R/cf.img" \
            -device falcon-pcc,drive=cf$([[ $c == cf3v ]] && echo ,volts=3)
        judge "$c" 'boot drive 0x81, sector size 512'
        ;;
    giveup)
        if [[ ! -x "$QPCC" ]]; then
            echo "SKIP  giveup    no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)"
            continue
        fi
        # INT 18h, then a halt that is never reached.
        head -c 4194304 /dev/zero > "$R/cf.img"
        printf '\xcd\x18\xfa\xf4' |
            dd of="$R/cf.img" conv=notrunc status=none
        printf '\x55\xaa' |
            dd of="$R/cf.img" bs=1 seek=510 conv=notrunc status=none
        floppy cf
        boot "$QPCC" -drive if=none,id=cf,format=raw,file="$R/cf.img" \
            -device falcon-pcc,drive=cf
        if grep -aq 'returned to the BIOS; resetting' "$R/serial.txt" &&
           [[ $(grep -ac 'stage 1, floppy' "$R/serial.txt") -ge 2 ]]; then
            echo "PASS  giveup    INT 18h reset the machine, back to the floppy"
        else
            echo "FAIL  giveup    INT 18h did not come back through a reset"
            FAILED=1
        fi
        cp "$R/serial.txt" "$R/giveup.log"
        ;;
    s2crc)
        floppy hdd
        flip $((34 * 512 + 0x1000))
        boot "$QEMU"
        judge s2crc 'stage 2 did not read back as it was written'
        ;;
    s3crc)
        floppy hdd
        flip $((98 * 512 + 0x2000))
        boot "$QEMU"
        judge s3crc 'stage 3 did not read back as it was written, twice'
        ;;
    stick3crc)
        floppy usb
        cp "$R/stick.img" "$R/stick-ok.img"
        b=$(od -An -tu1 -j $((98 * 512 + 0x4000)) -N1 "$R/stick.img" | tr -d ' ')
        printf "\\$(printf %03o $((b ^ 0xFF)))" |
            dd of="$R/stick.img" bs=1 seek=$((98 * 512 + 0x4000)) \
                conv=notrunc status=none
        boot "$QEMU" -device piix3-usb-uhci,id=hc \
            -drive if=none,id=stick,format=raw,file="$R/stick.img" \
            -device usb-storage,bus=hc.0,drive=stick
        mv "$R/stick-ok.img" "$R/stick.img"
        judge stick3crc 'stage 3 did not read back as it was written, twice'
        ;;
    modcrc)
        floppy usb
        flip $(( $(file_off build/bin/mod/UHCI.MOD.gz) + 0x400 ))
        boot "$QEMU" -device piix3-usb-uhci,id=hc
        judge modcrc 'did not read back as it was written, twice'
        ;;
    menu) menu_case ;;
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
