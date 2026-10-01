#!/usr/bin/env bash
# station-legacy's rig (docs/DESIGN.md Sections 12.10 and 12.11): the
# A21m's disk with station-legacy installed on it by the installer
# (`mkstick station install --bios --legacy`), booted under SeaBIOS as the
# A21m would be, and every row of its menu taken in turn.
#
# The disk is mk-a21m-station.sh's, in build/a21m-legacy: a qcow2 overlay
# of the original image (never copied, never opened for writing) with the
# NTFS partition shrunk, the station partition in the space that leaves,
# jump's drivers, MEMTEST.BIN and DevLink recovery's UKI in its /uki. Each
# boot runs on a throwaway overlay on top of that, as run-a21m-rig.sh's do.
# The machine is QEMU's pc with the A21m's PIIX4 south bridge, a Pentium
# III with 512 MB, the disk with the A21m's geometry.
#
# Each case waits for the menu, reads the order its rows were drawn in off
# serial, stops the countdown, moves to its row and presses Enter:
#
#   menu      every row is there with every device attached: Windows,
#             CD drive, CF card (PC Card), USB stick, DevLink recovery,
#             memtest
#   cd        a CD in an IDE ATAPI drive (the rig's memprobe disc, on the
#             secondary channel): it boots and reports the memory map
#   usb-uhci  the jump rig's falcon stick on each USB controller: the
#   usb-ohci  stick's own stages load off the drive the reverse thunk
#   usb-ehci  serves and mount its library
#   cf        the rig's PC Card bridge (falcon-pcc.c) with a CF card
#   cf3v      carrying falcon-bios, the second a 3.3 V-only card: its
#             stage 3 loads off the card as drive 0x81
#   devlink   DevLink recovery off the station partition's /uki, on an
#             i586 (-cpu pentium) with 32 MB: the devlink client
#             reaches its agent on COM1
#   windows   Windows 2000 from its own disk through the partition chain:
#             screens are saved for NTLDR, the splash and the desktop
#
# Judged from serial where falcon or the medium prints, from the devlink
# client, and from screendumps (build/legacy-rig/*.png) where Windows is
# silent.
#
# Usage: falcon-bios/tools/run-legacy-rig.sh [case...]   (default: all)
# Env:   DEVLINK_CLIENT  the devlink client (default:
#                        ~/git/home/devlink/build/bin/devlink)
#        A21M_FRESH=1    install again from the original image
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/legacy-rig
D=build/a21m-legacy
IMG=$D/a21m.qcow2
QEMU=${QEMU:-qemu-system-i386}
QPCC=build/qemu-pcc/build/qemu-system-i386
CLIENT=${DEVLINK_CLIENT:-$HOME/git/home/devlink/build/bin/devlink}
MEMTEST=${MEMTEST:-build/memtest/mt86p_810_i586}
MACHINE=pc,x-south-bridge=piix4-isa
DISK=cyls=20784,heads=15,secs=63,bios-chs-trans=large
TIMEOUT=90

command -v socat >/dev/null || { echo "socat is needed for the monitor" >&2; exit 2; }
mkdir -p "$R"
make --no-print-directory station-legacy devlink-uki test-bios-img \
    test-bios-probe >/dev/null

# The installed disk, made once (or again with A21M_FRESH=1).
if [[ ! -f "$IMG" || "${A21M_FRESH:-0}" == 1 ]]; then
    echo "== installing station-legacy on the A21m's disk ($IMG)"
    extra=()
    [[ -f "$MEMTEST" ]] && extra=(MEMTEST="$MEMTEST")
    env A21M_DIR="$D" "${extra[@]}" falcon-bios/tools/mk-a21m-station.sh \
        --legacy --uki build/devlink/devlink-recovery.efi >"$R/install.log" 2>&1 ||
        { tail -20 "$R/install.log"; exit 1; }
fi

QPID=
PTY=
cleanup() { [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null || true; }
trap cleanup EXIT

log_text() {    # log_text LOG: serial with CRs and ANSI sequences gone
    [[ -f "$1" ]] || return 0
    tr -d '\r' < "$1" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}
mon() { printf '%s\n' "$*" | socat - UNIX-CONNECT:"$R/mon.sock" >/dev/null 2>&1 || true; }
has() { log_text "$R/com1.log" | grep -aE -- "$1" >/dev/null; }
wait_for() {    # wait_for PATTERN SECONDS
    local i
    for (( i = 0; i < $2 * 10; i++ )); do
        has "$1" && return 0
        kill -0 "$QPID" 2>/dev/null || { has "$1"; return; }
        sleep 0.1
    done
    return 1
}
shot() {        # shot NAME
    mon "screendump $R/$1.ppm"
    sleep 0.5
    [[ -f "$R/$1.ppm" ]] && magick "$R/$1.ppm" "$R/$1.png" 2>/dev/null &&
        rm -f "$R/$1.ppm"
    return 0
}

# boot QEMU-BINARY [-pty] ARGS...: the installed disk on a throwaway
# overlay, COM1 to a file (or a pty for the devlink client).
boot() {
    local q=$1 com1=(-serial "file:$R/com1.log") cpu=(-cpu pentium3 -m 512)
    local pty=0
    shift
    if [[ "${1:-}" == -pty ]]; then
        pty=1
        shift
        com1=(-chardev "pty,id=com1,logfile=$R/com1.log" -serial chardev:com1)
        cpu=(-cpu pentium -m 32)
    fi
    rm -f "$R/com1.log" "$R/mon.sock" "$R/overlay.qcow2" "$R/qemu.log"
    qemu-img create -q -f qcow2 -F qcow2 -b "$(realpath "$IMG")" \
        "$R/overlay.qcow2"
    timeout "$TIMEOUT" "$q" -L /usr/share/qemu -machine "$MACHINE" \
        "${cpu[@]}" -display none -net none \
        -drive if=none,id=d0,file="$R/overlay.qcow2",format=qcow2 \
        -device ide-hd,drive=d0,bus=ide.0,unit=0,$DISK \
        "${com1[@]}" -monitor unix:"$R/mon.sock",server,nowait \
        "$@" >"$R/qemu.log" 2>&1 &
    QPID=$!
    PTY=
    local i
    for (( i = 0; pty == 1 && i < 50; i++ )); do
        PTY=$(sed -n 's/.*redirected to \(\/dev\/pts\/[0-9]*\).*/\1/p' \
            "$R/qemu.log" 2>/dev/null)
        [[ -n "$PTY" ]] && break
        sleep 0.1
    done
}

halt() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
}

# The rows in the order the first menu drew them, one label per line.
ROWS='Windows \(NTLDR\)|CD drive|CF card \(PC Card\)|USB stick|DevLink recovery|Memory test'
rows() {
    log_text "$R/com1.log" | sed -n '/entries, [0-9]* shown/,$p' |
        grep -aoE "$ROWS" | awk '!seen[$0]++'
}

# Stops the countdown on the menu, moves to the row LABEL and presses
# Enter. Nonzero, with WHY set, when the menu or the row is not there.
choose() {      # choose LABEL
    local n
    WHY=
    wait_for 'Booting in 5 s' 60 || { WHY="the menu's countdown never drew"; return 1; }
    sleep 0.5
    mon "sendkey down"        # any key stops the countdown; Down moves too
    sleep 0.5
    n=$(rows | grep -nxF -- "$1" | cut -d: -f1)
    [[ -n "$n" ]] || { WHY="no row '$1' (rows: $(rows | paste -sd,))"; return 1; }
    # The countdown's first key already moved one row down.
    local i
    for (( i = 0; i < 8; i++ )); do mon "sendkey up"; sleep 0.1; done
    for (( i = 1; i < n; i++ )); do mon "sendkey down"; sleep 0.2; done
    mon "sendkey ret"
    return 0
}

FAILED=0
result() {      # result CASE OK DETAIL
    if [[ $2 -eq 1 ]]; then
        printf 'PASS  %-9s %s\n' "$1" "$3"
    else
        printf 'FAIL  %-9s %s\n' "$1" "$3"
        FAILED=1
    fi
    log_text "$R/com1.log" > "$R/$1.log"
}

# A case booting a row and judged from serial: every pattern must appear.
run_serial() {  # run_serial CASE LABEL SECONDS QEMU ARGS... -- PATTERN...
    local c=$1 label=$2 secs=$3 q=$4 args=() pats=() p
    shift 4
    while [[ $# -gt 0 && "$1" != -- ]]; do args+=("$1"); shift; done
    shift
    pats=("$@")
    boot "$q" "${args[@]}"
    if choose "$label"; then
        WHY=
        for p in "${pats[@]}"; do
            wait_for "$p" "$secs" || { WHY="no /$p/"; break; }
        done
    fi
    shot "$c"
    halt
    if [[ -z "$WHY" ]]; then
        result "$c" 1 "$(log_text "$R/com1.log" | grep -aoE -- "${pats[-1]}.{0,50}" | tail -1)"
    else
        result "$c" 0 "$WHY"
    fi
}

stick() {       # stick CASE HC-DEVICE
    cp build/test-bios-gpt.img "$R/stick.img"
    run_serial "$1" "USB stick" 40 "$QEMU" -device "$2",id=hc \
        -drive if=none,id=stick,format=raw,file="$R/stick.img" \
        -device usb-storage,bus=hc.0,drive=stick -- \
        'booting: USB stick' 'module (EHCI|UHCI|OHCI)' \
        'boot drive 0x8[1-9a-f], sector size 512' 'p2 mounted as exfat'
    rm -f "$R/stick.img"
}

CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(menu cd usb-uhci usb-ohci usb-ehci cf cf3v devlink windows)

for c in "${CASES[@]}"; do
    case $c in
    menu)
        if [[ ! -x "$QPCC" ]]; then
            echo "SKIP  menu      no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)"
            continue
        fi
        cp build/test-bios.img "$R/cf.img"
        boot "$QPCC" -device piix3-usb-uhci,id=hc \
            -drive if=none,id=cf,format=raw,file="$R/cf.img" \
            -device falcon-pcc,drive=cf
        WHY=
        wait_for 'Booting in 5 s' 60 || WHY="the menu's countdown never drew"
        mon "sendkey down"
        sleep 1
        shot menu
        halt
        want=$'Windows (NTLDR)\nCD drive\nCF card (PC Card)\nUSB stick\nDevLink recovery\nMemory test'
        got=$(rows)
        [[ -z "$WHY" && "$got" != "$want" ]] && WHY="rows: $(paste -sd, <<<"$got")"
        result menu "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
            "${WHY:-$(paste -sd, <<<"$got")}"
        rm -f "$R/cf.img"
        ;;
    cd)
        run_serial cd "CD drive" 40 "$QEMU" \
            -drive file=build/memprobe.iso,media=cdrom,if=none,id=cd \
            -device ide-cd,drive=cd,bus=ide.1,unit=0 -- \
            'booting: CD drive' 'module ATA' 'MEMPROBE DONE'
        ;;
    usb-uhci) stick usb-uhci piix3-usb-uhci ;;
    usb-ohci) stick usb-ohci pci-ohci ;;
    usb-ehci) stick usb-ehci usb-ehci ;;
    cf|cf3v)
        if [[ ! -x "$QPCC" ]]; then
            printf 'SKIP  %-9s no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)\n' "$c"
            continue
        fi
        cp build/test-bios.img "$R/cf.img"
        run_serial "$c" "CF card (PC Card)" 40 "$QPCC" \
            -drive if=none,id=cf,format=raw,file="$R/cf.img" \
            -device falcon-pcc,drive=cf$([[ $c == cf3v ]] && echo ,volts=3) -- \
            'booting: CF card' 'module PCC' 'boot drive 0x81, sector size 512'
        rm -f "$R/cf.img"
        ;;
    devlink)
        if [[ ! -x "$CLIENT" ]]; then
            printf 'SKIP  %-9s no devlink client at %s (DEVLINK_CLIENT)\n' "$c" "$CLIENT"
            continue
        fi
        boot qemu-system-x86_64 -pty -enable-kvm
        WHY=
        choose "DevLink recovery" || true
        [[ -z "$WHY" ]] && { wait_for 'entering the kernel' 60 ||
            WHY="falcon never entered the kernel"; }
        AGENT=
        if [[ -z "$WHY" ]]; then
            for (( i = 0; i < 24; i++ )); do
                out=$(timeout 10 "$CLIENT" info --line "$PTY" 2>&1) &&
                    AGENT=$(printf '%s\n' "$out" | grep -a -m1 '^agent') || true
                [[ -n "$AGENT" ]] && break
                kill -0 "$QPID" 2>/dev/null || break
                sleep 5
            done
            [[ -n "$AGENT" ]] || WHY="the agent never answered on COM1"
        fi
        shot devlink
        halt
        result devlink "$([[ -z "$WHY" ]] && echo 1 || echo 0)" "${WHY:-$AGENT}"
        ;;
    windows)
        TIMEOUT=240 boot "$QEMU" -vga cirrus
        WHY=
        choose "Windows (NTLDR)" || true
        [[ -z "$WHY" ]] && { wait_for 'handing over' 30 ||
            WHY="the partition chain never handed over"; }
        if [[ -z "$WHY" ]]; then
            for (( i = 1; i <= 60; i++ )); do
                sleep 3
                shot "$(printf 'windows-%02d' "$i")"
            done
        fi
        halt
        result windows "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
            "${WHY:-handed over; screens in $R/windows-*.png (NTLDR, splash, desktop)}"
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
    rm -f "$R/overlay.qcow2"
done
exit "$FAILED"
