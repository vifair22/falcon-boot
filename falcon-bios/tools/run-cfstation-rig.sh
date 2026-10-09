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
#   freedos   the FreeDOS fixture (tools/fixtures/README.md) as the card:
#             C:\> on the screen, the swap on serial
#   win98     a Windows 98 SE system card (W98_CF: one active FAT32 LBA
#             partition at 63, FORMAT /S): its COMMAND.COM draws no prompt
#             until a key, so `ver` is typed once the handover is on
#             serial, and the answer and C:\> are judged from VGA text,
#             the swap from serial. Skipped without the image
#   win98-ios a Windows 98 SE installation on a card (W98_IOS_CF: one
#             active FAT32 partition at 63, a real-mode driver in
#             CONFIG.SYS serving the card by PIO at 0x1E0, the ports the
#             rig's card sits on), booted to its desktop: the screen is
#             watched until it leaves text mode and is then left to
#             settle; the Safe MBR Hook headers of the drive swap and the
#             reverse thunk (DESIGN 11.3.8, the structure Windows 98's I/O
#             supervisor reads at each INT 13h hook) are read out of
#             memory at the segments serial named; the machine is halted
#             and C:\WINDOWS\IOS.LOG read off the card's overlay. Judged
#             by the desktop and the headers. What IOS wrote about the
#             hooks is kept and quoted in the verdict: with the flags 0 it
#             lists falcon's hook as unknown, and a driver that takes the
#             hook over is what changes that. Skipped without the image
#
# Usage: falcon-bios/tools/run-cfstation-rig.sh [case...]   (default: all)
# Env:   A21M_IMG  the A21m's disk image as the internal disk (default: the
#                  stand-in, build/cfstation-rig/hdd.img)
#        CF_IMG    a card image to use instead of building one, for the
#                  station cases
#        FIXTURES  where freedos.img is (default build/fixtures)
#        W98_CF    the win98 case's card (default $FIXTURES/win98cf.img)
#        W98_IOS_CF  the win98-ios case's card (default
#                  $FIXTURES/cf-98-bridge.img)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/cfstation-rig
A21M_IMG=${A21M_IMG:-}
FIX=${FIXTURES:-build/fixtures}
W98_CF=${W98_CF:-$FIX/win98cf.img}
W98_IOS_CF=${W98_IOS_CF:-$FIX/cf-98-bridge.img}
QPCC=build/qemu-pcc/build/qemu-system-i386
MEMTEST=${MEMTEST:-build/memtest/mt86p_810_i586}
MACHINE=pc,x-south-bridge=piix4-isa
TIMEOUT=90
SIG_FOREIGN=5a7a0cf1
# DevLink recovery is on the card and in the menu only where its UKI or the
# images it is built from exist (apps/station-legacy/app.mk).
DEVLINK_UKI=build/devlink/devlink-recovery.efi
DEVLINK_IMAGES=${DEVLINK_IMAGES:-$HOME/git/home/devlink-rescue/build/classic/images}
DEVLINK=0
[[ -f $DEVLINK_UKI || -f $DEVLINK_IMAGES/bzImage ]] && DEVLINK=1

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
        extra=()
        if [[ $DEVLINK == 1 ]]; then
            [[ -f $DEVLINK_IMAGES/bzImage ]] &&
                make --no-print-directory devlink-uki >/dev/null
            extra=(--uki "$DEVLINK_UKI")
        fi
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
# The height of the screen as QEMU draws it now: 400 in VGA text mode, 480
# or more once Windows 98 has drawn its desktop.
screen_height() {
    rm -f "$R/probe.ppm"
    mon "screendump $R/probe.ppm"
    sleep 0.5
    [[ -s "$R/probe.ppm" ]] && sed -n '2s/^[0-9]* //p' "$R/probe.ppm" || echo 0
}

# A constant from biosdefs.h, as build.mk reads them.
bdef() { sed -n "s/^#define $1 *\(0x[0-9A-Fa-f]*\).*/\1/p" falcon-bios/src/start/biosdefs.h; }

# The Safe MBR Hook header at a resident's INT 13h entry, read out of the
# guest at SEG:OFF: the short jump, the signature and ID, smhnext when
# NEXT (eight hex digits as the bytes lie) is given, and flags 0. Prints
# what is wrong, nothing when the header is as biosdefs.h writes it.
hook_header() {  # hook_header NAME SEG OFF [NEXT]
    local f=$R/$1.hdr got
    rm -f "$f"
    mon "pmemsave $(( $2 * 16 + $3 )) 27 $f"
    sleep 0.3
    [[ -s "$f" ]] || { echo "no memory dump of the $1's header"; return; }
    got=$(od -An -tx1 -N3 "$f" | tr -d ' \n')
    [[ $got == eb1990 ]] || { echo "$1: no short jump at its entry ($got)"; return; }
    got=$(dd if="$f" bs=1 skip=3 count=16 status=none)
    [[ $got == '$INT13SFFALCON  ' ]] || { echo "$1: no signature and ID at its entry ('$got')"; return; }
    got=$(od -An -tx1 -j19 -N4 "$f" | tr -d ' \n')
    [[ -z "${4:-}" || $got == "$4" ]] || { echo "$1: smhnext is $got, not $4"; return; }
    got=$(od -An -tx1 -j23 -N4 "$f" | tr -d ' \n')
    [[ $got == 00000000 ]] || { echo "$1: flags are $got, not 0"; return; }
}

# Both disks on throwaway overlays, so neither a run nor Windows writes the
# card or the internal disk.
boot() {
    rm -f "$R/com1.log" "$R/mon.sock" "$R/hdd.qcow2" "$R/cf.qcow2"
    qemu-img create -q -f qcow2 -F raw -b "$(realpath "$HDD")" "$R/hdd.qcow2"
    qemu-img create -q -f qcow2 -F raw -b "$(realpath "$CARD")" "$R/cf.qcow2"
    timeout "$TIMEOUT" "$QPCC" -L build/qemu-pcc/share -L /usr/share/qemu \
        -machine "$MACHINE" \
        -cpu pentium3 -m 512 -display none -net none -vga cirrus -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img",readonly=on \
        -drive if=none,id=d0,file="$R/hdd.qcow2",format=qcow2 \
        -device ide-hd,drive=d0,bus=ide.0,unit=0$DISK \
        -drive if=none,id=cf,file="$R/cf.qcow2",format=qcow2 \
        -device falcon-pcc,addr=7.0,multifunction=on,drive=cf \
        -device falcon-pcc,addr=7.1 -device piix3-usb-uhci,id=hc \
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
# No rows is an answer, not an error: under pipefail grep's 1 would end the
# rig there, silently, instead of failing the case.
rows() {
    log_text "$R/com1.log" | sed -n '/entries, [0-9]* shown/,$p' |
        { grep -aoE "$ROWS" || true; } | sed 's/ *$//' | awk '!seen[$0]++'
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
[[ ${#CASES[@]} -gt 0 ]] || CASES=(menu windows cf foreign freedos win98 win98-ios)

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
            wants=('Windows (NTLDR)' 'Boot code' 'Memory test')
            [[ $DEVLINK == 1 ]] && wants+=('DevLink recovery')
            for want in "${wants[@]}"; do
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
    freedos|win98)
        if [[ $c == freedos ]]; then CARD=$FIX/freedos.img; else CARD=$W98_CF; fi
        if [[ ! -f "$CARD" ]]; then
            echo "SKIP  $c  no $CARD"
            continue
        fi
        rm -f "$R/$c.vga" "$R/$c.txt"
        boot
        WHY=
        if [[ $c == win98 ]]; then
            wait_for 'handing over' 60 && sleep 20
            for k in v e r ret; do mon "sendkey $k"; sleep 0.3; done
            wait_prompt "$c" 30 && grep -aq 'Windows 98 \[Version' "$R/$c.txt" ||
                WHY="no Windows 98 version line and C:\\> on screen ($R/$c.txt, $R/$c.png)"
        else
            wait_prompt "$c" 90 || WHY="no C:\\> on screen ($R/$c.txt, $R/$c.png)"
        fi
        halt
        [[ -z "$WHY" ]] && WHY=$(swap_missing)
        result "$c" "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
            "${WHY:-C:\\> on screen, card swapped with 0x80 ($R/$c.png)}"
        ;;
    win98-ios)
        CARD=$W98_IOS_CF
        if [[ ! -f "$CARD" ]]; then
            echo "SKIP  $c  no $CARD"
            continue
        fi
        rm -f "$R/$c.vga" "$R/$c.txt" "$R/$c.png" "$R/$c.img" "$R/$c.ioslog"
        TIMEOUT=480 boot
        WHY=
        wait_for 'handing over' 60 || WHY="jump never handed over"
        # IOS runs and writes its log before the desktop is drawn, and the
        # desktop is the first screen taller than text mode's 400 lines
        # (the logo is 320x400). Left to settle after it, for the lazy
        # writer.
        if [[ -z "$WHY" ]]; then
            up=0
            for (( i = 0; i < 60; i++ )); do
                sleep 5
                kill -0 "$QPID" 2>/dev/null || break
                [[ "$(screen_height)" -ge 480 ]] && { up=1; break; }
            done
            if [[ $up -eq 1 ]]; then
                sleep 90
            else
                WHY="the screen never left text mode in 300 s"
            fi
            mon "pmemsave 0xb8000 4000 $R/$c.vga"
            sleep 0.2
            [[ -s "$R/$c.vga" ]] && vga_text "$R/$c.vga" > "$R/$c.txt"
            shot "$c"
        fi
        # The headers, where serial says the blocks went: the swap's
        # smhnext must be the reverse thunk's entry, the hook it replaced.
        HDR=
        if kill -0 "$QPID" 2>/dev/null; then
            swapseg=$(log_text "$R/com1.log" |
                grep -aoE 'INT 13h resident at segment 0x[0-9a-f]+' |
                grep -oE '0x[0-9a-f]+$' | tail -1)
            thunk=$(log_text "$R/com1.log" |
                grep -aoE 'reverse thunk for drive 0x81 at 0x[0-9a-f]+' |
                grep -oE '0x[0-9a-f]+$' | tail -1)
            if [[ -n "$swapseg" && -n "$thunk" ]]; then
                next=$(printf '%02x%02x%02x%02x' \
                    $(( $(bdef JRES_OFF_CODE) & 0xFF )) \
                    $(( $(bdef JRES_OFF_CODE) >> 8 )) \
                    $(( thunk / 16 & 0xFF )) $(( thunk / 16 >> 8 )))
                HDR=$(hook_header swap "$swapseg" "$(bdef SWAP_OFF_CODE)" "$next")
                [[ -n "$HDR" ]] ||
                    HDR=$(hook_header thunk "$(( thunk / 16 ))" "$(bdef JRES_OFF_CODE)")
            else
                HDR="serial names no swap segment or thunk address"
            fi
        fi
        halt
        [[ -z "$WHY" ]] && WHY=$(swap_missing)
        [[ -z "$WHY" ]] && WHY=$HDR
        # The card's overlay flattened, and the log copied out of the
        # partition the MBR's first entry starts at.
        IOS=
        if [[ -z "$WHY" ]]; then
            qemu-img convert -O raw "$R/cf.qcow2" "$R/$c.img"
            start=$(od -An -tu4 -j454 -N4 "$R/$c.img" | tr -d ' ')
            if mcopy -n -i "$R/$c.img@@$(( start * 512 ))" \
                    ::/WINDOWS/IOS.LOG "$R/$c.ioslog" 2>/dev/null; then
                IOS=$(grep -ai 'int 13 hooker' "$R/$c.ioslog" | tr -d '\r' |
                    paste -sd ';' | sed 's/;/; /g')
                IOS="IOS.LOG: ${IOS:-no INT 13h hooker named} ($R/$c.ioslog)"
            else
                WHY="no C:\\WINDOWS\\IOS.LOG on the card ($R/$c.txt, $R/$c.png)"
            fi
            rm -f "$R/$c.img"
        fi
        result "$c" "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
            "${WHY:-desktop up over both headers; $IOS; card swapped with 0x80}"
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
