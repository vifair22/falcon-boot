#!/usr/bin/env bash
# falcon-boot: DevLink recovery (docs/DESIGN.md Section 12.5) off the stick,
# on the machine it is for: SeaBIOS, an i586 (-cpu pentium, no CMOV) and
# 32 MB. Each case boots the rig stick (mk-test-stick.sh) with the ISO in its
# library and is judged from both serial ports: COM1 carries falcon-bios's
# lines until the kernel starts and then belongs to the devlink agent, which
# the devlink client must reach (`devlink info`); COM2 is where the serial
# entry puts the kernel console.
#
#   devlink-dir-rescue   Enter on the row: falcon reads isolinux.cfg and
#                        boots its default entry, rescue, with falcon-hook
#   devlink-dir-serial   the same on a copy whose rescue entry names a
#                        kernel that is not there: the direct route boots
#                        the first entry whose files exist (bootcfg's pick;
#                        isolinux's DEFAULT is not consulted) and has no
#                        entry chooser, so this is how it reaches the serial
#                        entry. The kernel log reaches COM2
#   devlink-chn-rescue   c on the row: the ISO's own isolinux off the
#                        virtual CD, its menu.c32 answered with Enter
#   devlink-chn-serial   the same, the serial entry chosen with Down, Enter:
#                        the kernel log reaches COM2
#   devlink-uefi         the same stick under OVMF: the row is drawn
#                        {BIOS ONLY} (DESIGN Section 7.8)
#
# The ISO menu is read from VGA text memory through QEMU's monitor, since
# isolinux says nothing on serial. The ISO is devlink-rescue's classic build;
# the client is the devlink repository's build/bin/devlink (make). Each is
# skipped, with a sentence, when it is not there. Nothing touches a real disk.
#
# Usage: falcon-bios/tools/run-devlink-rig.sh [case...]   (default: all)
# Env:   DEVLINK_ISO     the ISO (default: devlink-rescue's build beside this
#                        repository in ~/git/home)
#        DEVLINK_CLIENT  the devlink client (default: ~/git/home/devlink's
#                        build/bin/devlink); iproute2's devlink is not it
#        KEEP=1          reuse the stick from an earlier run
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. falcon-efi/tools/ovmf.sh

R=build/devlink-rig
ISO=${DEVLINK_ISO:-$HOME/git/home/devlink-rescue/build/classic/images/devlink-recovery.iso}
CLIENT=${DEVLINK_CLIENT:-$HOME/git/home/devlink/build/bin/devlink}
TIMEOUT=240

CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(devlink-dir-rescue devlink-dir-serial devlink-chn-rescue
           devlink-chn-serial devlink-uefi)

if [[ ! -f "$ISO" ]]; then
    for c in "${CASES[@]}"; do
        printf 'SKIP  %-19s %s\n' "$c" "no DevLink ISO at $ISO (DEVLINK_ISO)"
    done
    exit 0
fi
HAVE_CLIENT=1
if [[ ! -x "$CLIENT" ]] || ! "$CLIENT" --version 2>/dev/null | grep . >/dev/null; then
    HAVE_CLIENT=0
fi

mkdir -p "$R"
QPID=
cleanup() { [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null || true; }
trap cleanup EXIT

# ---- the stick ----

# The copy for the direct route's serial case: in isolinux.cfg, the rescue
# entry's kernel renamed in place, the same length, so no ISO9660 structure
# moves and the serial entry is the first one bootcfg can boot. The config is
# not covered by isolinux.bin's boot info table, so nothing else changes.
serial_copy() {
    local label off
    cp "$ISO" "$R/serial.iso"
    label=$(grep -abo -m1 'LABEL rescue' "$R/serial.iso" | cut -d: -f1)
    [[ -n "$label" ]] || { echo "ERROR: $ISO has no 'LABEL rescue'" >&2; exit 1; }
    off=$(dd if="$R/serial.iso" bs=1 skip="$label" count=256 status=none |
          grep -abo -m1 'KERNEL /isolinux/bzimage' | cut -d: -f1)
    [[ -n "$off" ]] || { echo "ERROR: the rescue entry names no kernel" >&2; exit 1; }
    printf 'KERNEL /isolinux/missing' | dd of="$R/serial.iso" bs=1 \
        seek="$(( label + off ))" conv=notrunc status=none
}

build() {
    make --no-print-directory all bios cli payload test-iso >/dev/null
    for f in build/test/fixture-uki.efi build/mok-test/falcon-test-mok.key \
             build/shim/bins/fedora-16.1-shimx64.efi; do
        [[ -f "$f" ]] || { echo "ERROR: $f missing (make test-uki sign)" >&2; exit 1; }
    done
    make --no-print-directory sign >/dev/null
    if [[ "${KEEP:-0}" == 1 && -f "$R/stick.img" ]]; then
        return
    fi
    echo "== building the rig stick"
    serial_copy
    TEST_STICK_IMG=$R/stick.img STICK_ISOS=" " \
        STICK_ISO_FILES="$ISO:devlink-recovery.iso $R/serial.iso:devlink-serial.iso" \
        falcon-efi/tools/mk-test-stick.sh --no-labels >"$R/stick.build" 2>&1
}

# ---- the machine ----

log_text() {    # log_text LOG: serial with CRs and ANSI sequences gone
    tr -d '\r' 2>/dev/null < "$1" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}

mon() { printf '%s\n' "$*" | socat - "UNIX-CONNECT:$R/mon.sock" >/dev/null 2>&1 || true; }

# grep reads to the end rather than -q: under pipefail, a grep that stops at
# the first match can leave the writer a SIGPIPE and the pipeline a failure.
has() { log_text "$R/${2:-com1.log}" | grep -aF -- "$1" >/dev/null; }

wait_for() {    # wait_for PATTERN SECONDS [LOG]
    local i
    for (( i = 0; i < $2 * 10; i++ )); do
        has "$1" "${3:-}" && return 0
        kill -0 "$QPID" 2>/dev/null || { has "$1" "${3:-}"; return; }
        sleep 0.1
    done
    return 1
}

# The screen, as text: VGA text memory through the monitor, a character in
# every other byte. All 32 KB of it, not the first page: Linux's vgacon
# scrolls by moving the display start, so what is on screen after a page of
# kernel log is further up the window.
screen() {
    printf 'xp /16384hx 0xb8000\n' |
        socat -t 3 - "UNIX-CONNECT:$R/mon.sock" 2>/dev/null |
        tr -d '\r' | sed -n 's/^0*b[89a-f][0-9a-f]*: //p' | tr ' ' '\n' |
        sed -n 's/^0x..\(..\)$/\1/p' | xxd -r -p | tr -c '[:print:]' ' '
}

wait_screen() { # wait_screen TEXT SECONDS
    local i
    for (( i = 0; i < $2; i++ )); do
        screen | grep -aF -- "$1" >/dev/null && return 0
        sleep 1
    done
    return 1
}

boot() {        # boot bios|uefi
    local fwargs=() cpu=(-cpu pentium -m 32) code vars
    if [[ $1 == uefi ]]; then
        code=$(ovmf_find CODE plain)
        vars=$(ovmf_find VARS plain)
        cp "$vars" "$R/vars.fd"
        chmod u+w "$R/vars.fd"
        fwargs=(-machine q35
                -drive "if=pflash,format=raw,readonly=on,file=$code"
                -drive "if=pflash,format=raw,file=$R/vars.fd")
        cpu=(-cpu host -m 1024)
    else
        fwargs=(-machine pc)
    fi
    rm -f "$R/com1.log" "$R/com2.log" "$R/mon.sock"
    timeout "$TIMEOUT" qemu-system-x86_64 "${fwargs[@]}" "${cpu[@]}" \
        -enable-kvm -display none -net none \
        -drive "if=none,id=d0,format=raw,file=$R/stick.img" \
        -device ide-hd,drive=d0,bus=ide.0,unit=0,bootindex=0 \
        -chardev "pty,id=com1,logfile=$R/com1.log" -serial chardev:com1 \
        -serial "file:$R/com2.log" \
        -monitor "unix:$R/mon.sock,server,nowait" >"$R/qemu.log" 2>&1 &
    QPID=$!
    local i
    for (( i = 0; i < 50; i++ )); do
        PTY=$(sed -n 's/.*redirected to \(\/dev\/pts\/[0-9]*\).*/\1/p' "$R/qemu.log")
        [[ -n "$PTY" ]] && return 0
        sleep 0.1
    done
    echo "ERROR: QEMU gave COM1 no pty" >&2
    exit 1
}

halt() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
}

# The row NAME is on: the library as stage 3 lists it, in the menu's order,
# under the Personal OS row.
row_of() {
    log_text "$R/com1.log" | sed -n 's/^falcon-bios:     \([^ ]*\.iso\)  [0-9]* bytes$/\1/p' |
        grep -nxF "$1" | cut -d: -f1
}

# The agent, through the client: `devlink info` until it answers. Sets
# AGENT to the agent line.
#
# Only once the agent has said it is serving: on the screen (its status
# screen, or the console), or COM2 when the entry put the console there. A client that knocks while
# the kernel is still starting can leave the agent unable to answer anyone
# after it (seen with console=ttyS1, and with QEMU starting the kernel with
# no falcon at all), which would fail a boot that worked.
agent() {
    local i out
    AGENT=
    # The agent says so two ways: an older one writes "serving on
    # /dev/ttyS0" to the console; one with the status screen (devlink
    # c992cb7 on) draws "serial /dev/ttyS0 at 115200: waiting for a client"
    # on its second row once the port is open, and writes nothing to the
    # console. A moment after it, since the screen is up a few milliseconds
    # before the loop's first read.
    for (( i = 0; i < 120; i++ )); do
        has "serving on /dev/ttyS0" com2.log && break
        screen | grep -aE "serving on /dev/ttyS0|serial /dev/ttyS0 at 115200: waiting for a client" \
            >/dev/null && break
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 1
    done
    (( i < 120 )) || return 1
    sleep 1
    for (( i = 0; i < 24; i++ )); do
        out=$(timeout 10 "$CLIENT" info --line "$PTY" 2>&1) &&
            AGENT=$(printf '%s\n' "$out" | grep -a -m1 '^agent')
        [[ -n "$AGENT" ]] && return 0
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 5
    done
    return 1
}

FAILED=0
result() {      # result CASE OK DETAIL
    if [[ $2 -eq 1 ]]; then
        printf 'PASS  %-19s %s\n' "$1" "$3"
    else
        printf 'FAIL  %-19s %s\n' "$1" "$3"
        FAILED=1
    fi
    log_text "$R/com1.log" > "$R/$1.com1.log" 2>/dev/null || true
    log_text "$R/com2.log" > "$R/$1.com2.log" 2>/dev/null || true
}

# case_bios CASE ROUTE ISO ENTRY: ROUTE is dir or chn, ENTRY rescue or
# serial.
case_bios() {
    local c=$1 route=$2 iso=$3 entry=$4 why='' n i evidence
    if [[ $HAVE_CLIENT -eq 0 ]]; then
        printf 'SKIP  %-19s %s\n' "$c" "no devlink client at $CLIENT (DEVLINK_CLIENT)"
        return
    fi
    boot bios
    wait_for "payload-test.iso" 60 || why="the menu never listed the library"
    has "older than i586" && why="stage 2 refused the processor"
    if [[ -z "$why" ]]; then
        n=$(row_of "$iso")
        [[ -n "$n" ]] || why="$iso is not in the library"
    fi
    if [[ -z "$why" ]]; then
        sleep 1
        for (( i = 0; i < n; i++ )); do
            mon "sendkey down"
            sleep 0.2
        done
        if [[ $route == dir ]]; then
            mon "sendkey ret"
            wait_for "entering the kernel" 60 || why="falcon never entered the kernel"
            has "entry:  $entry" || why="${why:-the direct route did not take the $entry entry}"
        else
            mon "sendkey c"
            wait_screen "devlink-rescue" 60 || why="isolinux's menu never came up"
            if [[ -z "$why" ]]; then
                [[ $entry == serial ]] && { mon "sendkey down"; sleep 0.3; }
                mon "sendkey ret"
            fi
        fi
    fi
    if [[ -z "$why" ]]; then
        agent || why="the agent never answered on COM1"
    fi
    if [[ -z "$why" && $entry == serial ]]; then
        wait_for "Linux version" 10 com2.log || why="no kernel log on COM2"
    fi
    if [[ -z "$why" && $entry == rescue ]]; then
        has "Linux version" com2.log && why="the kernel log reached COM2"
    fi
    halt
    if [[ -z "$why" ]]; then
        evidence="$AGENT"
        [[ $entry == serial ]] &&
            evidence+="; COM2: $(log_text "$R/com2.log" | grep -a -m1 'Linux version' | cut -c1-40)"
        result "$c" 1 "$evidence"
    else
        result "$c" 0 "$why"
    fi
}

case_uefi() {
    local why=
    boot uefi
    wait_for "payload-test.iso" 90 || why="the menu never listed the library"
    sleep 1
    if [[ -z "$why" ]]; then
        log_text "$R/com1.log" |
            grep -aE 'devlink-recovery\.iso +\[no efi\] +\{BIOS ONLY\}' >/dev/null ||
            why="devlink-recovery.iso is not drawn with {BIOS ONLY}"
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "devlink-recovery.iso [no efi] {BIOS ONLY}"
    else
        result "$1" 0 "$why"
    fi
}

build

for c in "${CASES[@]}"; do
    case $c in
    devlink-dir-rescue) case_bios "$c" dir devlink-recovery.iso rescue ;;
    devlink-dir-serial) case_bios "$c" dir devlink-serial.iso serial ;;
    devlink-chn-rescue) case_bios "$c" chn devlink-recovery.iso rescue ;;
    devlink-chn-serial) case_bios "$c" chn devlink-recovery.iso serial ;;
    devlink-uefi)       case_uefi "$c" ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
done
exit "$FAILED"
