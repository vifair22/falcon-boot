#!/usr/bin/env bash
# falcon-boot: install Windows 95 OSR2 or 98 SE unattended onto a fixture disk.
#
# The station rig needs disks with an old Windows on them that boot on their
# own, as a machine's existing OS the station finds and chainloads. This makes
# one the way a person would have in 1999, with every question answered in
# advance:
#
#   1. A throwaway install floppy is cut down from a DOS boot floppy (the
#      Windows 98 CD's El Torito image, or a Windows 95 startup disk): its
#      own IO.SYS and COMMAND.COM, HIMEM, FDISK and FORMAT, and a CONFIG.SYS
#      and AUTOEXEC.BAT of ours that run the rest without a keystroke.
#   2. A second disk, D:, carries Setup's source directory off the CD and the
#      msbatch.inf with the product key filled in. The CD itself is never
#      attached, so the floppy needs no CD-ROM driver.
#   3. The target, C:, gets one active FAT32 (LBA, type 0x0C) primary
#      partition in its table and nothing else. From the floppy, `fdisk /mbr`
#      writes Microsoft's MBR code, `format c: /autotest` formats it without
#      asking (the flag is undocumented and exists in every 9x FORMAT), the
#      source directory is copied to C:\<dir> so the installed system never
#      asks for D: again, and Setup runs with the batch file.
#   4. QEMU boots the floppy once (-boot once=a) and the disk ever after, so
#      Setup's own reboots come up from C:. The script watches the screen
#      until it shows the desktop, shuts Windows down from the Start menu,
#      and stops the guest.
#
# Where Setup still stops, it is nudged. An OEM CD (the 98 SE one here) runs
# a Start Wizard on the first boot after the copy that asks again for the
# name, the licence and the key, prefilled from msbatch.inf, and no batch
# setting skips it. When the screen has not changed for --still polls, the
# script sends --nudge: the default, Alt+A then Enter, is Next on the name
# and key pages, "I accept" and Next on the licence page and Finish on the
# last, and does nothing harmful to a progress screen that is merely slow.
# Windows 95's licence page is a Yes/No dialog instead, so it wants
# --nudge "alt-y ret".
#
# msbatch.inf, which carries the key, lives on D:, and D: and the floppy are
# deleted when the install ends. Setup copies the file to C:\WINDOWS, and that
# copy is deleted once the guest is off. The key stays in the installed
# registry, as it does in every installed Windows.
#
# Usage: mk-win9x-fixture.sh [options] --iso ISO --srcdir DIR --inf TEMPLATE
#                            --key-file FILE OUT.img
#   --iso ISO          the Windows CD image
#   --srcdir DIR       Setup's directory on it (WIN98, WIN95)
#   --floppy IMG       a DOS boot floppy to cut down; default: the ISO's own
#                      El Torito floppy image
#   --inf TEMPLATE     msbatch.inf with @PRODUCTKEY@ where the key goes
#   --key-file FILE    the product key, alone on its first line
#   --size-mb N        target disk size (default 2048)
#   --setup-args ARGS  extra Setup switches (default "/is /ie")
#   --cpu MODEL        QEMU CPU model (default pentium2)
#   --mem MB           guest memory (default 128)
#   --timeout MIN      give up after this long (default 240)
#   --shots DIR        keep a screenshot every poll here (default: none)
#   --nudge KEYS       sendkey names sent to a screen that has stopped
#                      changing (default "alt-a ret"; "" never nudges)
#   --still N          polls a minute apart before a screen counts as
#                      stopped (default 3)
# Needs qemu-system-i386, mtools, xorriso and python3; the harness image
# (tools/coverage/Dockerfile) has them all.

set -euo pipefail

ISO= SRCDIR= FLOPPY= INF= KEYFILE= SIZE_MB=2048 SETUP_ARGS="/is /ie"
CPU=pentium2 MEM=128 TIMEOUT_MIN=240 SHOTS= NUDGE="alt-a ret" STILL=3
while [[ $# -gt 0 ]]; do
    case "$1" in
    --iso) ISO=$2; shift 2 ;;
    --srcdir) SRCDIR=$2; shift 2 ;;
    --floppy) FLOPPY=$2; shift 2 ;;
    --inf) INF=$2; shift 2 ;;
    --key-file) KEYFILE=$2; shift 2 ;;
    --size-mb) SIZE_MB=$2; shift 2 ;;
    --setup-args) SETUP_ARGS=$2; shift 2 ;;
    --cpu) CPU=$2; shift 2 ;;
    --mem) MEM=$2; shift 2 ;;
    --timeout) TIMEOUT_MIN=$2; shift 2 ;;
    --shots) SHOTS=$2; shift 2 ;;
    --nudge) NUDGE=$2; shift 2 ;;
    --still) STILL=$2; shift 2 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "ERROR: unknown option $1" >&2; exit 1 ;;
    *) break ;;
    esac
done
[[ $# -eq 1 && -n $ISO && -n $SRCDIR && -n $INF && -n $KEYFILE ]] || {
    echo "usage: $0 --iso ISO --srcdir DIR --inf TEMPLATE --key-file FILE [opts] OUT.img" >&2
    exit 1; }
OUT=$1
HERE=$(cd "$(dirname "$0")" && pwd)
export MTOOLS_SKIP_CHECK=1 MTOOLS_NO_VFAT=1

WORK=$(mktemp -d "$(dirname "$OUT")/.w9x.XXXXXX")
QPID=
cleanup() {
    [[ -n $QPID ]] && kill "$QPID" 2>/dev/null || true
    # D: holds the key; it goes whatever happened.
    rm -rf "$WORK"
}
trap cleanup EXIT
MON=$WORK/mon.sock
[[ -n $SHOTS ]] && mkdir -p "$SHOTS"

[[ -n $(head -n1 "$KEYFILE" | tr -d '\r\n ') ]] || {
    echo "ERROR: $KEYFILE has no key on its first line" >&2; exit 1; }

# Write an MBR partition table with one active primary partition and no boot
# code; `fdisk /mbr` in the guest supplies the code. CHS fields are the LBA
# placeholders every partitioner writes past cylinder 1023.
mbr_table() { # IMG TYPE START_LBA
    python3 - "$@" <<'EOF'
import os, struct, sys
img, ptype, start = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
total = os.path.getsize(img) // 512
entry = struct.pack("<B3sB3sII", 0x80, b"\x01\x01\x00", ptype,
                    b"\xfe\xff\xff", start, total - start)
with open(img, "r+b") as f:
    f.seek(446); f.write(entry + b"\x00" * 48)
    f.seek(510); f.write(b"\x55\xaa")
EOF
}

# --- 1. the install floppy -------------------------------------------------
FD=$WORK/install.img
if [[ -z $FLOPPY ]]; then
    xorriso -osirrox on -indev "$ISO" -extract_boot_images "$WORK/eltorito/" \
        >/dev/null 2>&1
    head -c 1474560 "$WORK/eltorito/eltorito_img1_bios.img" > "$FD"
else
    cp "$FLOPPY" "$FD"
fi

# Keep the system files and the four tools the batch runs; everything else on
# a startup floppy is CD-ROM drivers and the RAM-drive toolkit, which would
# only crowd out FORMAT. The system files are kept by name, since not every
# startup disk marks them hidden (the OSR2.5 EBD image does not).
mdir -b -i "$FD" ::/ | tr -d '\r' | while read -r f; do
    case "${f##*/}" in
    IO.SYS|MSDOS.SYS|DRVSPACE.BIN|COMMAND.COM|HIMEM.SYS|FDISK.EXE|FORMAT.COM|SYS.COM) ;;
    *) mdeltree -i "$FD" "$f" 2>/dev/null || mdel -i "$FD" "$f" ;;
    esac
done
# A JO.SYS left behind would put the CD's "boot from hard disk or CD"
# question back on screen.
mattrib -i "$FD" -h -s -r ::/JO.SYS 2>/dev/null && mdel -i "$FD" ::/JO.SYS || true

# FORMAT.COM from the CD's source directory when the floppy lacks it (the 98
# CD floppy keeps it inside EBD.CAB).
if ! mdir -i "$FD" ::/FORMAT.COM >/dev/null 2>&1; then
    for d in "$SRCDIR" "${SRCDIR,,}"; do
        for f in FORMAT.COM format.com; do
            xorriso -osirrox on -indev "$ISO" -extract "/$d/$f" "$WORK/FORMAT.COM" \
                >/dev/null 2>&1 && break 2
        done
    done
    [[ -s $WORK/FORMAT.COM ]] || { echo "ERROR: no FORMAT.COM on the floppy or in $SRCDIR" >&2; exit 1; }
    chmod u+w "$WORK/FORMAT.COM"
    mcopy -i "$FD" "$WORK/FORMAT.COM" ::/FORMAT.COM
fi

printf '%s\r\n' 'DEVICE=A:\HIMEM.SYS /TESTMEM:OFF' 'FILES=40' 'BUFFERS=30' \
    'LASTDRIVE=Z' > "$WORK/CONFIG.SYS"
S=${SRCDIR^^}
printf '%s\r\n' \
    '@ECHO OFF' \
    'ECHO falcon fixture: partition, format, copy, Setup' \
    'FDISK /MBR' \
    'FORMAT C: /AUTOTEST /V:FIXTURE' \
    "IF EXIST D:\\$S\\SMARTDRV.EXE D:\\$S\\SMARTDRV.EXE 4096" \
    "MD C:\\$S" \
    "COPY D:\\$S\\*.* C:\\$S > NUL" \
    'C:' \
    "CD \\$S" \
    "SETUP D:\\MSBATCH.INF $SETUP_ARGS" \
    > "$WORK/AUTOEXEC.BAT"
mcopy -o -i "$FD" "$WORK/CONFIG.SYS" "$WORK/AUTOEXEC.BAT" ::/
mdir -i "$FD" ::/ | tr -d '\r' | grep -E '^[A-Z]' || true

# --- 2. the source disk, D: ------------------------------------------------
SRC=$WORK/source.img
truncate -s 1G "$SRC"
mbr_table "$SRC" 0c 63
mformat -i "$SRC@@$((63 * 512))" -F -T $(((1024 * 2048) - 63)) -h 16 -s 63 -H 63 -v SOURCE ::
xorriso -osirrox on -indev "$ISO" -extract "/$SRCDIR" "$WORK/src" >/dev/null 2>&1 ||
    xorriso -osirrox on -indev "$ISO" -extract "/${SRCDIR,,}" "$WORK/src" >/dev/null 2>&1
chmod -R u+w "$WORK/src"
mcopy -s -i "$SRC@@$((63 * 512))" "$WORK/src" "::/$S"
rm -rf "$WORK/src"
# The key is read by python from the file, so it never appears in a command
# line (ps) or in a file outside $WORK.
python3 - "$INF" "$KEYFILE" <<'EOF2' | mcopy -i "$SRC@@$((63 * 512))" - ::/MSBATCH.INF
import sys
tmpl = open(sys.argv[1]).read()
key = open(sys.argv[2]).readline().strip()
sys.stdout.write(tmpl.replace("@PRODUCTKEY@", key).replace("\n", "\r\n"))
EOF2

# --- 3. the target, C: -----------------------------------------------------
rm -f "$OUT"
truncate -s "${SIZE_MB}M" "$OUT"
mbr_table "$OUT" 0c 63

# --- 4. install ------------------------------------------------------------
# TCG and a period CPU model: 9x's timing loops fault on a host-speed CPU.
qemu_start() { # BOOT (-boot argument)
    qemu-system-i386 -machine pc -accel tcg -cpu "$CPU" -m "$MEM" \
        -vga cirrus -nic none -rtc base=localtime -display none \
        -drive file="$OUT",format=raw,if=ide,index=0 \
        -drive file="$SRC",format=raw,if=ide,index=1 \
        -drive file="$FD",format=raw,if=floppy,index=0 \
        -boot "$1" -monitor unix:"$MON",server,nowait &
    QPID=$!
}
qemu_start once=a,order=c

vm() { python3 "$HERE/vmctl.py" "$@"; }
start=$(date +%s)
n=0
seen=0
still=0
nudged=0
while :; do
    sleep 60
    kill -0 "$QPID" 2>/dev/null || { echo "ERROR: QEMU exited during the install" >&2; exit 1; }
    n=$((n + 1))
    [[ -n $SHOTS ]] && vm shot "$MON" "$SHOTS/$(printf '%03d' "$n").png" >/dev/null
    if frac=$(vm desktop "$MON"); then
        seen=$((seen + 1))
        echo "$(date +%T) poll $n: desktop ($frac), $seen in a row"
        # Three polls a minute apart: Setup's last reboot passes through the
        # desktop's colour for a moment before its final RunOnce items.
        [[ $seen -ge 3 ]] && break
    else
        seen=0
        echo "$(date +%T) poll $n: installing ($frac)"
        if vm still "$MON" "$WORK/still.rgb" >/dev/null; then
            still=$((still + 1))
        else
            still=0
            nudged=0
        fi
        if [[ $still -ge $STILL && $nudged -ge 2 ]]; then
            # Two nudges changed nothing: the guest has hung, not stopped to
            # ask. Seen here once, on the boot after Setup's last restart,
            # frozen behind the startup logo; a warm reset hung the same way
            # and a cold start of a new QEMU booted to the desktop. Setup is
            # finished with the floppy by then, so the restart boots C:.
            echo "$(date +%T) poll $n: guest hung, restarting QEMU from the disk"
            vm cmd "$MON" quit >/dev/null 2>&1 || true
            wait "$QPID" 2>/dev/null || true
            rm -f "$MON" "$WORK/still.rgb"
            qemu_start order=c
            still=0
            nudged=0
        elif [[ -n $NUDGE && $still -ge $STILL ]]; then
            echo "$(date +%T) poll $n: screen unchanged for $still polls, sending: $NUDGE"
            # shellcheck disable=SC2086 # one sendkey name per word
            vm keys "$MON" $NUDGE
            still=0
            nudged=$((nudged + 1))
        fi
    fi
    if (( $(date +%s) - start > TIMEOUT_MIN * 60 )); then
        echo "ERROR: no desktop after $TIMEOUT_MIN minutes" >&2
        exit 1
    fi
done

# Shut down from the Start menu so the disk is clean (no ScanDisk on the next
# boot): Escape clears a Welcome window's focus, Ctrl+Esc opens Start, U is
# Shut Down, and Enter takes the dialog's default, which is Shut down.
vm keys "$MON" esc ctrl-esc
sleep 3
vm keys "$MON" u
sleep 5
vm keys "$MON" ret
# 98 powers the machine off through APM, which ends QEMU; 95 stops at "It's
# now safe to turn off your computer". Either way the disk is clean after
# this long.
for _ in $(seq 18); do
    kill -0 "$QPID" 2>/dev/null || break
    sleep 5
done
if kill -0 "$QPID" 2>/dev/null; then
    [[ -n $SHOTS ]] && vm shot "$MON" "$SHOTS/shutdown.png" >/dev/null
    vm cmd "$MON" quit >/dev/null 2>&1 || true
fi
wait "$QPID" 2>/dev/null || true
QPID=

# Setup keeps a copy of the batch file, key and all, as C:\WINDOWS\MSBATCH.INF.
mdel -i "$OUT@@$((63 * 512))" ::/WINDOWS/MSBATCH.INF 2>/dev/null &&
    echo "deleted C:\WINDOWS\MSBATCH.INF" || true
echo "wrote $OUT"
