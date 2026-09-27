#!/usr/bin/env bash
# falcon-bios: boot the A21m's disk with the station on it under SeaBIOS
# (docs/DESIGN.md Section 13.11), as the A21m would: a Pentium III, 512 MB,
# the disk on IDE. mk-a21m-station.sh makes the disk: a qcow2 overlay of
# the original image with the station installed in it.
#
# Every boot runs on a fresh qcow2 overlay on top of that one, so Windows'
# chkdsk and whatever else a boot writes land in the throwaway overlay and
# the next run starts from the installed disk again.
#
# Cases:
#
#   menu     the station's menu is drawn (serial), and its screen saved;
#   default  nothing pressed: the countdown boots the default, Windows
#            2000, and screens are saved every two seconds until SECONDS
#            run out, for NTLDR's menu or the Windows 2000 splash to be
#            looked at;
#   enter    Enter on the menu's first row, then the same.
#
# The judge for the station's half is serial ("booting: " and the route's
# lines). Windows writes nothing to serial, so its half is the screens in
# build/a21m-rig/<case>-NN.png; a person, or an agent reading the image,
# says whether it is NTLDR's boot menu or the Windows 2000 splash.
#
# The disk is given the A21m's own geometry: 15 heads and 63 sectors
# physical, translated LARGE (ECHS) to the 240 heads its NTFS BPB and MBR
# were written with. SeaBIOS's default translation reports 255, and
# Windows 2000's NTFS boot code converts by the BPB's 240 but reads by
# CHS below the BIOS's cylinder limit, so under the default the untouched
# image stops at "NTLDR is missing" before falcon is involved at all.
#
# Usage: falcon-bios/tools/run-a21m-rig.sh [menu|default|enter] [SECONDS]
#   A21M_IMG   the installed disk (default build/a21m/a21m.qcow2)
#   QEMU       qemu-system-i386

set -euo pipefail

cd "$(dirname "$0")/../.."
IMG=${A21M_IMG:-build/a21m/a21m.qcow2}
QEMU=${QEMU:-qemu-system-i386}
CASE=${1:-menu}
SECS=${2:-90}
R=build/a21m-rig

[ -f "$IMG" ] || { echo "no $IMG: falcon-bios/tools/mk-a21m-station.sh" >&2; exit 2; }
command -v socat >/dev/null || { echo "socat is needed for the monitor" >&2; exit 2; }
mkdir -p "$R"
rm -f "$R/$CASE"-*.png "$R/mon.sock" "$R/serial.log" "$R/overlay.qcow2"
fmt=$(qemu-img info "$IMG" | sed -n 's/^file format: //p')
qemu-img create -q -f qcow2 -F "$fmt" -b "$(realpath "$IMG")" "$R/overlay.qcow2"

monitor() { echo "$1" | socat - UNIX-CONNECT:"$R/mon.sock" >/dev/null 2>&1 || true; }
log_text() { tr -d '\r' < "$R/serial.log" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'; }
shot() {      # shot NAME
    monitor "screendump $R/$1.ppm"
    sleep 0.5
    if [ -f "$R/$1.ppm" ]; then
        magick "$R/$1.ppm" "$R/$1.png" 2>/dev/null && rm -f "$R/$1.ppm"
    fi
}

timeout $((SECS + 60)) "$QEMU" -L /usr/share/qemu -machine pc -cpu pentium3 \
    -m 512 -display none \
    -drive if=none,id=d0,file="$R/overlay.qcow2",format=qcow2 \
    -device ide-hd,drive=d0,bus=ide.0,unit=0,cyls=20784,heads=15,secs=63,bios-chs-trans=large \
    -serial file:"$R/serial.log" \
    -monitor unix:"$R/mon.sock",server,nowait >/dev/null 2>&1 &
qpid=$!

menu=0
for _ in $(seq 1 300); do
    if log_text 2>/dev/null | grep -aq 'falcon-boot .*station'; then
        menu=1
        break
    fi
    sleep 0.1
done
sleep 1
shot "$CASE-00-menu"

if [ "$CASE" = enter ]; then
    monitor "sendkey ret"
fi
if [ "$CASE" != menu ]; then
    n=1
    end=$((SECONDS + SECS))
    while [ $SECONDS -lt $end ]; do
        sleep 2
        shot "$(printf '%s-%02d' "$CASE" "$n")"
        n=$((n + 1))
    done
fi
monitor quit
wait "$qpid" 2>/dev/null || true
log_text > "$R/$CASE-serial.txt"
rm -f "$R/overlay.qcow2"

if [ "$menu" -eq 0 ]; then
    echo "FAIL  $CASE  the station's menu never drew (serial: $R/$CASE-serial.txt)"
    exit 1
fi
echo "PASS  $CASE  the station's menu drew ($R/$CASE-00-menu.png)"
if [ "$CASE" != menu ]; then
    if grep -aq 'booting: ' "$R/$CASE-serial.txt"; then
        echo "PASS  $CASE  a row booted: $(grep -a 'booting: ' "$R/$CASE-serial.txt" | head -1 | sed 's/^ *//')"
    else
        echo "FAIL  $CASE  no row booted"
        exit 1
    fi
    echo "      screens: $R/$CASE-*.png (look for NTLDR's menu or the Windows 2000 splash)"
fi
