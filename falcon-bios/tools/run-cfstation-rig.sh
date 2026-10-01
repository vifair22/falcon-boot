#!/usr/bin/env bash
# station-legacy on a CompactFlash card, reached through falcon jump
# (docs/DESIGN.md Sections 12.10 and 13): the shape a card is tried in on
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
# The internal disk is station-legacy on a stand-in for the A21m's disk
# (mk-legacy-disk.sh: a Windows (NTLDR) partition at 63 whose boot sector
# is rigboot.S), or the A21m's own image when A21M_IMG names it; either on
# a throwaway qcow2 overlay, never opened for writing. The machine is
# QEMU's pc with the A21m's PIIX4 south bridge, a Pentium III with 512 MB,
# the rig's PC Card bridge (falcon-pcc.c) and a UHCI controller.
#
#   menu      the station's menu comes up off the card: its rows, in the
#             order drawn, are recorded; the internal disk's boot code,
#             DevLink recovery and memtest must be among them
#   windows   the internal disk's Windows chained from the card: jump's
#             resident must be released before the handover (Windows 2000
#             froze on the A21m started on top of it; QEMU's video BIOS
#             never shows that, so the release is judged from serial). On
#             the A21m's image the screens are kept
#             (build/cfstation-rig/windows-*.png); on the stand-in its
#             rigboot.S reports DL 0x80, the internal disk as 0x80 and one
#             disk counted
#   cf        the station's own CF card row, taken while jump's resident
#             already drives the bridge: recorded, not judged, since two
#             drivers for one bridge is the case this rig is here to see
#
# Cards that carry no falcon record, booted by jump's CF route behind the
# drive swap (DESIGN Section 13.6):
#
#   foreign   a card whose LBA 0 is rigboot.S: DL 0x80, 0x80 reads the
#             card, the BDA counts two disks and 0x81 reads the internal
#             disk
#   freedos   the FreeDOS fixture (tools/fixtures/README.md) as the card,
#             for C:\> on the screen and the swap on serial. A known
#             failure, reported as KNOWN and not counted: the kernel finds
#             the card as C: and the internal disk as D:, then stops with
#             an invalid opcode (with HIMEMX and DOS=HIGH) or a divide by
#             zero (without) before COMMAND.COM is read
#
# Usage: falcon-bios/tools/run-cfstation-rig.sh [case...]   (default: all)
# Env:   A21M_IMG  the A21m's disk image as the internal disk (default: the
#                  stand-in, build/cfstation-rig/hdd.img)
#        CF_IMG    a card image to use instead of building one, for the
#                  station cases
#        FIXTURES  where freedos.img is (default build/fixtures)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/cfstation-rig
A21M_IMG=${A21M_IMG:-}
FIX=${FIXTURES:-build/fixtures}
QPCC=build/qemu-pcc/build/qemu-system-i386
MEMTEST=${MEMTEST:-build/memtest/mt86p_810_i586}
MACHINE=pc,x-south-bridge=piix4-isa
TIMEOUT=90
SIG_FOREIGN=5a7a0cf1
DEVLINK_UKI=build/devlink/devlink-recovery.efi

command -v socat >/dev/null || { echo "socat is needed for the monitor" >&2; exit 2; }
[[ -x "$QPCC" ]] || { echo "no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)" >&2; exit 2; }
mkdir -p "$R"
make --no-print-directory station-legacy build/rigboot.bin >/dev/null

# The internal disk: the A21m's image with its own geometry, or the
# stand-in.
if [[ -n "$A21M_IMG" ]]; then
    [[ -f "$A21M_IMG" ]] || { echo "no A21m image at $A21M_IMG (A21M_IMG)" >&2; exit 2; }
    HDD=$A21M_IMG
    DISK=,cyls=20784,heads=15,secs=63,bios-chs-trans=large
else
    HDD=$R/hdd.img
    DISK=
    falcon-bios/tools/mk-legacy-disk.sh "$HDD" >"$R/hdd-install.log" 2>&1 ||
        { tail -20 "$R/hdd-install.log"; exit 1; }
fi
SIG_HDD=$(od -An -tx4 -j440 -N4 "$HDD" | tr -d ' \n')

# The card the station cases boot: station-legacy, built once.
STATION_CARD=
station_card() {
    if [[ -n "${CF_IMG:-}" ]]; then
        STATION_CARD=$CF_IMG
    elif [[ -z "$STATION_CARD" ]]; then
        STATION_CARD=$R/cf.img
        rm -f "$STATION_CARD" "$R/cf.bak"
        truncate -s 2000M "$STATION_CARD"
        printf 'label: dos\nunit: sectors\nstart=2048, size=1048576, type=c\n' |
            sfdisk -q "$STATION_CARD"
        mkfs.fat -F 32 --offset 2048 -n CFDATA "$STATION_CARD" 524288 \
            >/dev/null 2>&1
        # DevLink recovery's UKI as built, or from its images when absent.
        [[ -f "$DEVLINK_UKI" ]] ||
            make --no-print-directory devlink-uki >/dev/null
        extra=(--uki "$DEVLINK_UKI")
        [[ -f "$MEMTEST" ]] && extra+=(--memtest "$MEMTEST")
        build/bin/falcon-boot-mkstick station install "$STATION_CARD" --bios \
            --legacy "${extra[@]}" \
            --backup "$R/cf.bak" >"$R/install.log" 2>&1 ||
            { tail -20 "$R/install.log"; exit 1; }
    fi
    CARD=$STATION_CARD
}

# A card with no falcon record: rigboot.S as its MBR code over a table
# with one FAT32 partition.
foreign_card() {
    CARD=$R/foreign.img
    rm -f "$CARD"
    truncate -s 64M "$CARD"
    printf 'label: dos\nlabel-id: 0x%s\nstart=2048, size=129024, type=c, bootable\n' \
        "$SIG_FOREIGN" | sfdisk -q --no-reread --no-tell-kernel "$CARD" >/dev/null
    dd if=build/rigboot.bin of="$CARD" bs=440 count=1 conv=notrunc status=none
}

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
# after FIRST PATTERN: PATTERN in the serial log below FIRST's line.
after() {
    log_text "$R/com1.log" | sed -n "/$1/,\$p" | grep -aE -- "$2" >/dev/null
}
wait_after() {  # wait_after FIRST PATTERN SECONDS
    local i
    for (( i = 0; i < $3 * 10; i++ )); do
        after "$1" "$2" && return 0
        kill -0 "$QPID" 2>/dev/null || { after "$1" "$2"; return; }
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
# card or the internal disk.
boot() {
    rm -f "$R/com1.log" "$R/mon.sock" "$R/hdd.qcow2" "$R/cf.qcow2"
    qemu-img create -q -f qcow2 -F raw -b "$(realpath "$HDD")" "$R/hdd.qcow2"
    qemu-img create -q -f qcow2 -F raw -b "$(realpath "$CARD")" "$R/cf.qcow2"
    timeout "$TIMEOUT" "$QPCC" -L /usr/share/qemu -machine "$MACHINE" \
        -cpu pentium3 -m 512 -display none -net none -vga cirrus -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img",readonly=on \
        -drive if=none,id=d0,file="$R/hdd.qcow2",format=qcow2 \
        -device ide-hd,drive=d0,bus=ide.0,unit=0$DISK \
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

vga_text() {  # vga_text FILE: the 80x25 text screen dumped from 0xB8000
    python3 - "$1" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
for r in range(25):
    print(''.join(chr(c) if 32 <= c < 127 else ' '
                  for c in b[r * 160:(r + 1) * 160:2]).rstrip())
PY
}

# Waits for a DOS prompt in VGA text memory; 0 when it came. The text is
# kept in $R/CASE.txt and a screendump in $R/CASE.png.
wait_prompt() {  # wait_prompt CASE SECONDS
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
    shot "$1"
    grep -aq 'C:\\>' "$R/$1.txt" 2>/dev/null
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

# The lines of the CF route's handover to a card with no falcon record.
SWAP_LINES=('drive 0x81 carries no falcon boot record; swapping it with 0x80'
            'MBR of drive 0x81 to 0x7c00, DL 0x80'
            'drive 0x81 swapped with 0x80')
swap_missing() {
    local p
    for p in "${SWAP_LINES[@]}"; do
        has "$p" || { echo "no '$p' on serial"; return; }
    done
}

CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] || CASES=(menu windows cf foreign freedos)

for c in "${CASES[@]}"; do
    case $c in
    menu)
        station_card
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
        station_card
        if [[ -n "$A21M_IMG" ]]; then TIMEOUT=240 boot; else boot; fi
        choose 'Windows (NTLDR)' || true
        # jump's own route says "handing over" first; the chain's is the
        # one after the row's entry line.
        [[ -z "$WHY" ]] && { wait_for 'entry mbr-.*-p1' 30 &&
            wait_after 'entry mbr-.*-p1' 'handing over' 30 ||
            WHY="the chain never handed over"; }
        [[ -z "$WHY" ]] && ! after 'entry mbr-.*-p1' \
            "released falcon jump's resident for drive 0x81" &&
            WHY="jump's resident was not released before the handover" 
        if [[ -z "$WHY" && -n "$A21M_IMG" ]]; then
            for (( i = 1; i <= 60; i++ )); do
                sleep 3
                shot "$(printf 'windows-%02d' "$i")"
            done
            DONE="handed over; screens in $R/windows-*.png"
        elif [[ -z "$WHY" ]]; then
            want="RIGBOOT dl=80 .* sig80=$SIG_HDD sigdl=$SIG_HDD ndisks=01"
            wait_for "$want" 20 || WHY="no /$want/ on serial"
            DONE=$(log_text "$R/com1.log" | grep -ao 'RIGBOOT.*' | tail -1)
        fi
        halt
        result windows "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
            "${WHY:-$DONE}"
        ;;
    cf)
        station_card
        boot
        choose 'CF card' || true
        sleep 20
        shot cf
        halt
        echo "NOTE  cf       ${WHY:-taken; its serial is $R/cf.log, its screen $R/cf.png}"
        log_text "$R/com1.log" > "$R/cf.log"
        ;;
    foreign)
        foreign_card
        boot
        want="RIGBOOT dl=80 dssi=0000:0000 .* sig80=$SIG_FOREIGN sigdl=$SIG_FOREIGN ndisks=02 sig81=$SIG_HDD"
        WHY=
        wait_for 'RIGBOOT.*sig81=[0-9a-f-]{8}' 60 || WHY="rigboot never reported"
        halt
        [[ -z "$WHY" ]] && WHY=$(swap_missing)
        [[ -z "$WHY" ]] && ! has "$want" && WHY="no /$want/ on serial"
        result foreign "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
            "${WHY:+$WHY; }$(log_text "$R/com1.log" | grep -ao 'RIGBOOT.*' | tail -1)"
        ;;
    freedos)
        if [[ ! -f "$FIX/freedos.img" ]]; then
            echo "SKIP  freedos  no $FIX/freedos.img (tools/fixtures/README.md)"
            continue
        fi
        CARD=$FIX/freedos.img
        rm -f "$R/freedos.vga" "$R/freedos.txt"
        boot
        WHY=
        wait_prompt freedos 90 || WHY="no C:\\> on screen ($R/freedos.txt, $R/freedos.png)"
        halt
        [[ -z "$WHY" ]] && WHY=$(swap_missing)
        log_text "$R/com1.log" > "$R/freedos.log"
        if [[ -z "$WHY" ]]; then
            result freedos 1 "C:\\> on screen, card swapped with 0x80 ($R/freedos.png)"
        else
            printf 'KNOWN %-8s %s; screen ends: %s\n' freedos "$WHY" \
                "$(grep -v '^$' "$R/freedos.txt" 2>/dev/null | tail -2 | paste -sd' ')"
        fi
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
    rm -f "$R/hdd.qcow2" "$R/cf.qcow2"
done
rm -f "$R/foreign.img"
exit "$FAILED"
