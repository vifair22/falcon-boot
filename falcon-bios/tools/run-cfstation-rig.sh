#!/usr/bin/env bash
# station-legacy on a CompactFlash card, reached through falcon jump
# (docs/DESIGN.md Sections 13.10 and 14): the shape a card is tried in on
# the A21m before station-legacy goes onto its internal disk.
#
# The A21m's BIOS cannot boot a PC Card, so the machine starts falcon jump
# off the floppy, jump's CF route boots the card as drive 0x81 through its
# resident, and the card's own stages bring up station-legacy. Everything
# the station then does runs on top of that resident, which an install on
# the internal disk never has under it.
#
# The card is built here as the installer builds any disk: a 2000 MiB
# image with an MBR and a FAT32 data partition (a card's station goes
# beside something, as it does on a machine's disk), then `mkstick station
# install --bios --legacy` with DevLink recovery in /uki and memtest86+.
# The internal disk is the A21m's own image (A21M_IMG) on a throwaway
# qcow2 overlay, never opened for writing. The machine is QEMU's pc with
# the A21m's PIIX4 south bridge, a Pentium III with 512 MB, the rig's PC
# Card bridge (falcon-pcc.c) and a UHCI controller.
#
#   menu      the station's menu comes up off the card: its rows, in the
#             order drawn, are recorded; the internal disk's boot code,
#             DevLink recovery and memtest must be among them
#   windows   the internal disk's boot code chained from the card:
#             Windows 2000 from its own disk, judged from screendumps
#             (build/cfstation-rig/windows-*.png)
#   cf        the station's own CF card row, taken while jump's resident
#             already drives the bridge: recorded, not judged, since two
#             drivers for one bridge is the case this rig is here to see
#
# Usage: falcon-bios/tools/run-cfstation-rig.sh [case...]   (default: all)
# Env:   A21M_IMG  the A21m's disk image (default ~/a21m-image/DJSA-210.img)
#        CF_IMG    a card image to use instead of building one
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/cfstation-rig
A21M_IMG=${A21M_IMG:-$HOME/a21m-image/DJSA-210.img}
QPCC=build/qemu-pcc/build/qemu-system-i386
MEMTEST=${MEMTEST:-build/memtest/mt86p_810_i586}
MACHINE=pc,x-south-bridge=piix4-isa
DISK=cyls=20784,heads=15,secs=63,bios-chs-trans=large
TIMEOUT=90

command -v socat >/dev/null || { echo "socat is needed for the monitor" >&2; exit 2; }
[[ -x "$QPCC" ]] || { echo "no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)" >&2; exit 2; }
[[ -f "$A21M_IMG" ]] || { echo "no A21m image at $A21M_IMG (A21M_IMG)" >&2; exit 2; }
mkdir -p "$R"
make --no-print-directory station-legacy devlink-uki >/dev/null

# The card.
if [[ -n "${CF_IMG:-}" ]]; then
    CARD=$CF_IMG
else
    CARD=$R/cf.img
    rm -f "$CARD" "$R/cf.bak"
    truncate -s 2000M "$CARD"
    printf 'label: dos\nunit: sectors\nstart=2048, size=1048576, type=c\n' |
        sfdisk -q "$CARD"
    mkfs.fat -F 32 --offset 2048 -n CFDATA "$CARD" 524288 >/dev/null 2>&1
    extra=()
    [[ -f "$MEMTEST" ]] && extra=(--memtest "$MEMTEST")
    build/bin/falcon-boot-mkstick station install "$CARD" --bios --legacy \
        --uki build/devlink/devlink-recovery.efi "${extra[@]}" \
        --backup "$R/cf.bak" >"$R/install.log" 2>&1 ||
        { tail -20 "$R/install.log"; exit 1; }
fi

# The floppy: jump straight to the CF route.
printf 'default=cf\ntimeout=1\n' > "$R/JUMP.INI"
make --no-print-directory jump JUMP_INI="$R/JUMP.INI" >/dev/null
cp build/bin/falcon-jump.img "$R/jump.img"

QPID=
cleanup() { [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null || true; }
trap cleanup EXIT

log_text() {
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
shot() {
    mon "screendump $R/$1.ppm"
    sleep 0.5
    [[ -f "$R/$1.ppm" ]] && magick "$R/$1.ppm" "$R/$1.png" 2>/dev/null &&
        rm -f "$R/$1.ppm"
    return 0
}

# Both disks on throwaway overlays, so neither a run nor Windows writes the
# card or the A21m's image.
boot() {
    rm -f "$R/com1.log" "$R/mon.sock" "$R/hdd.qcow2" "$R/cf.qcow2"
    qemu-img create -q -f qcow2 -F raw -b "$(realpath "$A21M_IMG")" "$R/hdd.qcow2"
    qemu-img create -q -f qcow2 -F raw -b "$(realpath "$CARD")" "$R/cf.qcow2"
    timeout "$TIMEOUT" "$QPCC" -L /usr/share/qemu -machine "$MACHINE" \
        -cpu pentium3 -m 512 -display none -net none -vga cirrus -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img",readonly=on \
        -drive if=none,id=d0,file="$R/hdd.qcow2",format=qcow2 \
        -device ide-hd,drive=d0,bus=ide.0,unit=0,$DISK \
        -drive if=none,id=cf,file="$R/cf.qcow2",format=qcow2 \
        -device falcon-pcc,drive=cf -device piix3-usb-uhci,id=hc \
        -serial file:"$R/com1.log" -monitor unix:"$R/mon.sock",server,nowait \
        >"$R/qemu.log" 2>&1 &
    QPID=$!
}
halt() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
}

# The station's rows in the order drawn. With no default in STATION.INI
# the menu has no countdown, so the menu is waited for by its entry count.
ROWS='Boot code \(disk [^)]*\)|Windows \(NTLDR\)|CD drive|CF card \(PC Card\)|USB stick|DevLink recovery|Memory test'
rows() {
    log_text "$R/com1.log" | sed -n '/entries, [0-9]* shown/,$p' |
        grep -aoE "$ROWS" | sed 's/ *$//' | awk '!seen[$0]++'
}
menu_up() { wait_for 'station-legacy.*entries, [0-9]* shown|entries, [0-9]* shown' 60; }
choose() {      # choose LABEL-PREFIX
    local n i
    WHY=
    menu_up || { WHY="station-legacy's menu never drew"; return 1; }
    sleep 1
    n=$(rows | grep -nF -- "$1" | head -1 | cut -d: -f1)
    [[ -n "$n" ]] || { WHY="no row '$1' (rows: $(rows | paste -sd,))"; return 1; }
    for (( i = 0; i < 8; i++ )); do mon "sendkey up"; sleep 0.1; done
    for (( i = 1; i < n; i++ )); do mon "sendkey down"; sleep 0.2; done
    mon "sendkey ret"
    return 0
}

FAILED=0
result() {      # result CASE OK DETAIL
    if [[ $2 -eq 1 ]]; then
        printf 'PASS  %-8s %s\n' "$1" "$3"
    else
        printf 'FAIL  %-8s %s\n' "$1" "$3"
        FAILED=1
    fi
    log_text "$R/com1.log" > "$R/$1.log"
}

CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] || CASES=(menu windows cf)

for c in "${CASES[@]}"; do
    case $c in
    menu)
        boot
        WHY=
        menu_up || WHY="station-legacy's menu never drew"
        sleep 1
        shot menu
        halt
        got=$(rows)
        if [[ -z "$WHY" ]]; then
            for want in 'Windows (NTLDR)' 'Boot code' 'DevLink recovery' 'Memory test'; do
                grep -qF -- "$want" <<<"$got" || WHY="no '$want' row"
            done
        fi
        result menu "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
            "${WHY:+$WHY; }rows: $(paste -sd, <<<"$got")"
        ;;
    windows)
        TIMEOUT=240 boot
        choose 'Windows (NTLDR)' || true
        [[ -z "$WHY" ]] && { wait_for 'handing over' 30 ||
            WHY="the chain never handed over"; }
        if [[ -z "$WHY" ]]; then
            for (( i = 1; i <= 60; i++ )); do
                sleep 3
                shot "$(printf 'windows-%02d' "$i")"
            done
        fi
        halt
        result windows "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
            "${WHY:-handed over; screens in $R/windows-*.png}"
        ;;
    cf)
        boot
        choose 'CF card' || true
        sleep 20
        shot cf
        halt
        echo "NOTE  cf       ${WHY:-taken; its serial is $R/cf.log, its screen $R/cf.png}"
        log_text "$R/com1.log" > "$R/cf.log"
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
    rm -f "$R/hdd.qcow2" "$R/cf.qcow2"
done
exit "$FAILED"
