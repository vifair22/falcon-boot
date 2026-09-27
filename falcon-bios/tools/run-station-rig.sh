#!/usr/bin/env bash
# The BIOS station's rig (docs/DESIGN.md Section 13.11): the partition
# chain, the drive swap, STATION.INI and next boot, booted headless under
# SeaBIOS and judged from serial; FreeDOS, which has no serial, judged from
# the VGA text it leaves on screen, with a screendump kept beside it.
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
#   fd-own     FreeDOS 1.4 LiteUSB as the station's own disk (the station
#              put in its free space): its p1 found by KERNEL.SYS, booted
#   fd-second  the same image as a second disk behind the swap, p1 booted
#   fd-mbr     its own MBR code behind the swap
#
# Usage: falcon-bios/tools/run-station-rig.sh [case...]   (default: all)
# Env:   MEMTEST  memtest86+ for the mem case (skipped without it)
#        FREEDOS  FD14LITE.img (default build/freedos/FD14LITE.img, fetched
#                 from binhost's fixtures or ibiblio when absent, and
#                 checked against FreeDOS's published SHA-256)
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

FD_DIR=build/freedos
FD_ZIP_SHA256=857dcd2ebf9d3d094320154db5fb5b830acba6fb98f981a95a0ca7ab3350338b
FD_URL=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/FD14-LiteUSB.zip
FD_FIXTURE=vifair@172.20.0.170:/srv/falcon/fixtures/station/freedos.img

SIG_A=5a7a0a01
SIG_B=5a7a0b02
SIG_FD=5a7a0f03
ID=f41c0b1057a6e0000000000000000010

mkdir -p "$R"
make --no-print-directory station build/rigboot.bin "$MKSTICK" >/dev/null

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

# FreeDOS 1.4 LiteUSB: binhost's fixture when the fixtures stream has put
# one there, else FreeDOS's own zip, checked against its published hash.
freedos() {
    local img=${FREEDOS:-$FD_DIR/FD14LITE.img}
    if [[ -f "$img" ]]; then
        echo "$img"
        return
    fi
    mkdir -p "$FD_DIR"
    if scp -q -o BatchMode=yes -o ConnectTimeout=5 "$FD_FIXTURE" \
           "$FD_DIR/FD14LITE.img" 2>/dev/null; then
        echo "$FD_DIR/FD14LITE.img"
        return
    fi
    curl -sfL -o "$FD_DIR/FD14-LiteUSB.zip" "$FD_URL" || return 1
    echo "$FD_ZIP_SHA256  $FD_DIR/FD14-LiteUSB.zip" | sha256sum -c --quiet \
        >&2 || return 1
    (cd "$FD_DIR" && unzip -o -q FD14-LiteUSB.zip FD14LITE.img) || return 1
    echo "$FD_DIR/FD14LITE.img"
}

# The FreeDOS image with the station installed in its free space, as the
# installer does: the image grown by 40 MiB for the station partition, a
# disk signature, stage 1 over FreeDOS's MBR code and stage 2 at LBA 1, in
# front of its partition at 63.
build_fd_own() {  # build_fd_own SRC
    local start end
    cp "$1" "$R/fd-own.img"
    end=$(( $(stat -c %s "$R/fd-own.img") / 512 ))
    start=$(( (end + 2047) / 2048 * 2048 ))
    truncate -s $(( (start + 81920) * 512 )) "$R/fd-own.img"
    sfdisk -q --no-reread --no-tell-kernel --disk-id "$R/fd-own.img" \
        "0x$SIG_FD" >/dev/null 2>&1
    printf 'start=%d, size=%d, type=1c\n' "$start" 81920 |
        sfdisk -q --no-reread --no-tell-kernel --append "$R/fd-own.img" \
            >/dev/null
    station_part "$R/fd-own.img" "$start" 81920 2
    stages12 "$R/fd-own.img"
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
    local drives=(-drive file="$4",format=raw,if=ide,index=0)
    [[ -n "${5:-}" ]] && drives+=(-drive file="$5",format=raw,if=ide,index=1)
    rm -f "$R/serial.log" "$R/mon.sock" "$R/$name.vga" "$R/$name.png"
    "$QEMU" -L /usr/share/qemu -machine pc -cpu pentium3 -m 128 \
        -display none -boot c "${drives[@]}" -rtc base=utc \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait >/dev/null 2>&1 &
    qpid=$!
    for ((i = 0; i < secs * 10; i++)); do
        [[ -n "$until" ]] && log_text | grep -aqE -- "$until" && break
        kill -0 "$qpid" 2>/dev/null || break
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

# The screen's characters, a line of 80 per row.
vga_text() {  # vga_text NAME
    python3 - "$R/$1.vga" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
for r in range(25):
    print(''.join(chr(c) if 32 <= c < 127 else ' '
                  for c in b[r * 160:(r + 1) * 160:2]).rstrip())
PY
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

fd_judge() {  # fd_judge CASE
    vga_text "$1" > "$R/$1.txt"
    if grep -aq 'FreeDOS' "$R/$1.txt"; then
        pass "$1" "$(grep -a 'FreeDOS' "$R/$1.txt" | head -1 | cut -c1-80 |
            sed 's/  */ /g')"
    else
        fail "$1" "no FreeDOS on screen ($R/$1.txt, $R/$1.png)"
    fi
}

CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(own-p1 own-p2 logical b-p1-swap b-p1-noswap b-mbr ini-error
           next-boot next-miss mem fd-own fd-second fd-mbr)

build_disks
FD_SRC=

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
    fd-own|fd-second|fd-mbr)
        if [[ -z "$FD_SRC" ]]; then
            FD_SRC=$(freedos) || {
                echo "SKIP  $c no FreeDOS image"
                continue
            }
        fi
        if [[ $c == fd-own ]]; then
            build_fd_own "$FD_SRC"
            start=$(sfdisk -d "$R/fd-own.img" | sed -n 's/.*2 : start= *\([0-9]*\).*/\1/p')
            ini "$R/fd-own.img" "$start" $'[general]\ndefault = mbr-'"$SIG_FD"$'-p1\ntimeout = 1\n'
            boot fd-own '' 25 "$R/fd-own.img"
            grep -aq "mbr-$SIG_FD-p1: FreeDOS" "$R/serial.txt" ||
                fail fd-own "its p1 was not found as FreeDOS"
        else
            cp "$FD_SRC" "$R/fd-b.img"
            sfdisk -q --no-reread --no-tell-kernel --disk-id "$R/fd-b.img" \
                "0x$SIG_FD" >/dev/null 2>&1
            cp "$R/a.img" "$R/a-run.img"
            if [[ $c == fd-second ]]; then
                ini "$R/a-run.img" "$A_P4" $'[general]\ndefault = mbr-'"$SIG_FD"$'-p1\ntimeout = 1\n'
            else
                ini "$R/a-run.img" "$A_P4" $'[general]\ndefault = mbr-'"$SIG_FD"$'\ntimeout = 1\n'
            fi
            boot "$c" '' 25 "$R/a-run.img" "$R/fd-b.img"
            grep -aq 'drive 0x81 swapped with 0x80' "$R/serial.txt" ||
                fail "$c" "the swap was not installed"
        fi
        fd_judge "$c"
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
done
exit "$FAILED"
