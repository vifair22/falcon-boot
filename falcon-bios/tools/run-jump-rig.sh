#!/usr/bin/env bash
# falcon jump's rig (docs/DESIGN.md Section 14.9): every route booted
# headless under SeaBIOS and judged from the serial log.
#
# One case per route, each a JUMP.INI naming it with a one-second countdown,
# so nothing waits for a key:
#
#   hdd       an IDE disk carrying falcon-bios (build/test-bios.img, an MBR
#             disk with its boot partition at LBA 63): its stage 3 loads off
#             drive 0x80
#   cd        the rig's memprobe disc in an IDE CD drive: it boots and
#             reports the memory map it was handed
#   usb-uhci  the rig stick (build/test-bios-gpt.img, a GPT falcon stick)
#   usb-ohci  on each controller: jump knows it by its boot record, and the
#   usb-ehci  stick's own stage 3 loads off the drive the reverse thunk
#             serves and finds its library by record
#   cf        a CompactFlash card in the PC Card bridge falcon-pcc.c models,
#             carrying falcon-bios: its stage 3 loads off the card
#   cf3v      the same with a card that takes only 3.3 V, as the A21m's
#             SanDisk reports itself: the bridge powers it only when both
#             of its voltage registers ask for 3.3 V
#   cf48      a card past 128 GiB (mk-rig-disk.sh big, sparse) whose boot
#             partition starts past sector 2^28: its stage 2 reads stage 3
#             from there through PCC.MOD, which takes 48-bit commands for
#             it (the model's lba48=on), and stage 3's CRC-32 holds
#   mem       memtest86+, when MEMTEST names a build with the boot protocol
#             header: the handoff to it
#   giveup    the cf route to a card whose boot sector only calls INT 18h,
#             a medium with nothing to boot: jump's resident resets the
#             machine, which comes back to the floppy, rather than the BIOS
#             starting its next device on top of the resident (DESIGN 14.6)
#   s2crc     one byte of stage 2 flipped on the floppy: stage 2's own
#             CRC-32 stops it before it runs past its first sector
#   s3crc     one byte of the compressed stage 3 flipped: stage 2 reads it
#             twice and stops; on the screen, stage 2's loading line with its
#             spinner, and the fatal under it
#   stick3crc one byte of the rig stick's stage 3 flipped, the usb route
#             chosen: the stick's stage 2 reads it twice and stops
#   modcrc    one byte of UHCI.MOD flipped, the usb route chosen: the module
#             is read twice and refused
#   chs       no floppy of jump's: the stick's stages on an MBR-partitioned
#             2.88 MB floppy image, which the BIOS serves without INT 13h
#             extensions, so stage 1 and stage 2 read it by CHS
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

def() {       # def NAME: its value in biosdefs.h
    sed -n "s/^#define[ \t]*$1[ \t]*\([0-9]*\).*/\1/p" \
        falcon-bios/src/start/biosdefs.h | head -1
}
STAGE2_LBA=$(def STAGE2_LBA)
FD_STAGE3_SECTOR=$(def FD_STAGE3_SECTOR)
BOOTPART_STAGE3_SECTOR=$(def BOOTPART_STAGE3_SECTOR)

# The rig stick, as build.mk makes it: stages and records already on it.
cp build/test-bios-gpt.img "$R/stick.img"
# Where its stage 3 is: the ESP's start plus the record's stage 3 sector.
STICK_S3=$(( $(sgdisk -i 1 "$R/stick.img" |
    sed -n 's/^First sector: \([0-9]*\).*/\1/p') + BOOTPART_STAGE3_SECTOR ))

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

# boot, stopped once serial carries UNTIL: the VGA text memory saved first
# (to $R/screen.txt, one row a line), for a case judged by what is on the
# screen when a fatal is waiting for its key.
boot_until() { # boot_until QEMU-BINARY UNTIL ARGS...
    local q=$1 until=$2 qpid i
    shift 2
    cp build/test-bios.img "$R/disk.img"
    rm -f "$R/serial.log" "$R/mon.sock" "$R/screen.vga" "$R/screen.txt"
    "$q" -L /usr/share/qemu -machine pc -cpu pentium3 -m 128 -display none \
        -boot a -drive if=floppy,format=raw,file="$R/jump.img" \
        -drive file="$R/disk.img",format=raw,if=ide,index=0 \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait "$@" >/dev/null 2>&1 &
    qpid=$!
    for ((i = 0; i < TIMEOUT * 10; i++)); do
        [[ -f "$R/serial.log" ]] && log_text | grep -aqF -- "$until" && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 0.5
    monitor "pmemsave 0xb8000 4000 $R/screen.vga"
    sleep 0.5
    monitor quit
    wait "$qpid" 2>/dev/null || true
    log_text > "$R/serial.txt"
    [[ -f "$R/screen.vga" ]] &&
        python3 -c 'import sys; d = open(sys.argv[1], "rb").read()[0::2]; print("\n".join(d[i:i + 80].decode("ascii", "replace").rstrip() for i in range(0, 2000, 80)))' \
            "$R/screen.vga" > "$R/screen.txt"
    return 0
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

# PASS when the log carries `want`, the boot the case is for, and every
# `also` besides.
judge() {     # judge CASE WANT [ALSO...]
    local c=$1 want=$2 also ok=1
    shift 2
    grep -aq -- "$want" "$R/serial.txt" || ok=0
    for also in "$@"; do
        grep -aq -- "$also" "$R/serial.txt" || ok=0
    done
    set -- "$c" "$want"
    if [[ $ok -eq 1 ]]; then
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
    judge "$c" 'boot drive 0x8[1-9a-f], sector size 512' \
        'boot partition p1 at LBA' 'p2  LBA [0-9.]*  \[library\]' \
        'p2 mounted as exfat'
}

FAILED=0
CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(hdd cd usb-uhci usb-ohci usb-ehci cf cf3v cf48 giveup mem s2crc s3crc
           stick3crc modcrc chs menu)

for c in "${CASES[@]}"; do
    case $c in
    hdd)
        floppy hdd
        boot "$QEMU"
        judge hdd 'boot drive 0x80, sector size 512' \
            'boot partition p1 at LBA 63'
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
    cf48)
        if [[ ! -x "$QPCC" ]]; then
            printf 'SKIP  %-9s no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)\n' "$c"
            continue
        fi
        falcon-bios/tools/mk-rig-disk.sh big "$R/cf48.img" >/dev/null
        floppy cf
        boot "$QPCC" -drive if=none,id=cf,format=raw,file="$R/cf48.img" \
            -device falcon-pcc,drive=cf,lba48=on
        rm -f "$R/cf48.img"
        judge cf48 'boot partition p1 at LBA 268437504' \
            'stage 3 inflated' 'boot drive 0x81, sector size 512'
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
        flip $((STAGE2_LBA * 512 + 0x1000))
        boot "$QEMU"
        judge s2crc 'stage 2 did not read back as it was written'
        ;;
    s3crc)
        # Stage 3 never draws here, so the screen is still stage 2's: its
        # loading line, the spinner at a step, and the fatal under it.
        floppy hdd
        flip $((FD_STAGE3_SECTOR * 512 + 0x2000))
        boot_until "$QEMU" 'stage 3 did not read back as it was written, twice'
        if ! grep -aqF 'stage 3 did not read back as it was written, twice' "$R/serial.txt"; then
            judge s3crc 'stage 3 did not read back as it was written, twice'
        elif ! grep -qE '^falcon-boot: loading [-/|\\]$' "$R/screen.txt"; then
            printf 'FAIL  %-9s %s\n' s3crc "no 'falcon-boot: loading' line with its spinner on the screen"
            FAILED=1
        elif ! awk '/^falcon-boot: loading/ { s = 1; next }
                    s && /stage 3 did not read back as it was written, twice/ { f = 1 }
                    END { exit !f }' "$R/screen.txt"; then
            printf 'FAIL  %-9s %s\n' s3crc "the fatal is not on the screen under the loading line"
            FAILED=1
        else
            printf 'PASS  %-9s %s\n' s3crc "the loading line and its spinner on screen, the fatal under it"
        fi
        ;;
    stick3crc)
        floppy usb
        cp "$R/stick.img" "$R/stick-ok.img"
        b=$(od -An -tu1 -j $((STICK_S3 * 512 + 0x4000)) -N1 "$R/stick.img" |
            tr -d ' ')
        printf "\\$(printf %03o $((b ^ 0xFF)))" |
            dd of="$R/stick.img" bs=1 seek=$((STICK_S3 * 512 + 0x4000)) \
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
    chs)
        falcon-bios/tools/mk-rig-disk.sh chs "$R/chs.img" >/dev/null
        rm -f "$R/serial.log"
        timeout "$TIMEOUT" "$QEMU" -L /usr/share/qemu -machine pc \
            -cpu pentium3 -m 128 -display none -boot a \
            -drive if=floppy,format=raw,file="$R/chs.img" \
            -serial file:"$R/serial.log" -monitor none >/dev/null 2>&1 || true
        log_text > "$R/serial.txt"
        judge chs 'boot partition p1 at LBA 63' \
            'no INT 13h extensions, reading by CHS' 'C is running'
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
