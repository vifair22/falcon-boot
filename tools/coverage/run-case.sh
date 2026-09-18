#!/usr/bin/env bash
# falcon-boot: boot one coverage case and judge it (docs/notes/
# coverage-roadmap.md Section 2).
#
# A case is one stick (tools/coverage/mk-case-stick.sh) booted under OVMF in
# one Secure Boot state, with one route taken at the loader's menu. The stick
# carries a single image, so its row is the first and selected one; the route
# is the key pressed on it.
#
# How a case is judged, strongest evidence first:
#
#   serial   the loader draws its menu and its messages on the serial console,
#            so "returned to the menu" and "the platform refused" are read
#            straight off it. A blessed boot also carries console=ttyS0
#            (mk-case-stick.sh), so its kernel and init land here too, and a
#            getty's "login:" prompt is proof of userland. Emergency shells
#            and panics are read here as failures.
#
#   network  the guest has one NIC on QEMU's user network with a packet dump
#            on it. A DHCP request from the guest means the OS brought its
#            network up, which a live system does from its real root and an
#            initramfs stuck without its media does not. It is the one signal
#            that works when the ISO's own loader chose the cmdline and the
#            kernel prints only to the screen. The NIC has no option ROM, so
#            the firmware cannot PXE-boot and fake one.
#
#   screen   a screenshot is always taken at the end, for a human reading a
#            FAIL. It never decides a verdict.
#
# Verdicts: PASS, FAIL, REFUSED (the platform refused an image under Secure
# Boot; whether that is by design is the scorecard's call, not this script's),
# and ERROR for a harness problem (QEMU would not start, the menu never came).
#
# Usage: tools/coverage/run-case.sh STICK STATE ROUTE OUTDIR [TIMEOUT]
#   STATE    off | ms | own   (tools/coverage/states.sh)
#   ROUTE    dnd      Enter on the image row: whatever the loader does by
#                     default with an unblessed image
#            chain    the chainload key: the ISO's own EFI loader (M2)
#            blessed  Enter on a blessed image row
#   OUTDIR   receives serial.log, net.pcap, screen.png and result
#   TIMEOUT  seconds from the keypress to a verdict (default 300)
#
# The result file is one line: VERDICT<TAB>REASON<TAB>SECONDS.

set -euo pipefail

cd "$(dirname "$0")/../.."
. tools/coverage/states.sh

[[ $# -ge 4 ]] || { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
STICK=$1
STATE=$2
ROUTE=$3
OUTDIR=$4
TIMEOUT=${5:-300}

MENU_WAIT=120
MEM=${FALCON_CASE_MEM:-4096}
CPUS=${FALCON_CASE_CPUS:-2}

case "$ROUTE" in
dnd|blessed) KEY=ret ;;
chain)       KEY=c ;;
*) echo "ERROR: unknown route $ROUTE" >&2; exit 1 ;;
esac

mkdir -p "$OUTDIR"
OUTDIR=$(readlink -f "$OUTDIR")
SERIAL="$OUTDIR/serial.log"
PCAP="$OUTDIR/net.pcap"
MON="$OUTDIR/mon.sock"
VARS="$OUTDIR/vars.fd"
RESULT="$OUTDIR/result"
rm -f "$SERIAL" "$PCAP" "$MON" "$RESULT" "$OUTDIR/screen.png"

state_firmware "$STATE" "$VARS"   # sets STATE_CODE/SECURE, copies the vars

# The Secure Boot builds of OVMF keep their variable store behind SMM, and
# refuse to run without it; the plain build has no such requirement.
MACHINE=(-machine q35)
if [[ "$STATE_SECURE" -eq 1 ]]; then
    MACHINE=(-machine q35,smm=on
             -global driver=cfi.pflash01,property=secure,value=on)
fi

QPID=
finish() {   # finish VERDICT REASON
    local secs=$(( $(date +%s) - T0 ))

    local what=""

    if [[ -S "$MON" ]]; then
        MON_SOCK="$MON" falcon-efi/tools/rig-shot.sh screen "$OUTDIR" \
            >/dev/null 2>&1 || true
        [[ -f "$OUTDIR/screen.ppm" ]] &&
            what=$(screen_kind "$OUTDIR/screen.ppm")
        rm -f "$OUTDIR/screen.ppm"
    fi
    if [[ -n "$QPID" ]]; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        QPID=
    fi
    rm -f "$MON" "$VARS"
    local reason=$2
    [[ "$1" != PASS && -n "$what" ]] && reason="$reason (screen: $what)"
    printf '%s\t%s\t%s\n' "$1" "$reason" "$secs" > "$RESULT"
    printf '%s\t%s\t%s\n' "$1" "$reason" "$secs"
    exit 0
}

# What the last frame looks like, in a word or two, for a failure's reason.
# Never a verdict: a screen cannot say whether an OS found its media. It is
# here because the one case with no serial and no network, Windows Setup,
# otherwise reads as a bare timeout. Setup draws its dialogs on a
# near-uniform deep purple (#1A0050 in 24H2 and 25H2).
screen_kind() {   # screen_kind PPM
    python3 - "$1" <<'PY' 2>/dev/null || true
import sys
d = open(sys.argv[1], 'rb').read()
fields, i = [], 2
while len(fields) < 3:
    while d[i:i+1].isspace():
        i += 1
    j = i
    while not d[j:j+1].isspace():
        j += 1
    fields.append(int(d[i:j]))
    i = j
w, h = fields[0], fields[1]
px = d[i + 1:]
def at(x, y):
    o = 3 * (y * w + x)
    return px[o], px[o + 1], px[o + 2]
edge = [at(x, y) for x in (2, w // 2, w - 3) for y in (2, h - 3)]
if all(abs(r - 0x1A) < 24 and g < 24 and abs(b - 0x50) < 32 for r, g, b in edge):
    print('Windows Setup')
    sys.exit()
dark = sum(1 for k in range(0, len(px) - 2, 3 * 97) if max(px[k:k+3]) < 40)
total = len(range(0, len(px) - 2, 3 * 97))
print('text console' if dark > 0.8 * total else 'graphical')
PY
}
trap 'finish ERROR "interrupted"' INT TERM

# QEMU writes its snapshot overlay beside TMPDIR; keep it on the same volume
# as build/ rather than in a RAM-backed /tmp that a 5 GB guest write would
# fill.
export TMPDIR="$OUTDIR"

T0=$(date +%s)
qemu-system-x86_64 \
    -name "falcon-case" \
    "${MACHINE[@]}" \
    -enable-kvm -cpu host -m "$MEM" -smp "$CPUS" \
    -drive if=pflash,format=raw,readonly=on,file="$STATE_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive if=none,id=stick,format=raw,file="$STICK",snapshot=on \
    -device ahci,id=ahci -device ide-hd,drive=stick,bus=ahci.0 \
    -netdev user,id=net0 \
    -device e1000e,netdev=net0,romfile= \
    -object filter-dump,id=dump0,netdev=net0,file="$PCAP" \
    -display none -vga std \
    -serial "file:$SERIAL" \
    -monitor "unix:$MON,server,nowait" \
    >"$OUTDIR/qemu.log" 2>&1 &
QPID=$!

alive() { kill -0 "$QPID" 2>/dev/null; }

# Serial text with the ANSI control sequences the menu draws stripped, so a
# pattern never has to straddle a cursor move.
serial_text() {
    [[ -f "$SERIAL" ]] || return 0
    sed -e 's/\x1b\[[0-9;=?]*[A-Za-z]//g' "$SERIAL" | tr -d '\r' 2>/dev/null
}

# ---- the menu ----
for ((i = 0; i < MENU_WAIT; i++)); do
    alive || finish ERROR "qemu exited before the menu ($(tail -c 200 "$OUTDIR/qemu.log" | tr '\n\t' '  '))"
    if serial_text | grep -q 'esc back'; then
        break
    fi
    sleep 1
done
if (( i >= MENU_WAIT )); then
    finish ERROR "no loader menu within ${MENU_WAIT}s"
fi
# The menu has drawn its footer; give the key queue drain a moment so the
# keypress is not swallowed with the firmware's auto-repeat backlog.
sleep 2
MENU_LINES=$(serial_text | wc -l)
echo "sendkey $KEY" | socat - UNIX-CONNECT:"$MON" >/dev/null
T1=$(date +%s)

# ---- the verdict ----
#
# Enter is pressed again every NUDGE_EVERY seconds for the first NUDGE_FOR:
# an ISO's own boot menu may wait for a key forever (Gentoo's GRUB does), and
# Windows media asks for "any key to boot from CD". Enter takes each menu's
# default entry, which is what a case means by booting the ISO, and it is
# harmless to a kernel that is already running.
# Every 2 s for the first 30: Windows' cdboot.efi gives up on its prompt
# after about five seconds. Every 10 s after that, for slower menus.
NUDGE_EVERY=2
NUDGE_FOR=90
next_nudge=$(( T1 + NUDGE_EVERY ))
# FALCON_CASE_SHOTS=N keeps a screenshot every N seconds (shot-SECS.png), for
# reading a failure the serial log cannot show: a graphical boot menu, a
# kernel that prints only to the screen.
SHOTS=${FALCON_CASE_SHOTS:-0}
next_shot=$T1
while (( $(date +%s) - T1 < TIMEOUT )); do
    alive || finish FAIL "the guest powered off or reset"

    now=$(date +%s)
    if (( now >= next_nudge && now - T1 <= NUDGE_FOR )); then
        echo "sendkey ret" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
        if (( now - T1 < 30 )); then
            next_nudge=$(( now + NUDGE_EVERY ))
        else
            next_nudge=$(( now + 10 ))
        fi
    fi

    if (( SHOTS > 0 && now >= next_shot )); then
        MON_SOCK="$MON" falcon-efi/tools/rig-shot.sh "shot-$(printf '%03d' $((now - T1)))" \
            "$OUTDIR" >/dev/null 2>&1 || true
        rm -f "$OUTDIR"/shot-*.ppm
        next_shot=$(( now + SHOTS ))
    fi

    after=$(serial_text | tail -n +"$((MENU_LINES + 1))")

    # The loader offers to prepare unprepared Windows media before booting
    # it (DESIGN 6.3.1). The drop route means booting what is on the stick,
    # so the answer is the offer's "boot unprepared" key, once.
    if [[ -z "${offered:-}" ]] &&
       grep -q 'boot unprepared' <<<"$after"; then
        echo "sendkey b" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
        offered=1
        # The handover to the payload starts now, not at the menu keypress:
        # restart the fast nudges for cdboot's prompt.
        T1=$now
        next_nudge=$(( now + NUDGE_EVERY ))
    fi

    if grep -q 'press any key to return to the menu' <<<"$after"; then
        why=$(grep -m1 -E 'failed while|exited, status|refused' <<<"$after" |
              sed 's/^[[:space:]]*//' | cut -c1-120)
        if grep -q -E 'refused|0x800000000000001a' <<<"$after"; then
            finish REFUSED "${why:-the platform refused the image}"
        fi
        finish FAIL "back at the menu: ${why:-no reason printed}"
    fi
    if grep -q -E '[a-zA-Z0-9-]+ login: *$|^[a-zA-Z0-9-]+ login:' <<<"$after"; then
        finish PASS "login prompt on serial"
    fi
    if grep -q -E 'Kernel panic - not syncing' <<<"$after"; then
        finish FAIL "$(grep -m1 'Kernel panic' <<<"$after" | cut -c1-120)"
    fi
    if grep -q -E 'emergency shell|Entering emergency mode|You are in emergency mode|dracut-initqueue.*timeout|\(initramfs\)|Cannot find root' <<<"$after"; then
        finish FAIL "initramfs gave up: $(grep -m1 -E 'emergency|timeout|initramfs|Cannot find' <<<"$after" | cut -c1-100)"
    fi
    if [[ -s "$PCAP" ]] &&
       tcpdump -nr "$PCAP" 'udp and src port 68 and dst port 67' 2>/dev/null |
           grep -q .; then
        finish PASS "dhcp from the guest"
    fi
    sleep 1
done
finish FAIL "no userland within ${TIMEOUT}s"
