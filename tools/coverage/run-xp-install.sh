#!/usr/bin/env bash
# falcon-boot: install Windows XP from a case stick, unattended, and judge it.
#
# XP's text-mode Setup prints nothing to serial and has no network, and the
# case is not over until Windows stands on its own disk, so this drives the
# whole install instead of run-case.sh's single boot. WINNT.SIF comes on a
# floppy, where Setup looks for it, which leaves the ISO and the stick as a
# person would have them.
#
#   text    the stick is first in the boot order, the blank target second.
#           Text-mode Setup formats the target, copies and restarts; QEMU's
#           -no-reboot ends the run there.
#   gui     the target boots, the stick stays attached for the rest of the
#           files. Setup's GuiRunOnce reports over COM1 at the first logon,
#           then powers off.
#   nostick the target boots with the stick removed and its controller left in
#           place. A Run key from the gui phase reports at logon.
#
# A verdict also needs Windows on C:, falcon-vdisk disabled (Start 4) and Setup
# finished, all read from the report (DESIGN Section 6.3).
#
# The answer file leaves the partition and format screens to the rig, which
# presses Enter as a person would: Repartition=Yes put Windows on D:, behind the
# stick's partition.
#
# With no product key, Setup would stop at the key page of its GUI phase, so
# the case ends after the text phase and its reason says so.
#
# Usage: tools/coverage/run-xp-install.sh STICK STATE OUTDIR [TIMEOUT]
#   STATE    bios | bios32 | bios32-sse2 (tools/coverage/states.sh)
#   OUTDIR   receives serial-PHASE.log, screen.png and result
#   TIMEOUT  seconds allowed for each phase (default 2400)
#
# The result file is one line: VERDICT<TAB>REASON<TAB>SECONDS, as run-case.sh.
# Env: FALCON_CASE_XP_KEY    the product key. Never written under OUTDIR.
#      FALCON_CASE_XP_BUS    how the stick attaches: ide (default) or ehci
#      FALCON_CASE_XP_STALL  seconds of an unchanged screen that end a phase
#                            (default 300)
#      FALCON_CASE_DISPLAY   QEMU's -display (default none)

set -euo pipefail

cd "$(dirname "$0")/../.."
. tools/coverage/states.sh

[[ $# -ge 3 ]] || { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
STICK=$(readlink -f "$1")
STATE=$2
OUTDIR=$3
TIMEOUT=${4:-2400}
# Setup takes a key only upper case and in its five groups, and asks for it on
# screen otherwise.
KEY=$(tr -d '-' <<<"${FALCON_CASE_XP_KEY:-}" | tr '[:lower:]' '[:upper:]' |
      sed -E 's/^(.{5})(.{5})(.{5})(.{5})(.{5})$/\1-\2-\3-\4-\5/')
BUS=${FALCON_CASE_XP_BUS:-ide}
STALL=${FALCON_CASE_XP_STALL:-300}
DISPLAY_ARG=${FALCON_CASE_DISPLAY:-none}

mkdir -p "$OUTDIR"
OUTDIR=$(readlink -f "$OUTDIR")
RESULT="$OUTDIR/result"
rm -f "$RESULT" "$OUTDIR"/serial-*.log "$OUTDIR/screen.png"
# Outside OUTDIR, which CI keeps as an artifact: the floppy holds the key. The
# monitor's socket path is limited to 108 bytes as well (run-case.sh).
WORK=$(mktemp -d "${TMPDIR:-/tmp}/falcon-xp.XXXXXX")
MON="$WORK/mon.sock"
FLOPPY="$WORK/floppy.img"
TARGET="$OUTDIR/target.qcow2"
QPID=
T0=$(date +%s)

cleanup() {
    [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null
    rm -f "$FLOPPY" "$MON" "$TARGET" "$OUTDIR/vars.fd"
    rmdir "$WORK" 2>/dev/null
    return 0
}
on_exit() {
    local rc=$?

    trap - EXIT
    cleanup
    [[ -f "$RESULT" ]] && exit "$rc"
    printf 'ERROR\texited %s before reaching a verdict\t%s\n' "$rc" \
        "$(( $(date +%s) - T0 ))" > "$RESULT"
    exit "$rc"
}
trap on_exit EXIT
trap 'finish ERROR "interrupted"' INT TERM

finish() {   # finish VERDICT REASON
    if [[ -S "$MON" ]]; then
        MON_SOCK="$MON" falcon-efi/tools/rig-shot.sh screen "$OUTDIR" \
            >/dev/null 2>&1 || true
        rm -f "$OUTDIR/screen.ppm"
    fi
    printf '%s\t%s\t%s\n' "$1" "$2" "$(( $(date +%s) - T0 ))" | tee "$RESULT"
    exit 0
}

state_firmware "$STATE" "$OUTDIR/vars.fd"
[[ "$STATE_BIOS" -eq 1 ]] || finish ERROR "XP installs on a BIOS state, not $STATE"

# ---- the answer file ----
# Cirrus, in boot() below: XP has a driver for it. On -vga std the first logon
# stopped at a Display Settings dialog with [Display] set.
# The report is COM1 and each line carries its own redirect: a GuiRunOnce entry
# is one command. % starts a string substitution in a SIF, so nothing here says
# %SystemRoot%; `set SystemRoot` prints it.
sif() {
    cat <<'EOF'
[Data]
AutoPartition=1
MsDosInitiated="0"
UnattendedInstall="Yes"

[Unattended]
UnattendMode=FullUnattended
OemSkipEula=Yes
OemPreinstall=No
UnattendSwitch=Yes
TargetPath=\WINDOWS
Repartition=No
WaitForReboot=No

[GuiUnattended]
AdminPassword=*
AutoLogon=Yes
AutoLogonCount=9
OEMSkipRegional=1
OemSkipWelcome=1
TimeZone=4

[UserData]
FullName="falcon"
OrgName="falcon"
ComputerName=*
EOF
    printf 'ProductKey="%s"\n' "$KEY"
    cat <<'EOF'

[Display]
BitsPerPel=16
XResolution=800
YResolution=600
Vrefresh=60

[Identification]
JoinWorkgroup=WORKGROUP

[Networking]
InstallDefaultComponents=No

[GuiRunOnce]
"cmd /c echo @echo FALCON-XP-LOGON^>COM1 > C:\falcon.cmd"
"reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run /v falcon /d C:\falcon.cmd /f"
"cmd /c set SystemRoot > COM1"
"cmd /c reg query HKLM\SYSTEM\CurrentControlSet\Services\falconcd /v Start > COM1"
"cmd /c reg query HKLM\SYSTEM\Setup /v SystemSetupInProgress > COM1"
"cmd /c echo FALCON-XP-DESKTOP > COM1"
"shutdown -s -f -t 0"
EOF
}
truncate -s 1440K "$FLOPPY"
mkfs.vfat -n WINNT "$FLOPPY" >/dev/null
sif | sed 's/$/\r/' | mcopy -i "$FLOPPY" - ::WINNT.SIF

qemu-img create -q -f qcow2 "$TARGET" 8G

# ---- one boot ----
# Sets PHASE_END to why the guest is no longer running: exit (QEMU left, by a
# reset under -no-reboot or a power off), or the marker it was told to wait for.
boot() {   # boot PHASE STICKBOOT MARKER [QEMU ARGS...]
    local phase=$1 stickboot=$2 marker=$3 serial="$OUTDIR/serial-$1.log"
    shift 3
    local stick=() started seen= same=0 pressed=0 answered=0 now h

    case "$stickboot:$BUS" in
    none:ehci) stick=(-device usb-ehci,id=ehci) ;;
    none:*)    ;;
    *:ehci)    stick=(-drive "if=none,id=stick,format=raw,file=$STICK,snapshot=on"
                      -device usb-ehci,id=ehci
                      -device "usb-storage,bus=ehci.0,drive=stick,bootindex=$stickboot") ;;
    *)         stick=(-drive "if=none,id=stick,format=raw,file=$STICK,snapshot=on"
                      -device "ide-hd,drive=stick,bus=ide.1,unit=0,bootindex=$stickboot") ;;
    esac
    # Stick first means the target second, and the other way about.
    local tboot=1
    [[ "$stickboot" != 0 ]] && tboot=0

    qemu-system-x86_64 -name "falcon xp $phase" -machine pc -enable-kvm \
        -cpu "$STATE_CPU" -m 1024 -smp 2 \
        -drive "if=none,id=target,format=qcow2,file=$TARGET" \
        -device "ide-hd,drive=target,bus=ide.0,unit=0,bootindex=$tboot" \
        "${stick[@]}" -nic none \
        -drive "if=floppy,format=raw,readonly=on,file=$FLOPPY" \
        -vga cirrus -display "$DISPLAY_ARG" -serial "file:$serial" \
        -monitor "unix:$MON,server,nowait" "$@" \
        >"$OUTDIR/qemu-$phase.log" 2>&1 &
    QPID=$!
    started=$(date +%s)

    while :; do
        if ! kill -0 "$QPID" 2>/dev/null; then
            wait "$QPID" 2>/dev/null || true
            QPID=
            PHASE_END=exit
            return 0
        fi
        now=$(date +%s)
        if [[ -n "$marker" ]] && grep -aq "$marker" "$serial" 2>/dev/null; then
            PHASE_END=$marker
            return 0
        fi
        # Enter on the loader's one image row. Pressed until the route's page
        # says it took, because a key lost to the menu's drain ends the run.
        if [[ "$stickboot" == 0 && "$pressed" -eq 0 ]] &&
           grep -aq 'esc back' "$serial" 2>/dev/null; then
            if grep -aq 'booting:' "$serial"; then
                pressed=1
            else
                sleep 2
                echo 'sendkey ret' | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
            fi
        fi
        if grep -aq 'press any key to return to the menu' "$serial" 2>/dev/null; then
            finish FAIL "$phase: back at the menu: $(sed -e 's/\x1b\[[0-9;=?]*[A-Za-z]//g' "$serial" |
                tr -d '\r' | grep -am1 -E 'failed while|advice' | sed 's/^[[:space:]]*//' | cut -c1-100)"
        fi
        (( now - started < TIMEOUT )) ||
            finish FAIL "$phase: not finished within ${TIMEOUT}s"
        # Whatever a phase stops on, predicted or not, ends up holding the
        # screen still: a STOP, a dialog nobody answers, a black hang.
        echo "screendump $WORK/s.ppm" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
        sleep 5
        h=$(sha256sum "$WORK/s.ppm" 2>/dev/null | cut -c1-16 || true)
        rm -f "$WORK/s.ppm"
        # Seen before, not just equal to the last frame: a blinking cursor
        # alternates between two.
        [[ -n "$h" ]] || continue
        if [[ " $seen " == *" $h "* ]]; then
            same=$(( same + 5 ))
            # The partition and format screens are the questions the answer
            # file leaves open (above), so a still screen in the text phase gets
            # Enter, twice at most. Their selected rows are the target's
            # unpartitioned space and quick NTFS.
            if [[ "$phase" == text && "$same" -ge 20 && "$answered" -lt 2 ]]; then
                answered=$(( answered + 1 ))
                same=0
                echo 'sendkey ret' | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
            fi
            (( same >= STALL )) &&
                finish FAIL "$phase: the screen did not change for ${STALL}s"
        else
            same=0
            seen="$seen $h"
        fi
    done
}

stop_guest() {
    if [[ -n "$QPID" ]]; then
        echo quit | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
        wait "$QPID" 2>/dev/null || true
        QPID=
    fi
}

# ---- text ----
boot text 0 '' -no-reboot
# Setup also restarts when it gives up, so what it left on the target is what
# says it finished: the files come to several hundred MiB.
(( $(stat -c %s "$TARGET") > 100 * 1048576 )) ||
    finish FAIL "text: restarted without installing"
[[ -n "$KEY" ]] ||
    finish PASS "text mode finished and restarted; no product key for the GUI phase"

# ---- gui ----
boot gui 1 ''
report=$(tr -d '\r' < "$OUTDIR/serial-gui.log")
grep -q FALCON-XP-DESKTOP <<<"$report" ||
    finish FAIL "gui: powered off before the first logon reported"
grep -qi 'SystemRoot=C:\\WINDOWS' <<<"$report" ||
    finish FAIL "gui: Windows is not on C: ($(grep -i -m1 SystemRoot= <<<"$report"))"
grep -Eq 'Start[[:space:]]+REG_DWORD[[:space:]]+0x4' <<<"$report" ||
    finish FAIL "gui: falcon-vdisk is not disabled after Setup"
grep -Eq 'SystemSetupInProgress[[:space:]]+REG_DWORD[[:space:]]+0x0' <<<"$report" ||
    finish FAIL "gui: Setup is still in progress at the first logon"

# ---- nostick ----
boot nostick none FALCON-XP-LOGON
[[ "$PHASE_END" == FALCON-XP-LOGON ]] ||
    finish FAIL "nostick: powered off before the logon reported"
MON_SOCK="$MON" falcon-efi/tools/rig-shot.sh screen "$OUTDIR" >/dev/null 2>&1 || true
rm -f "$OUTDIR/screen.ppm"
stop_guest
finish PASS "installed to C:\\WINDOWS and logged on with the stick removed"
