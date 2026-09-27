#!/usr/bin/env bash
# The BIOS station with real systems behind it (docs/DESIGN.md Section
# 13.11): the fixture disks of tools/fixtures/README.md, each with the
# station put on the machine by `falcon-boot-mkstick station install
# --bios`, booted under SeaBIOS on the machine the fixtures were verified
# on (i440fx, a Pentium II, 128 MB, Cirrus VGA, TCG), and judged by what
# the screen shows: FreeDOS's prompt from VGA text memory, the Windows 9x
# desktop's taskbar from a screendump. The station's own lines come from serial.
#
# Two shapes per system:
#
#   *-own     the system's disk is the station's own: the installer puts
#             the station partition in free space at the disk's end, stage
#             1 in LBA 0 and stage 2 in front of the system's partition at
#             63, and the INI's default (the system, which it found) boots
#             after the countdown with no key pressed
#   *-second  a disk of its own carrying only the station, the system's
#             disk behind it as the second disk: the installer is given it
#             with --also, writes the signature it has none of, and the
#             default boots its partition behind the drive swap, which
#             makes it 0x80
#
# Cases:
#
#   fd-own, fd-second   FreeDOS 1.4 (freedos.img), to C:\>
#   fd-mbr              the second shape with the default changed to the
#                       FreeDOS disk's own MBR code, to C:\>
#   w98-own, w98-second Windows 98 SE (win98se.img), to the desktop
#   w95-own, w95-second Windows 95 OSR2 (win95.img), to the desktop;
#                       skipped while there is no such image
#
# The fixtures are read-only here. Each is the backing file of a qcow2
# overlay, grown by the station partition's room and presented by qemu-nbd
# for the installer (needs sudo: the nbd module and the device); each boot
# runs on a throwaway overlay of that (-snapshot). The station's own disk
# in the second shape is an image file. Nothing touches a real disk.
#
# Usage: falcon-bios/tools/run-station-fixture-rig.sh [case...]  (default: all)
# Env:   FIXTURES  where freedos.img, win98se.img, win95.img are
#                  (default build/fixtures; tools/fixtures/README.md says
#                  how to copy them, sparse, from binhost-builder)
#        QEMU      qemu-system-i386 (default: on PATH)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/station-fixture-rig
FIX=${FIXTURES:-build/fixtures}
QEMU=${QEMU:-qemu-system-i386}
MKSTICK=$ROOT/build/bin/falcon-boot-mkstick
VMCTL=tools/fixtures/vmctl.py

# Room for the default 64 MiB station partition and its MiB alignment.
GROW_MIB=72
# The second shape's station disk: a FAT32 data partition, then room for
# the station partition; and its signature.
ST_MIB=104
ST_DATA_MIB=32
SIG_ST=5a7a5701

mkdir -p "$R"
make --no-print-directory station "$MKSTICK" >/dev/null

NBD=
QPID=
cleanup() {
    [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null || true
    [[ -n "$NBD" ]] && sudo qemu-nbd --disconnect "$NBD" >/dev/null 2>&1 || true
}
trap cleanup EXIT

FAILED=0
pass() { printf 'PASS  %-11s %s\n' "$1" "$2"; }
fail() { printf 'FAIL  %-11s %s\n' "$1" "$2"; FAILED=1; }
skip() { printf 'SKIP  %-11s %s\n' "$1" "$2"; }

# The fixture, checked against the sha256 tools/fixtures/README.md records
# for it; 1 when it is absent or does not match.
declare -A CHECKED=()
fixture() {   # fixture NAME: prints its path
    local img=$FIX/$1 want
    [[ -f "$img" ]] || return 1
    if [[ -z "${CHECKED[$1]:-}" ]]; then
        want=$(sed -n "s/^| \`$1\` | \`\([0-9a-f]\{64\}\)\`.*/\1/p" \
            tools/fixtures/README.md)
        if [[ -z "$want" ]]; then
            echo "note: tools/fixtures/README.md records no sha256 for $1;" \
                "it is used unchecked" >&2
        elif ! echo "$want  $img" | sha256sum -c --quiet >/dev/null 2>&1; then
            echo "ERROR: $img does not match the README's sha256" >&2
            return 1
        fi
        CHECKED[$1]=1
    fi
    echo "$img"
}

nbd_connect() {  # nbd_connect QCOW2: sets NBD
    local n
    sudo modprobe nbd max_part=8
    NBD=
    for n in $(seq 0 15); do
        if [[ ! -e /sys/block/nbd$n/pid ]]; then
            NBD=/dev/nbd$n
            break
        fi
    done
    [[ -n "$NBD" ]] || { echo "ERROR: no free nbd device" >&2; exit 2; }
    sudo qemu-nbd --connect="$NBD" -f qcow2 "$1"
    for n in $(seq 1 50); do
        [[ -e /sys/block/${NBD#/dev/}/pid ]] && break
        sleep 0.1
    done
    sudo udevadm settle 2>/dev/null || sleep 1
}

nbd_disconnect() {
    sudo qemu-nbd --disconnect "$NBD" >/dev/null
    NBD=
}

# A qcow2 overlay of the fixture, its virtual size grown by GROW_MIB.
overlay() {   # overlay SRC OUT
    rm -f "$2"
    qemu-img create -q -f qcow2 -F raw -b "$(realpath "$1")" "$2"
    qemu-img resize -q "$2" "+${GROW_MIB}M"
}

sig_of() {    # sig_of DEV-OR-IMG: the MBR's disk signature, 8 hex digits
    sudo od -An -tx4 -j440 -N4 "$1" | tr -d ' \n'
}

# The system's disk as the station's own: the installer on the overlay.
build_own() {  # build_own NAME SRC: sets SIG
    local q=$R/$1-own.qcow2
    overlay "$2" "$q"
    nbd_connect "$q"
    sudo "$MKSTICK" station install "$NBD" --bios --no-scan \
        --backup "$R/$1-own.bak" --force >"$R/$1-own.install" 2>&1 ||
        { cat "$R/$1-own.install" >&2; exit 1; }
    sudo "$MKSTICK" station status "$NBD" --no-scan >"$R/$1-own.status" 2>&1 ||
        { cat "$R/$1-own.status" >&2; exit 1; }
    SIG=$(sig_of "$NBD")
    nbd_disconnect
    sudo chown "$(id -u):$(id -g)" "$R/$1-own.bak"
}

# The station on a disk of its own, the system's disk given with --also.
build_second() {  # build_second NAME SRC: sets SIG
    local q=$R/$1-sys.qcow2 st=$R/$1-station.img
    rm -f "$st"
    truncate -s "${ST_MIB}M" "$st"
    # A data partition, as a machine's disk has something on it; the
    # installer takes a disk with no table for a blank one and refuses it.
    printf 'label: dos\nlabel-id: 0x%s\nstart=2048, size=%d, type=c\n' \
        "$SIG_ST" $(( ST_DATA_MIB * 2048 )) |
        sfdisk -q --no-reread --no-tell-kernel "$st" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n DATA "$st" $(( ST_DATA_MIB * 1024 )) \
        >/dev/null 2>&1
    overlay "$2" "$q"
    nbd_connect "$q"
    sudo "$MKSTICK" station install "$st" --bios --no-scan \
        --also "$NBD" --backup "$R/$1-second.bak" --force \
        >"$R/$1-second.install" 2>&1 ||
        { cat "$R/$1-second.install" >&2; exit 1; }
    sudo "$MKSTICK" station status "$st" --also "$NBD" \
        >"$R/$1-second.status" 2>&1 ||
        { cat "$R/$1-second.status" >&2; exit 1; }
    SIG=$(sig_of "$NBD")
    nbd_disconnect
    sudo chown "$(id -u):$(id -g)" "$R/$1-second.bak" "$st"
}

# The station partition's start in an image file (type 0x1C).
station_start() {  # station_start IMG
    sfdisk -d "$1" | sed -n 's/.*start= *\([0-9]*\),.*type=1c.*/\1/p'
}

log_text() {
    [[ -f "$R/serial.log" ]] || return 0
    tr -d '\r' < "$R/serial.log" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}

mon() {
    printf '%s\n' "$*" | socat - UNIX-CONNECT:"$R/mon.sock" >/dev/null 2>&1 || true
}

vga_text() {  # vga_text FILE: the 80x25 text screen dumped from 0xB8000
    python3 - "$1" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
for r in range(25):
    print(''.join(chr(c) if 32 <= c < 127 else ' '
                  for c in b[r * 160:(r + 1) * 160:2]).rstrip())
PY
}

# The machine tools/fixtures/fixture-shot.sh verified each fixture on. TCG,
# for the reason that script gives.
start() {     # start DISK0 [DISK1]
    local drives=() d i=0
    for d in "$@"; do
        case $d in
        *.qcow2) drives+=(-drive "file=$d,format=qcow2,if=ide,index=$i") ;;
        *)       drives+=(-drive "file=$d,format=raw,if=ide,index=$i") ;;
        esac
        i=$(( i + 1 ))
    done
    rm -f "$R/serial.log" "$R/mon.sock"
    "$QEMU" -L /usr/share/qemu -machine pc -accel tcg -cpu pentium2 -m 128 \
        -vga cirrus -nic none -rtc base=localtime -display none \
        "${drives[@]}" -snapshot -boot order=c \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait >"$R/qemu.log" 2>&1 &
    QPID=$!
    for i in $(seq 1 50); do
        [[ -S "$R/mon.sock" ]] && break
        sleep 0.1
    done
}

stop() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
    log_text > "$R/serial.txt"
}

# Waits for FreeDOS's prompt in VGA text memory, then keeps the text and a
# screendump. 0 when the prompt came.
wait_dos() {  # wait_dos CASE SECONDS
    local i
    for (( i = 0; i < $2; i++ )); do
        sleep 1
        kill -0 "$QPID" 2>/dev/null || break
        mon "pmemsave 0xb8000 4000 $R/$1.vga"
        sleep 0.2
        [[ -s "$R/$1.vga" ]] || continue
        vga_text "$R/$1.vga" > "$R/$1.txt"
        grep -aq 'C:\\>' "$R/$1.txt" && break
    done
    sleep 1
    mon "screendump $R/$1.png -f png"
    sleep 0.5
    grep -aq 'C:\\>' "$R/$1.txt" 2>/dev/null
}

# Whether the screen is the Windows 9x desktop: the taskbar's grey band
# along the bottom (vmctl.py's measure). Its other test, the wallpaper's
# teal, is not taken here: 98 paints the teal alone, with the hourglass on
# it, for a while before the desktop comes, and a machine stuck there has
# not booted.
taskbar_up() {  # taskbar_up CASE
    local out
    out=$(python3 "$VMCTL" desktop "$R/mon.sock" 2>&1) || true
    echo "$out" >> "$R/$1.desktop"
    awk '{ exit !($3 > 0.6) }' <<< "$out"
}

# Waits for the desktop, then keeps a screendump of it once the tray has
# had time to fill. 0 when the desktop came and stayed.
wait_desktop() {  # wait_desktop CASE SECONDS
    local t0=$SECONDS ok=1
    while (( SECONDS - t0 < $2 )); do
        sleep 10
        kill -0 "$QPID" 2>/dev/null || break
        if taskbar_up "$1"; then
            ok=0
            break
        fi
    done
    if [[ $ok -eq 0 ]]; then
        sleep 15
        taskbar_up "$1" || ok=1
    fi
    python3 "$VMCTL" shot "$R/mon.sock" "$R/$1.png" >/dev/null 2>&1 ||
        mon "screendump $R/$1.png -f png"
    echo "desktop after $(( SECONDS - t0 )) s" >> "$R/$1.desktop"
    return $ok
}

# The serial lines a case must carry, grep -E; the first missing one, or "".
missing() {   # missing PATTERN...
    local p
    for p in "$@"; do
        grep -aqE -- "$p" "$R/serial.txt" || { echo "$p"; return; }
    done
}

# One case: the disks built, the machine started, the screen waited for,
# the serial judged. KIND is dos or win; SHAPE own, second or mbr.
run_case() {  # run_case CASE FIXTURE LABEL KIND SHAPE
    local c=$1 src label=$3 kind=$4 shape=$5 miss st start_lba ok why=
    local secs=60
    [[ $kind == win ]] && secs=900
    if ! src=$(fixture "$2"); then
        skip "$c" "no $FIX/$2 (tools/fixtures/README.md)"
        return
    fi
    rm -f "$R/$c.png" "$R/$c.vga" "$R/$c.txt" "$R/$c.desktop"
    case $shape in
    own)
        build_own "$c" "$src"
        start "$R/$c-own.qcow2"
        ;;
    second|mbr)
        build_second "$c" "$src"
        st=$R/$c-station.img
        if [[ $shape == mbr ]]; then
            start_lba=$(station_start "$st")
            mtype -i "$st@@$(( start_lba * 512 ))" ::STATION.INI |
                sed "s/^default = .*/default = mbr-$SIG/" > "$R/STATION.INI"
            mcopy -o -i "$st@@$(( start_lba * 512 ))" "$R/STATION.INI" \
                ::STATION.INI
        fi
        start "$st" "$R/$c-sys.qcow2"
        ;;
    esac
    if [[ $kind == dos ]]; then
        wait_dos "$c" "$secs" && ok=1 || ok=0
    else
        wait_desktop "$c" "$secs" && ok=1 || ok=0
    fi
    stop
    if [[ $ok -eq 0 ]]; then
        if [[ $kind == dos ]]; then
            why="no C:\\> on screen ($R/$c.txt, $R/$c.png)"
        else
            why="no desktop in $secs s ($R/$c.png)"
        fi
    fi
    case $shape in
    own)
        miss=$(missing 'Booting in [0-9]' \
            "booting: $label \\(disk mbr:$SIG, partition 1\\)")
        ;;
    second)
        miss=$(missing 'Booting in [0-9]' \
            "booting: $label \\(disk mbr:$SIG, partition 1\\)" \
            'drive 0x81 swapped with 0x80')
        ;;
    mbr)
        miss=$(missing 'Booting in [0-9]' \
            "booting: Boot code \\(disk mbr:$SIG\\)" \
            'MBR of drive 0x81 to 0x7c00, DL 0x80' \
            'drive 0x81 swapped with 0x80')
        ;;
    esac
    [[ -z "$why" && -n "$miss" ]] && why="serial has no /$miss/"
    cp "$R/serial.txt" "$R/$c.log"
    if [[ -z "$why" ]]; then
        if [[ $kind == dos ]]; then
            pass "$c" "mbr:$SIG, C:\\> ($R/$c.png)"
        else
            pass "$c" "mbr:$SIG, the desktop ($R/$c.png)"
        fi
    else
        fail "$c" "$why"
    fi
}

CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(fd-own fd-second fd-mbr w98-own w98-second w95-own w95-second)

for c in "${CASES[@]}"; do
    case $c in
    fd-own)     run_case "$c" freedos.img FreeDOS dos own ;;
    fd-second)  run_case "$c" freedos.img FreeDOS dos second ;;
    fd-mbr)     run_case "$c" freedos.img FreeDOS dos mbr ;;
    w98-own)    run_case "$c" win98se.img 'Windows 9x' win own ;;
    w98-second) run_case "$c" win98se.img 'Windows 9x' win second ;;
    w95-own)    run_case "$c" win95.img 'Windows 9x' win own ;;
    w95-second) run_case "$c" win95.img 'Windows 9x' win second ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
done
exit "$FAILED"
