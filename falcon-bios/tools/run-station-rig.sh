#!/usr/bin/env bash
# The BIOS station's rig (docs/DESIGN.md Section 13.11): the partition
# chain, the drive swap, STATION.INI and next boot, booted headless under
# SeaBIOS and judged from serial. Real systems behind the station (FreeDOS,
# Windows 9x) are run-station-fixture-rig.sh's.
#
# Two rig disks, built from image files alone (sfdisk, mkfs.fat --offset,
# mtools, `mkstick ident`), no loop device and no root:
#
#   A (0x80)  mbr:5a7a0a01, the station's own disk. Stage 1 in LBA 0, stage
#             2 at LBA 1. p1 FAT32 type 0x0C whose boot sector is the rig's
#             (tools/rigboot.S) tagged NTLDR, its hidden-sectors field 0 on
#             disk; p2 FAT32 type 0x0B tagged WINBOOT SYS; p3 extended, p5
#             in it a FAT16 logical with the rig's sector; p4 the station
#             partition, hidden FAT32 (0x1C): stage 3, the marker sector,
#             the boot record, STATION.INI, MEMTEST.BIN with MEMTEST.
#   B (0x81)  mbr:5a7a0b02, a second disk: the rig's sector as its MBR code,
#             p1 FAT32 0x0C, active, tagged BOOTMGR.
#
# Cases, each a STATION.INI with a one- or two-second default so nothing
# waits for a key:
#
#   own-p1     A p1, autodetected: DL 0x80, DS:SI 0000:07BE, the entry
#              active with type 0x0C, the hidden sectors patched to 2048 in
#              memory, and the countdown drawn
#   own-p2     A p2 (type 0x0B): DS:SI 0000:07CE, the entry's type 0x0B
#   logical    A p5, added by a section: refused with the sentence
#   b-p1-swap  B p1, autodetected: DL 0x80 and 0x80 answering as B (the
#              swap), the entry B's
#   b-p1-noswap  the same with swap = no: DL 0x81, 0x80 still A, and the
#              BPB's drive number patched to 0x81
#   b-mbr      B's own MBR code, autodetected: DL 0x80, 0x80 answering as B
#   ini-error  a key STATION.INI does not know: the line and why on screen,
#              and no countdown
#   next-boot  a marker naming B's MBR, and a default naming A p1: B's MBR
#              boots with no menu drawn
#   next-miss  a marker naming nothing here: the sentence, and no boot
#   mem        MEMTEST.BIN in the station partition, the default: the
#              handoff to it (skipped without MEMTEST)
#   no-ticks   booted through a floppy (tools/tickstop.S) that masks IRQ0
#              first, so the BIOS's tick count stands still: the countdown
#              runs by the RTC and boots A p1 (console/cdclock.c)
#   idle       poweroff = 4 and no default: the menu left alone, the
#              machine powered off (APM, which SeaBIOS answers with ACPI
#              S5), QEMU exiting by itself (DESIGN Section 7.9)
#   idle-no-ticks  the same through tickstop: timed by the RTC alone
#
# Usage: falcon-bios/tools/run-station-rig.sh [case...]   (default: all)
# Env:   MEMTEST  memtest86+ for the mem case (skipped without it)
#        QEMU     qemu-system-i386 (default: on PATH)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/station-rig
QEMU=${QEMU:-qemu-system-i386}
TIMEOUT=40

S1=build/bin/falcon-bios-stage1.bin
S2=build/bin/falcon-bios-stage2.bin
S3=build/bin/falcon-bios-station-stage3.bin
RIG=build/rigboot.bin
MKSTICK=build/bin/falcon-boot-mkstick

SIG_A=5a7a0a01
SIG_B=5a7a0b02
ID=f41c0b1057a6e0000000000000000010

mkdir -p "$R"
make --no-print-directory station build/rigboot.bin build/tickstop.bin "$MKSTICK" >/dev/null

def() {       # def NAME: its value in biosdefs.h, hex or decimal
    local v
    v=$(sed -n "s/^#define[ \t]*$1[ \t]*\([0-9A-Fa-fx]*\).*/\1/p" \
        falcon-bios/src/start/biosdefs.h | head -1)
    [[ -n "$v" ]] || { echo "ERROR: $1 is not in biosdefs.h" >&2; exit 1; }
    echo $((v))
}
STAGE2_LBA_MBR=$(def STAGE2_LBA_MBR)
S2_LBA_OFF=$(def STAGE1_S2_LBA_OFF)
S2_SECTORS_OFF=$(def STAGE1_S2_SECTORS_OFF)
STAGE3=$(def BOOTPART_STAGE3_SECTOR)
MARKER=$(def BOOTPART_MARKER_SECTOR)
s2_sectors=$(( ($(stat -c %s "$S2") + 511) / 512 ))
reserved=$("$MKSTICK" ident --reserved-sectors --stage3 "$S3")

# Disk A's partitions, in sectors.
A_P1=2048;   A_P1_LEN=73728
A_P2=75776;  A_P2_LEN=73728
A_P3=149504; A_P3_LEN=16384
A_P5=151552; A_P5_LEN=14336
A_P4=165888; A_P4_LEN=81920
B_P1=2048;   B_P1_LEN=73728

le() {        # le VALUE BYTES: VALUE as BYTES little-endian bytes
    local v=$1 n=$2 i out=
    for ((i = 0; i < n; i++)); do
        out+=$(printf '\\%03o' $(( (v >> (8 * i)) & 255 )))
    done
    printf "$out"
}

put() {       # put IMG BYTE-OFFSET: stdin into the image there
    dd of="$1" bs=1 seek="$2" conv=notrunc status=none
}

# Stage 1's 440 bytes with stage 2's place written in, stage 2 at LBA 1.
stages12() {  # stages12 IMG
    dd if="$S1" of="$1" bs=440 count=1 conv=notrunc status=none
    le "$STAGE2_LBA_MBR" 4 | put "$1" "$S2_LBA_OFF"
    le "$s2_sectors" 2 | put "$1" "$S2_SECTORS_OFF"
    dd if="$S2" of="$1" bs=512 seek="$STAGE2_LBA_MBR" conv=notrunc status=none
}

# The station partition: FAT32 with stage 3 in its reserved sectors and the
# boot record, as `mkstick station install` makes one.
station_part() {  # station_part IMG START SECTORS NUMBER
    local log
    log=$(mkfs.fat -F 32 -S 512 -s 1 -R "$reserved" -h "$2" --offset "$2" \
        -n FALCON "$1" $(( $3 / 2 )) 2>&1) || { echo "$log" >&2; exit 1; }
    dd if="$S3" of="$1" bs=512 seek=$(( $2 + STAGE3 )) conv=notrunc \
        status=none
    "$MKSTICK" --quiet ident "$1" --write-boot "$4" --id "$ID" >/dev/null
}

# A FAT volume whose boot sector's code is the rig's: its own BPB (bytes
# 3-0x59) kept, the rest the rig's, and TAG at 0x1A4.
rig_part() {  # rig_part IMG START SECTORS BITS HIDDEN TAG
    local log off=$(( $2 * 512 ))
    log=$(mkfs.fat -F "$4" -S 512 -s 1 -h "$5" --offset "$2" "$1" \
        $(( $3 / 2 )) 2>&1) || { echo "$log" >&2; exit 1; }
    dd if="$RIG" bs=1 count=3 status=none | put "$1" "$off"
    dd if="$RIG" bs=1 skip=$((0x5A)) count=$((512 - 0x5A)) status=none |
        put "$1" $(( off + 0x5A ))
    printf '%s' "$6" | put "$1" $(( off + 0x1A4 ))
}

# The rig's sector as a disk's MBR code: bytes 0-439, the table kept.
rig_mbr() {   # rig_mbr IMG
    dd if="$RIG" of="$1" bs=440 count=1 conv=notrunc status=none
}

build_disks() {
    rm -f "$R/a.img" "$R/b.img"
    truncate -s 128M "$R/a.img"
    printf 'label: dos\nlabel-id: 0x%s\n%s\n%s\n%s\n%s\n%s\n' "$SIG_A" \
        "start=$A_P1, size=$A_P1_LEN, type=c" \
        "start=$A_P2, size=$A_P2_LEN, type=b" \
        "start=$A_P3, size=$A_P3_LEN, type=5" \
        "start=$A_P4, size=$A_P4_LEN, type=1c" \
        "start=$A_P5, size=$A_P5_LEN, type=6" |
        sfdisk -q --no-reread --no-tell-kernel "$R/a.img" >/dev/null
    # p1's BPB says 0 hidden sectors: a volume imaged from elsewhere.
    rig_part "$R/a.img" "$A_P1" "$A_P1_LEN" 32 0 NTLDR
    rig_part "$R/a.img" "$A_P2" "$A_P2_LEN" 32 "$A_P2" "WINBOOT SYS"
    rig_part "$R/a.img" "$A_P5" "$A_P5_LEN" 16 "$A_P5" ""
    station_part "$R/a.img" "$A_P4" "$A_P4_LEN" 4
    if [[ -n "${MEMTEST:-}" ]]; then
        mcopy -o -i "$R/a.img@@$((A_P4 * 512))" "$MEMTEST" ::MEMTEST.BIN
    fi
    stages12 "$R/a.img"

    truncate -s 48M "$R/b.img"
    printf 'label: dos\nlabel-id: 0x%s\nstart=%d, size=%d, type=c, bootable\n' \
        "$SIG_B" "$B_P1" "$B_P1_LEN" |
        sfdisk -q --no-reread --no-tell-kernel "$R/b.img" >/dev/null
    rig_part "$R/b.img" "$B_P1" "$B_P1_LEN" 32 "$B_P1" BOOTMGR
    rig_mbr "$R/b.img"
}

ini() {       # ini IMG START TEXT: STATION.INI into a station partition
    printf '%s' "$3" > "$R/STATION.INI"
    mcopy -o -i "$1@@$(( $2 * 512 ))" "$R/STATION.INI" ::STATION.INI
}

# A next-boot marker in the station partition's marker sector, dated now by
# the clock QEMU's RTC follows (UTC).
marker() {    # marker IMG START VALUE
    local sec=$R/marker.bin
    printf 'falcon-next-boot 1\n%s\n%s\n' "$(date -u +%s)" "$3" > "$sec"
    truncate -s 512 "$sec"
    dd if="$sec" of="$1" bs=512 seek=$(( $2 + MARKER )) conv=notrunc \
        status=none
}

log_text() {
    [[ -f "$R/serial.log" ]] || return 0
    tr -d '\r' < "$R/serial.log" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}

mon() {
    printf '%s\n' "$*" | socat - UNIX-CONNECT:"$R/mon.sock" >/dev/null 2>&1 || true
}

# Boots disk 0 (and disk 1) until the log carries UNTIL, or for SECONDS
# when UNTIL is empty, then saves the VGA text and a screendump and stops.
boot() {      # boot NAME UNTIL SECONDS DISK0 [DISK1]
    local name=$1 until=$2 secs=$3 i qpid
    local drives=(-drive file="$4",format=raw,if=ide,index=0) order=c
    [[ -n "${5:-}" ]] && drives+=(-drive file="$5",format=raw,if=ide,index=1)
    if [[ -n "${FLOPPY:-}" ]]; then
        drives+=(-drive if=floppy,format=raw,file="$FLOPPY")
        order=a
    fi
    rm -f "$R/serial.log" "$R/mon.sock" "$R/$name.vga" "$R/$name.png"
    "$QEMU" -L /usr/share/qemu -machine pc -cpu pentium3 -m 128 \
        -display none -boot "$order" "${drives[@]}" -rtc base=utc \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait >/dev/null 2>&1 &
    qpid=$!
    EXITED=0
    for ((i = 0; i < secs * 10; i++)); do
        [[ -n "$until" ]] && log_text | grep -aqE -- "$until" && break
        kill -0 "$qpid" 2>/dev/null || { EXITED=1; break; }
        sleep 0.1
    done
    sleep 1
    mon "pmemsave 0xb8000 4000 $R/$name.vga"
    mon "screendump $R/$name.png -f png"
    sleep 0.5
    mon quit
    wait "$qpid" 2>/dev/null || true
    log_text > "$R/serial.txt"
    cp "$R/serial.txt" "$R/$name.log"
}

FAILED=0

pass() { printf 'PASS  %-12s %s\n' "$1" "$2"; }
fail() { printf 'FAIL  %-12s %s\n' "$1" "$2"; FAILED=1; }

# PASS when the log has every pattern (grep -E), naming the RIGBOOT line.
judge() {     # judge CASE PATTERN...
    local c=$1 p ok=1 missing=
    shift
    for p in "$@"; do
        grep -aqE -- "$p" "$R/serial.txt" || { ok=0; missing=$p; break; }
    done
    if [[ $ok -eq 1 ]]; then
        pass "$c" "$(grep -ao 'RIGBOOT.*\|failed while.*\|entering the kernel.*' \
            "$R/serial.txt" | tail -1 | cut -c1-110)"
    else
        fail "$c" "no /$missing/: $(grep -aoE 'RIGBOOT.*|failed while.{0,60}' \
            "$R/serial.txt" | tail -1 | cut -c1-90)"
    fi
}

case_ini() {  # case_ini NAME INI-TEXT [MARKER-VALUE]
    cp "$R/a.img" "$R/a-run.img"
    cp "$R/b.img" "$R/b-run.img"
    ini "$R/a-run.img" "$A_P4" "$2"
    [[ -n "${3:-}" ]] && marker "$R/a-run.img" "$A_P4" "$3"
    return 0
}

run_rig_case() {  # run_rig_case NAME INI [MARKER]
    case_ini "$@"
    boot "$1" 'RIGBOOT.*sigdl=[0-9a-f-]{8}|failed while|entering the kernel' \
        "$TIMEOUT" "$R/a-run.img" "$R/b-run.img"
}

CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(own-p1 own-p2 logical b-p1-swap b-p1-noswap b-mbr ini-error
           next-boot next-miss mem no-ticks idle idle-no-ticks)

build_disks

for c in "${CASES[@]}"; do
    case $c in
    own-p1)
        run_rig_case own-p1 $'[general]\ndefault = mbr-'"$SIG_A"$'-p1\ntimeout = 2\n'
        judge own-p1 'Booting in 2' \
            "hidden sectors were 0, the partition starts at $A_P1; patched" \
            "RIGBOOT dl=80 dssi=0000:07be entry=80[0-9a-f]{6}0c[0-9a-f]{6}00080000" \
            "hidden=00000800 type=0c sig80=$SIG_A sigdl=$SIG_A"
        ;;
    own-p2)
        run_rig_case own-p2 $'[general]\ndefault = mbr-'"$SIG_A"$'-p2\ntimeout = 1\n'
        judge own-p2 \
            "RIGBOOT dl=80 dssi=0000:07ce entry=80[0-9a-f]{6}0b" \
            "hidden=$(printf %08x $A_P2) type=0b sig80=$SIG_A"
        ;;
    logical)
        run_rig_case logical $'[general]\ndefault = logical\ntimeout = 1\n[logical]\ndisk = mbr:'"$SIG_A"$'\npart = 5\n'
        judge logical 'booting: Partition \(disk mbr:5a7a0a01, partition 5\)' \
            'failed while checking the partition' 'It is a logical partition'
        ;;
    b-p1-swap)
        run_rig_case b-p1-swap $'[general]\ndefault = mbr-'"$SIG_B"$'-p1\ntimeout = 1\n'
        judge b-p1-swap 'drive 0x81 swapped with 0x80' \
            "RIGBOOT dl=80 dssi=0000:07be entry=80[0-9a-f]{6}0c[0-9a-f]{6}00080000" \
            "hidden=00000800 type=0c sig80=$SIG_B sigdl=$SIG_B"
        ;;
    b-p1-noswap)
        run_rig_case b-p1-noswap $'[general]\ndefault = b1\ntimeout = 1\n[b1]\ndisk = mbr:'"$SIG_B"$'\npart = 1\nswap = no\n'
        judge b-p1-noswap "drive number was 0x80, DL is 0x81; patched" \
            "RIGBOOT dl=81 dssi=0000:07be entry=80" \
            "sig80=$SIG_A sigdl=$SIG_B"
        if grep -aq 'swapped with 0x80' "$R/serial.txt"; then
            fail b-p1-noswap "the swap was installed with swap = no"
        fi
        ;;
    b-mbr)
        run_rig_case b-mbr $'[general]\ndefault = mbr-'"$SIG_B"$'\ntimeout = 1\n'
        judge b-mbr 'MBR of drive 0x81 to 0x7c00, DL 0x80' \
            "RIGBOOT dl=80 dssi=0000:0000 .* sig80=$SIG_B sigdl=$SIG_B"
        ;;
    ini-error)
        case_ini ini-error $'[general]\ndefault = mbr-'"$SIG_A"$'-p1\ntimeout = 1\nbogus = 1\n'
        boot ini-error '' 12 "$R/a-run.img" "$R/b-run.img"
        if grep -aq 'STATION.INI line 4' "$R/serial.txt" &&
           grep -aq 'The menu will wait with no default' "$R/serial.txt" &&
           ! grep -aq 'RIGBOOT\|Booting in' "$R/serial.txt"; then
            pass ini-error "$(grep -ao 'STATION.INI line 4.*' "$R/serial.txt" |
                head -1 | cut -c1-80)"
        else
            fail ini-error "no error screen, or a boot followed it"
        fi
        ;;
    next-boot)
        run_rig_case next-boot $'[general]\ndefault = mbr-'"$SIG_A"$'-p1\ntimeout = 5\n' \
            "mbr-$SIG_B"
        judge next-boot "next boot: mbr-$SIG_B" \
            "RIGBOOT dl=80 dssi=0000:0000 .* sig80=$SIG_B"
        if grep -aq 'Booting in' "$R/serial.txt"; then
            fail next-boot "the menu's countdown ran before the marker's entry"
        fi
        ;;
    next-miss)
        case_ini next-miss $'[general]\ndefault = mbr-'"$SIG_A"$'-p1\ntimeout = 1\n' \
            no-such-entry
        boot next-miss '' 12 "$R/a-run.img" "$R/b-run.img"
        if grep -aq 'the next-boot marker names no-such-entry, and no entry has that name' \
               "$R/serial.txt" && ! grep -aq 'RIGBOOT' "$R/serial.txt"; then
            pass next-miss "the marker's name refused, nothing booted"
        else
            fail next-miss "no sentence, or a boot followed it"
        fi
        ;;
    mem)
        if [[ -z "${MEMTEST:-}" ]]; then
            echo "SKIP  mem          no MEMTEST"
            continue
        fi
        run_rig_case mem $'[general]\ndefault = memtest\ntimeout = 1\n'
        judge mem 'booting: Memory test' 'entering the kernel'
        ;;
    no-ticks)
        head -c 1474560 /dev/zero > "$R/tickstop.img"
        dd if=build/tickstop.bin of="$R/tickstop.img" conv=notrunc status=none
        FLOPPY=$R/tickstop.img run_rig_case no-ticks \
            $'[general]\ndefault = mbr-'"$SIG_A"$'-p1\ntimeout = 3\n'
        judge no-ticks 'TICKSTOP: IRQ0 masked' 'Booting in 3' 'Booting in 1' \
            "RIGBOOT dl=80 dssi=0000:07be"
        ;;
    idle|idle-no-ticks)
        FLOPPY=
        if [[ $c == idle-no-ticks ]]; then
            head -c 1474560 /dev/zero > "$R/tickstop.img"
            dd if=build/tickstop.bin of="$R/tickstop.img" conv=notrunc status=none
            FLOPPY=$R/tickstop.img
        fi
        case_ini "$c" $'[general]\npoweroff = 4\n'
        FLOPPY=$FLOPPY boot "$c" '' 60 "$R/a-run.img" "$R/b-run.img"
        if [[ $EXITED -ne 1 ]]; then
            fail "$c" "the machine was still on after 60 s"
        elif ! grep -aq 'no key for the idle time, powering off' "$R/serial.txt"; then
            fail "$c" "QEMU exited, but not through the idle power-off"
        elif grep -aq 'RIGBOOT' "$R/serial.txt"; then
            fail "$c" "something booted"
        else
            pass "$c" "the menu left alone; powered off, QEMU exited"
        fi
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
done
exit "$FAILED"
