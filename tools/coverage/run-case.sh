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
#   media    on a direct boot the loader prints "starting <entry>" as it
#            hands over to the kernel. What the guest reads from the stick
#            after that line is the OS reading its media: an initramfs that
#            never found the ISO reads next to nothing, a live system that
#            mounted its root from it reads hundreds of megabytes. Past
#            MEDIA_MIB the case passes. This is what judges a live system
#            that neither prints to serial nor brings up a network
#            (Clonezilla stops at its own menu).
#
#   setup    Windows Setup has neither serial nor a network that says
#            anything about its media, and its first screen comes up with or
#            without an install source. So once the screen is Setup's, the
#            harness opens its console (Shift+F10) and copies
#            \sources\install.* from every drive letter Windows might have
#            given the ISO to nul. That reads the stick only if Windows
#            itself has the ISO attached, which is exactly what Setup needs;
#            past MEDIA_MIB the case passes.
#
#   screen   a screenshot is always taken at the end, for a human reading a
#            FAIL. It never decides a verdict.
#
# Verdicts: PASS, FAIL, REFUSED (the platform refused an image under Secure
# Boot; whether that is by design is the scorecard's call, not this script's),
# and ERROR for a harness problem (QEMU would not start, the menu never came).
#
# Usage: tools/coverage/run-case.sh STICK STATE ROUTE OUTDIR [TIMEOUT]
#   STATE    off | ms | own | bios | bios32   (tools/coverage/states.sh)
#   ROUTE    dnd      Enter on the image row: whatever the loader does by
#                     default with an unblessed image
#            chain    the chainload key: the ISO's own EFI loader (M2), or
#                     under bios its own BIOS loader through the resident
#                     INT 13h handler
#            blessed  Enter on a blessed image row
#   OUTDIR   receives serial.log, net.pcap, screen.png and result
#   TIMEOUT  seconds from the keypress to a verdict (default 300)
#
# The result file is one line: VERDICT<TAB>REASON<TAB>SECONDS.
# Env: FALCON_CASE_MEM, FALCON_CASE_CPUS, FALCON_CASE_SHOTS and
# FALCON_CASE_LINGER, each described where it is read.

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

# Every exit writes a verdict, including the ones nobody planned.
#
# This script runs under `set -e`, so any command that fails on its way to
# the verdict took it out with no result file and a stale mon.sock beside
# it. The scorecard then had no cell to render and the corpus showed a
# silent gap - which is what two ESXi cases did, twice, while the loader's
# own serial said it had behaved correctly. A cell reading ERROR with an
# exit status is a defect somebody can chase; a missing cell has to be
# noticed first.
#
# Installed here, the moment RESULT has a name, and deliberately NOT written
# in terms of finish(): the failures worth catching include the ones that
# happen before finish has been defined. finish writes RESULT before it
# exits, so the guard is what keeps this from firing a second verdict over
# a real one.
on_exit() {
    local rc=$?

    trap - EXIT
    [[ -f "$RESULT" ]] && exit "$rc"
    [[ -n "${QPID:-}" ]] && kill "$QPID" 2>/dev/null
    rm -f "$MON" "${VARS:-}" 2>/dev/null
    printf 'ERROR\texited %s before reaching a verdict\t%s\n' "$rc" \
        "$(( $(date +%s) - ${T0:-$(date +%s)} ))" > "$RESULT"
    exit "$rc"
}
trap on_exit EXIT

state_firmware "$STATE" "$VARS"   # sets STATE_CODE/SECURE/BIOS, copies vars

# The Secure Boot builds of OVMF keep their variable store behind SMM, and
# refuse to run without it; the plain build has no such requirement.
MACHINE=(-machine q35)
if [[ "$STATE_SECURE" -eq 1 ]]; then
    MACHINE=(-machine q35,smm=on
             -global driver=cfi.pflash01,property=secure,value=on)
fi

# What the guest boots from. Under OVMF that is the firmware in pflash and the
# stick as the only disk; under SeaBIOS there is no pflash at all, and
# falcon-bios comes off a second disk that is first in the boot order
# (states.sh says why the stick is not that disk). i440fx rather than q35,
# because q35 is a UEFI-era chipset and SeaBIOS on it is a combination nobody
# ships.
FIRMWARE=(-drive if=pflash,format=raw,readonly=on,file="$STATE_CODE"
          -drive if=pflash,format=raw,file="$VARS")
DRIVES=(-drive if=none,id=stick,format=raw,file="$STICK",snapshot=on
        -device ahci,id=ahci -device ide-hd,drive=stick,bus=ahci.0)
if [[ "$STATE_BIOS" -eq 1 ]]; then
    MACHINE=(-machine pc)
    FIRMWARE=()
    # The stick keeps its id: stick_read finds it by that name, and the
    # Windows Setup probe judges by what is read from it.
    DRIVES=(-drive "format=raw,file=build/test-bios.img,snapshot=on"
            -drive "id=stick,format=raw,file=$STICK,snapshot=on")
fi

QPID=
finish() {   # finish VERDICT REASON
    local secs=$(( $(date +%s) - T0 ))

    local what=""

    # FALCON_CASE_LINGER=N keeps a passing guest running N more seconds
    # before the final screenshot, so the screen shows what the verdict
    # stood on (a desktop, a login prompt) rather than the moment of it.
    if [[ "$1" == PASS && "${FALCON_CASE_LINGER:-0}" -gt 0 ]] && alive; then
        sleep "$FALCON_CASE_LINGER"
    fi
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
# near-uniform background: deep purple (#1A0050) in Windows 11 24H2 and 25H2,
# navy (#001940) in Server 2025.
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
for sr, sg, sb in ((0x1A, 0x00, 0x50), (0x00, 0x19, 0x40)):
    if all(abs(r - sr) < 24 and abs(g - sg) < 24 and abs(b - sb) < 32
           for r, g, b in edge):
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
    -enable-kvm -cpu "$STATE_CPU" -m "$MEM" -smp "$CPUS" \
    "${FIRMWARE[@]}" \
    "${DRIVES[@]}" \
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
    # The menu's key legend, which both loaders draw from one layout
    # (falcon-core/src/tui).
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

# Move to the image row before pressing anything.
#
# The stick carries one image, but not always on the first row: a blessed
# stick also has a /falcon directory, and whether that sorts above or below
# the image is the alphabet's business (it sits above FreeBSD-15.1 and below
# archlinux-2026.01.01). The selected row is the one the menu draws in
# reverse video, so it can be read off the raw serial and the cursor walked
# down until it is the image.
selected_row() {
    [[ -f "$SERIAL" ]] || return 0
    # The last reverse-video run, up to the escape that ends it.
    # Everything between the escape that turns reverse video on and the next
    # escape of any kind, which is the one that turns it off again.
    printf '%s' "$(sed -e 's/\x1b\[30m\x1b\[47m/\n@ROW@/g' "$SERIAL" |
                   grep '@ROW@' | tail -n 1 |
                   sed -e 's/.*@ROW@//' -e 's/\x1b\[[0-9;=?]*[A-Za-z]//' \
                       -e 's/\x1b.*//' -e 's/^ *//' -e 's/ *$//')"
}
# Both loaders: falcon-bios sends its menu to serial as the same ANSI
# sequences OVMF's terminal does (falcon-bios/src/screen.c).
for ((i = 0; i < 8; i++)); do
    row=$(selected_row)
    [[ "$row" == *.iso* || "$row" == *.img* ]] && break
    echo "sendkey down" | socat - UNIX-CONNECT:"$MON" >/dev/null
    sleep 1
done

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
# Bytes read from the stick so far, from the monitor (0 when it cannot say).
MEDIA_MIB=64
# Both of QEMU's layouts: 10.x prints "stick: rd_bytes=N ..." on one line,
# 11.x a table under "stick:" whose Read: row starts with the bytes. The
# harness container ships the first and a workstation may have the second.
stick_read() {
    echo "info blockstats" | socat - UNIX-CONNECT:"$MON" 2>/dev/null |
        tr -d '\r' |
        awk '/^stick:/ { if (match($0, /rd_bytes=[0-9]+/)) {
                             print substr($0, RSTART + 9, RLENGTH - 9); exit }
                         f = 1; next }
             f && /^Read:/ { print $2; exit }'
}
media_base=
media_what="its media after the kernel started"

# Types a line into the guest through the monitor, then Enter. Only the
# characters the Setup probe uses are mapped.
type_line() {   # type_line TEXT
    local s=$1 c k i
    for (( i = 0; i < ${#s}; i++ )); do
        c=${s:i:1}
        case "$c" in
            [a-z0-9]) k=$c ;;
            [A-Z]) k="shift-${c,,}" ;;
            ' ') k=spc ;;  '.') k=dot ;;  '\') k=backslash ;;
            ':') k=shift-semicolon ;;  '*') k=shift-8 ;;
            '%') k=shift-5 ;;  '(') k=shift-9 ;;  ')') k=shift-0 ;;
            '/') k=slash ;;
            *) continue ;;
        esac
        echo "sendkey $k" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
    done
    echo "sendkey ret" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
}
# The Setup probe (the "setup" signal above): looked for every SETUP_EVERY
# seconds once the payload has the machine, run once.
SETUP_EVERY=10
next_setup=$(( T1 + 30 ))
setup_probe=

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

    # The loader patching boot.wim in memory takes a moment before it hands
    # over, so the fast nudges for cdboot's prompt restart when it does.
    if [[ -z "${patched:-}" ]] &&
       grep -q 'boot.wim patched in memory' <<<"$after"; then
        patched=1
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
    # A BSD installer asking a question is the same evidence a login prompt
    # is: userland, running off the root it mounted. It is worth its own
    # line because these installers never bring up the network by
    # themselves, so the DHCP signal below never fires for them, and pfSense
    # puts the installer on the serial console while the VGA one shows an
    # error about the daemon it cannot reach.
    if grep -q -E 'Console type \[|Welcome to pfSense!|Welcome to OPNsense' \
            <<<"$after"; then
        finish PASS "the BSD installer is up on serial"
    fi
    if grep -q -E 'Kernel panic - not syncing' <<<"$after"; then
        finish FAIL "$(grep -m1 'Kernel panic' <<<"$after" | cut -c1-120)"
    fi
    # The reason quotes the line that matched, not the first line with a
    # similar word in it ("unpack rootfs image as initramfs" is in every
    # boot).
    gave_up='emergency shell|Entering emergency mode|You are in emergency mode|dracut-initqueue.*timeout|\(initramfs\)|Cannot find root'
    if grep -q -E "$gave_up" <<<"$after"; then
        finish FAIL "initramfs gave up: $(grep -m1 -E "$gave_up" <<<"$after" | sed 's/^[[:space:]]*//' | cut -c1-100)"
    fi
    if [[ -z "$setup_probe" && -z "$media_base" ]] && (( now >= next_setup )); then
        next_setup=$(( now + SETUP_EVERY ))
        MON_SOCK="$MON" falcon-efi/tools/rig-shot.sh setup "$OUTDIR" \
            >/dev/null 2>&1 || true
        if [[ -f "$OUTDIR/setup.ppm" &&
              "$(screen_kind "$OUTDIR/setup.ppm")" == 'Windows Setup' ]]; then
            setup_probe=1
            echo "sendkey shift-f10" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true
            sleep 5
            media_base=$(stick_read)
            media_base=${media_base:-0}
            media_what="install.* through the ISO attached in Windows"
            type_line 'for %d in (D E F G H I) do copy /b %d:\sources\install.* nul'
        fi
        rm -f "$OUTDIR"/setup.ppm "$OUTDIR"/setup.png
    fi
    if [[ -z "$media_base" ]] && grep -q '^ *starting ' <<<"$after"; then
        media_base=$(stick_read)
        media_base=${media_base:-0}
    fi
    if [[ -n "$media_base" ]]; then
        media_now=$(stick_read)
        if [[ -n "$media_now" ]] &&
           (( (media_now - media_base) / 1048576 >= MEDIA_MIB )); then
            finish PASS "read $(( (media_now - media_base) / 1048576 )) MiB of $media_what"
        fi
    fi
    if [[ -s "$PCAP" ]] &&
       tcpdump -nr "$PCAP" 'udp and src port 68 and dst port 67' 2>/dev/null |
           grep -q .; then
        finish PASS "dhcp from the guest"
    fi
    sleep 1
done
# The loader prints "starting <entry>" as it hands over, so a timeout after
# one is a different thing from a timeout before it: the payload was
# launched and then said nothing this harness can read.
#
# That is usually not a defect. The harness reads serial and DHCP; a
# blessed wrap carries console=ttyS0 because mk-case-stick.sh puts it there,
# and falcon-bios appends it to a direct boot itself, but a direct or chained
# boot under UEFI runs the ISO's OWN cmdline and many of them write to the
# screen alone. Clonezilla's default entry is "Clonezilla live (VGA
# 800x600)": measured 2026-09-20, its screenshot shows ocs-scan-disk running
# and listing the disk it found, while serial stayed empty for 300s. The
# reason says which entry, so the next reader does not have to rediscover
# that.
# `|| true`: no such line is the common case under pipefail, and set -e
# would turn it into an ERROR instead of the FAIL below.
started=$(grep -m1 '^ *starting ' <<<"$(serial_text | tail -n +"$((MENU_LINES + 1))")" |
          sed 's/^[[:space:]]*starting //' | cut -c1-80 || true)
if [[ -n "$started" ]]; then
    finish FAIL "no userland on serial within ${TIMEOUT}s after starting $started"
fi
finish FAIL "no userland within ${TIMEOUT}s"
