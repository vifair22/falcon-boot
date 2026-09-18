#!/usr/bin/env bash
# falcon-boot: the signing appliance's acceptance run (DESIGN Section 7.5,
# build order step 8).
#
# Boots build/appliance/appliance-stick.img under OVMF with Secure Boot ON and
# a varstore that trusts the stick's own MOK, drives the loader into the
# appliance, and drives the appliance itself. Two appliance runs in one QEMU
# session, because the second one is what proves the first one was recorded:
#
#   run 1  the select-then-sign flow of DESIGN Section 7.5. Two pending ISOs,
#          one at the p2 root and one in a folder, so the grouped listing has
#          something to group. A junk line first (it must cost one error line,
#          not the session), then image 2 is marked, reviewed and confirmed.
#          -> expect 2 pending, SELECTED 1, 1 signed, 1 skipped
#   run 2  the same stick a second time: the blessed ISO must no longer be
#          pending, and 'quit' at the selection prompt must leave without
#          signing.
#          -> expect 1 pending, no SELECTED marker, 0 signed, 0 failed
#
# With a Windows ISO staged on p2 (tools/mk-test-appliance-stick.sh with
# FALCON_WIN_ISO set), both runs carry a second leg, the auto-prep contract of
# DESIGN Section 6.3.1:
#
#   run 1  the survey routes the Windows image to the prepare queue, offers it
#          at the PREPARE GATE (DESIGN Section 7.7: nothing pre-marked, a
#          review that lists every path and states the cost, a typed "yes"),
#          then prepares it WITH p3 STILL LOCKED and only then unlocks for the
#          bless half. The gate is driven the long way round on purpose - mark,
#          review, decline, review again, confirm - because "anything short of
#          yes cancels" is the half of the gate a happy path never exercises.
#          The ordering is asserted off the finished log, because "preparation
#          needs no key" is the claim that makes an auto-prep boot cost the
#          user no passphrase, and a summary alone would not catch its loss.
#   run 2  the same image is recognised as prepared and skipped by name, so
#          batching does not redo three minutes of work per boot.
#   after  the record, the sidecar and the patched WIM are read back on the
#          host: kind=prepared, a splice covering exactly the byte range the
#          ISO's own directory entry gives for /sources/boot.wim, a sidecar of
#          that exact length, wimverify clean, our winjump.exe in place of
#          winpeshl.exe with Microsoft's preserved beside it, and the ISO's
#          own hash unchanged (preparation opens it read-only).
#
# Neither run types a passphrase on the usual path: the loader hands the
# appliance the one it already collected (DESIGN Section 5.1). The driver
# waits for whichever unlock the run takes, so the interactive fallback is
# still driven if the credential is ever missing.
#
# Then the stick is mounted on the host and checked: exactly one wrap exists,
# it verifies against the stick's certificate, the manifest holds exactly the
# one record, its sha256 is the ISO's real hash, and the signed cmdline names
# p2 by the volume serial that partition actually carries and the ISO by its
# path inside the folder.
#
# Two serial ports, and the reason matters: ttyS0 is the rig's usual
# write-only log (tools/run-qemu.sh --headless), which carries the firmware
# and the loader. The appliance needs a console it can READ from, so this
# script adds a second port as a pair of FIFOs, and ttyS1 is one of the
# consoles the appliance opens (tools/appliance/init.c writes to all of them
# and reads from whichever answers, so which console= came last on the cmdline
# does not decide whether this drive works). The loader menu is still driven
# the rig's usual way, by sendkey through the QEMU monitor.
#
# What this log does NOT carry is the output of the helper tools the appliance
# runs: those inherit /dev/console, which the cmdline points at the screen so
# a real machine shows its own progress. Only the appliance's own lines, the
# ones asserted below, come down the serial port.
#
# Usage: tools/appliance/run-appliance-proof.sh
# Env:   FALCON_TEST_PASS  the stick's passphrase (default "falcon-test")
#
# Which legs run is read off the stick, not out of the environment: if p2
# carries no Windows ISO the Windows assertions become "nothing was queued to
# prepare", which is itself worth pinning.

set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=build/appliance
IMG="$OUT/appliance-stick.img"
CERT="$OUT/appliance-mok.crt"
MOK_VARS="$OUT/OVMF_VARS.secboot.mok.fd"
VARS_RUN="$OUT/vars-run.fd"
SERIAL0="$OUT/loader-serial.log"
CONSOLE="$OUT/console"          # $CONSOLE.in / $CONSOLE.out, the ttyS1 FIFOs
CONSOLE_LOG="$OUT/appliance-console.log"
MON="$OUT/mon.sock"

PASS="${FALCON_TEST_PASS:-falcon-test}"

# What tools/mk-test-appliance-stick.sh staged on p2, and what signing the
# second of them must produce under falcon/blessed.
ROOT_ISO_NAME=arch-mini-root.iso
FOLDER=Linux
FOLDER_ISO_NAME=arch-mini-linux.iso
BLESSED_UKI=arch-mini-linux.uki

# Where mk-test-appliance-stick.sh puts a Windows ISO when it was given one.
# Its presence on p2 is what turns the preparation assertions below on.
WIN_FOLDER=Windows

QEMU_PID=
READER_PID=
LOOP=
MNT=
T_LAUNCH=0

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
        MNT=
    fi
    if [[ -n "$LOOP" ]]; then
        detach_img "$LOOP"
        LOOP=
    fi
    exec 3>&- 2>/dev/null || true
    if [[ -n "$READER_PID" ]]; then
        kill "$READER_PID" 2>/dev/null || true
        READER_PID=
    fi
    if [[ -n "$QEMU_PID" ]]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=
    fi
    rm -f "$CONSOLE.in" "$CONSOLE.out" "$MON"
}
trap cleanup EXIT

for t in qemu-system-x86_64 socat sbverify sha256sum blkid; do
    command -v "$t" >/dev/null 2>&1 || { echo "ERROR: $t not found" >&2; exit 1; }
done
# Only for reading back what preparation wrote, and only when the stick
# carries a Windows ISO to have prepared. Checked up front all the same: the
# alternative is discovering a missing wimdir after a three-minute run.
for t in 7z wimverify wiminfo wimdir wimextract; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "note: $t not found; the Windows leg cannot be checked" >&2
    }
done
[[ -f "$IMG" ]] || {
    echo "ERROR: $IMG missing (run: tools/mk-test-appliance-stick.sh)" >&2
    exit 1
}

# ---- helpers ----

fail() {
    echo
    echo "FAIL: $*" >&2
    echo "  loader log:    $SERIAL0"
    echo "  appliance log: $CONSOLE_LOG"
    exit 1
}

# Waits for a pattern to appear in a log. Everything in this script is a race
# against a booting machine, so nothing is done on a timer: each step waits
# for the evidence that the previous one landed.
wait_for() {
    local file="$1" pat="$2" secs="${3:-120}" i=0

    while (( i < secs * 10 )); do
        if grep -aqF "$pat" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    fail "timed out after ${secs}s waiting for '$pat' in $file"
}

# Same, but only from a byte offset on, so a pattern that has already appeared
# once (the menu banner, a prompt from the previous run) is not mistaken for
# this run's.
wait_for_from() {
    local file="$1" off="$2" pat="$3" secs="${4:-120}" i=0

    while (( i < secs * 10 )); do
        if tail -c "+$(( off + 1 ))" "$file" 2>/dev/null |
               awk -v pat="$pat" 'index($0, pat) { found = 1 }
                                  END { exit found ? 0 : 1 }'; then
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    fail "timed out after ${secs}s waiting for '$pat' in $file"
}

# True when a pattern has appeared in the appliance console since a byte
# offset. The console is a serial line, so every line ends CR LF; -F keeps the
# brackets and slashes in the patterns below literal.
# seen_from for a whole line: true only when a line of the console log, CR
# stripped, IS pat. The appliance's folder headers are lines of their own
# ("  Linux/"), while the loader's menu, which OVMF also mirrors onto this
# port, carries the same folder name inside a longer, escape-laden row; a
# substring match cannot tell the two apart and failed run 2 on the loader's
# screen rather than on anything the appliance printed.
seen_line_from() {
    local off="$1" pat="$2"

    tail -c "+$(( off + 1 ))" "$CONSOLE_LOG" 2>/dev/null |
        awk -v pat="$pat" '{ sub(/\r$/, "") } $0 == pat { found = 1 }
                           END { exit !found }'
}

seen_from() {
    local off="$1" pat="$2"

    # awk rather than `grep -q`: grep stops reading at the first match, tail
    # then dies of SIGPIPE, and under `set -o pipefail` the pipeline reports
    # 141 even though the pattern WAS found. Every reader in this script that
    # would stop early is written this way for that reason.
    tail -c "+$(( off + 1 ))" "$CONSOLE_LOG" 2>/dev/null |
        awk -v pat="$pat" 'index($0, pat) { found = 1 }
                           END { exit found ? 0 : 1 }'
}

# Line number of a pattern's first appearance since a byte offset, or nothing.
# Ordering assertions are made this way rather than by checking that something
# has NOT happened yet: the appliance moves from one step to the next in the
# time it takes to write a line, so a live absence check is a race, while the
# order two lines ended up in is a fact.
line_of() {
    local off="$1" pat="$2"

    # awk over the whole stream rather than `grep | head -1`: head closing the
    # pipe early kills the writer with SIGPIPE, and under `set -o pipefail`
    # that makes the whole substitution fail and the script exit with nothing
    # said. Every pipeline here that stops reading early has the same trap.
    tail -c "+$(( off + 1 ))" "$CONSOLE_LOG" 2>/dev/null |
        awk -v pat="$pat" 'n == 0 && index($0, pat) { n = NR }
                           END { if (n) print n }'
}

# Fails unless `first` appears before `second` in the console since an offset.
before() {
    local off="$1" first="$2" second="$3" a b

    a=$(line_of "$off" "$first")
    b=$(line_of "$off" "$second")
    [[ -n "$a" ]] || fail "'$first' never appeared on the appliance console"
    [[ -n "$b" ]] || fail "'$second' never appeared on the appliance console"
    (( a < b )) || fail "'$first' came after '$second' on the console"
}

# The selection prompt carries no newline, so prompts are counted rather than
# waited for by position: the appliance prints the whole list and the first
# prompt in one breath, and the Nth prompt since a mark is an unambiguous
# thing to synchronise on.
select_count() {
    tail -c "+$(( ${1:-0} + 1 ))" "$CONSOLE_LOG" 2>/dev/null |
        grep -ac 'select>' || true
}

# The same, for the preparation gate. The two gates prompt with different
# words (tools/appliance/init.c, gate_ui), which is what lets a reader of the
# console - and this driver - tell which one is being answered.
prepare_count() {
    tail -c "+$(( ${1:-0} + 1 ))" "$CONSOLE_LOG" 2>/dev/null |
        grep -ac 'prepare>' || true
}

# The loader hands the appliance the passphrase it already collected (DESIGN
# Section 5.1), so the usual path reaches the pending list with no prompt at
# all; the prompt is the fallback for a missing or stale credential. Which one
# a run takes is the loader's business, so it is waited for rather than
# assumed, and the fallback still gets driven when it happens.
unlock_appliance() {
    local off="$1" secs="${2:-180}" i=0

    while (( i < secs * 10 )); do
        if seen_from "$off" "p3 unlocked (read-only)"; then
            echo "   p3 unlocked with the credential the loader delivered"
            return 0
        fi
        if seen_from "$off" "passphrase for p3 (attempt 1 of 3)"; then
            echo "   no usable credential; typing the passphrase"
            say_to_appliance "$PASS"
            wait_for_from "$CONSOLE_LOG" "$off" "p3 unlocked (read-only)" 120
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    fail "timed out after ${secs}s waiting for p3 to unlock"
}

wait_select() {
    local off="$1" want="$2" secs="${3:-120}" i=0

    while (( i < secs * 10 )); do
        if (( $(select_count "$off") >= want )); then
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    fail "timed out after ${secs}s waiting for selection prompt #$want"
}

wait_prepare() {
    local off="$1" want="$2" secs="${3:-120}" i=0

    while (( i < secs * 10 )); do
        if (( $(prepare_count "$off") >= want )); then
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    fail "timed out after ${secs}s waiting for prepare prompt #$want"
}

size_of() { stat -c%s "$1" 2>/dev/null || echo 0; }

mon() { printf '%s\n' "$*" | socat - "UNIX-CONNECT:$MON" >/dev/null; }

# One QEMU sendkey per character. The firmware's console input is a PS/2
# keyboard whether or not a display is attached, which is what lets a headless
# run drive the loader at all.
send_keys() {
    local s="$1" i c key

    for (( i = 0; i < ${#s}; i++ )); do
        c="${s:i:1}"
        case "$c" in
        [a-z0-9]) key="$c" ;;
        -)        key=minus ;;
        .)        key=dot ;;
        *) fail "no sendkey mapping for '$c'; keep rig passphrases to [a-z0-9.-]" ;;
        esac
        mon "sendkey $key"
        sleep 0.05
    done
    mon "sendkey ret"
}

# The appliance's own console is a real bidirectional tty, so its input is
# typed rather than injected.
say_to_appliance() { printf '%s\n' "$1" >&3; }

# ---- the loader leg ----
#
# Menu -> Personal OS -> passphrase -> the UKI chooser -> the appliance.
enter_appliance() {
    local mark entries idx i

    mark=$(size_of "$SERIAL0")
    wait_for_from "$SERIAL0" "$mark" "Personal OS (encrypted)" 120
    sleep 1
    mon "sendkey ret"

    wait_for_from "$SERIAL0" "$mark" "passphrase for the encrypted partition" 60
    sleep 1
    send_keys "$PASS"

    # One UKI boots without a chooser; two or more are offered. Rows are
    # labelled from each image's .osrel (src/pos/osrel.c), so the appliance
    # is found by the name it gives itself, not by its file name, and the
    # chooser puts it LAST (src/pos/pos.c, uki_before), which is asserted
    # here: a stick whose chooser opens on the appliance boots it on Enter.
    if wait_for_from "$SERIAL0" "$mark" "Personal OS: " 60; then
        sleep 1
        entries=$(tail -c "+$(( mark + 1 ))" "$SERIAL0" |
                  grep -ao 'Personal OS: [A-Za-z0-9_.() -]*[A-Za-z0-9_.)]' |
                  awk '!seen[$0]++')
        idx=$(printf '%s\n' "$entries" | grep -n 'signing appliance' | cut -d: -f1)
        [[ -n "$idx" ]] || fail "the appliance is not in the loader's UKI chooser"
        [[ "$idx" -eq "$(printf '%s\n' "$entries" | wc -l)" ]] ||
            fail "the appliance is row $idx, not the last row of the chooser"
        echo "   chooser: the appliance is entry $idx of $(printf '%s\n' "$entries" | wc -l) (last)"
        for (( i = 1; i < idx; i++ )); do
            mon "sendkey down"
            sleep 0.1
        done
        mon "sendkey ret"
    fi
    T_LAUNCH=$(date +%s%N)
}

# Attaches $IMG and prints the loop device, with the block layer's cached
# view of it invalidated first.
#
# This is not defensive habit, it is a measured hazard. The loop device keeps
# a buffer cache of its own, and a loop number recycled from this script's own
# pre-flight attach can still hold blocks read BEFORE the guest ran: the
# read-back then shows an empty falcon/ directory on a stick the appliance
# demonstrably wrote to. BLKFLSBUF on the device and on the partition drops
# exactly that. (`losetup --direct-io=on` would sidestep the cache entirely
# and was tried first: it sets a 4096-byte block size and skips the partition
# scan, so the partition nodes never appear at all.)
attach_img() {
    local l i j

    sync
    # Retried as a whole, not just waited on. -P asks for a partition scan,
    # but a loop number freed moments ago by another script may still be
    # tearing down when it is handed back: the nodes then never appear, or
    # appear and vanish under the mount as "Can't lookup blockdev". Taking a
    # different loop device is the cure, so a failed attempt is released and
    # the next one gets a fresh number.
    for j in 1 2 3 4 5; do
        l=$(sudo losetup --show -f -P "$IMG")
        for i in $(seq 1 20); do
            if [[ -b "${l}p2" ]]; then
                break
            fi
            sudo udevadm settle 2>/dev/null || true
            sleep 0.25
        done
        if [[ -b "${l}p2" ]]; then
            sudo blockdev --flushbufs "$l" 2>/dev/null || true
            sudo blockdev --flushbufs "${l}p2" 2>/dev/null || true
            printf '%s\n' "$l"
            return 0
        fi
        echo "note: $l came up without partition nodes; trying another" >&2
        detach_img "$l"
        sleep 1
    done
    fail "$IMG would not attach with its partition nodes"
}

# Detaches a loop device and waits until it is really gone. `losetup -d` is
# deferred while anything still holds the device, and it reports success
# either way; a loop left over this image is exactly what makes the next
# attach of it serve a stale view.
detach_img() {
    local l="$1" i

    [[ -n "$l" ]] || return 0
    for i in 1 2 3 4 5 6 7 8 9 10; do
        sudo losetup -d "$l" 2>/dev/null || true
        losetup -a 2>/dev/null | grep -q "^$l:" || return 0
        sleep 0.5
    done
    echo "note: $l would not detach" >&2
}

# ---- start ----

# Run 1 exists to turn an unblessed stick into a blessed one, so a stick that
# has already been through it cannot prove anything. Checked here rather than
# discovered three minutes in as a confusing count mismatch.
LOOP=$(attach_img)
sudo udevadm settle 2>/dev/null || sleep 1
MNT=$(mktemp -d)
sudo mount -o ro "${LOOP}p2" "$MNT"
if [[ -f "$MNT/falcon/blessed/manifest" ]]; then
    sudo umount "$MNT"; rmdir "$MNT"; MNT=
    detach_img "$LOOP"; LOOP=
    echo "ERROR: $IMG already carries a manifest, so nothing on it is pending." >&2
    echo "       Rebuild it first: tools/mk-test-appliance-stick.sh" >&2
    exit 1
fi
# Whether this stick carries the Windows leg is read off the stick rather than
# taken from the environment, so the proof asserts what is actually there.
WIN_REL=$(cd "$MNT" && find "$WIN_FOLDER" -maxdepth 1 -type f -iname '*.iso' \
    2>/dev/null | head -1 || true)
WIN_SIZE=0
[[ -n "$WIN_REL" ]] && WIN_SIZE=$(stat -c%s "$MNT/$WIN_REL")
sudo umount "$MNT"; rmdir "$MNT"; MNT=
detach_img "$LOOP"; LOOP=

if [[ -n "$WIN_REL" ]]; then
    echo "== Windows leg ON: /$WIN_REL ($WIN_SIZE bytes) is staged on p2"
else
    echo "== Windows leg OFF: no ISO under /$WIN_FOLDER on p2."
    echo "   Rebuild with FALCON_WIN_ISO=<path to a Windows install ISO> to"
    echo "   prove preparation; without one this run proves only that the"
    echo "   appliance surveys the library and finds nothing to prepare."
fi

echo "== booting $IMG (Secure Boot ON, MokList = this stick's own cert)"
rm -f "$VARS_RUN" "$SERIAL0" "$CONSOLE_LOG" "$CONSOLE.in" "$CONSOLE.out" "$MON"
mkfifo "$CONSOLE.in" "$CONSOLE.out"

# 2 GiB because the initramfs is ~77 MiB uncompressed and the signing run
# keeps the extracted kernel and the unsigned wrap in a tmpfs on purpose.
STICK_IMG="$IMG" MOK_VARS="$MOK_VARS" VARS_FILE="$VARS_RUN" \
SERIAL_LOG="$SERIAL0" MON_SOCK="$MON" QEMU_MEM=2048 \
    tools/run-qemu.sh --stick --secboot --headless \
        -serial "pipe:$CONSOLE" &
QEMU_PID=$!

# The reader has to be running before the guest writes anything, or the FIFO
# backs up; the writer end is held open for the whole session so the guest
# never sees EOF on its console.
( cat "$CONSOLE.out" > "$CONSOLE_LOG" ) &
READER_PID=$!
exec 3> "$CONSOLE.in"

# ---- run 1: mark one image, review it, sign it ----

echo
echo "== run 1: drive the loader into the appliance"
RUN1_MARK=$(size_of "$CONSOLE_LOG")
enter_appliance

wait_for "$CONSOLE_LOG" "falcon-boot signing appliance" 180
T_UP=$(date +%s%N)
echo "   appliance is up"
echo "   TIMING boot-to-console $(( (T_UP - T_LAUNCH) / 1000000 )) ms" \
     "(from the menu selection, so it includes the loader reading the image)"

# ---- the preparation half, which runs before p3 is touched ----
#
# Ordering is the assertion here as much as the outcome is. Preparation needs
# no key (DESIGN Section 6.3), so the appliance does it with p3 still locked
# and only unlocks afterwards if the bless half has work; that is what makes
# an auto-prep boot cost no passphrase. Waiting for the batch BEFORE
# unlock_appliance is therefore not just a timeout convenience, it is the
# order this proof expects to see.
wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "surveying p2..." 120
wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "survey: " 120
SURVEY1=$(tail -c "+$(( RUN1_MARK + 1 ))" "$CONSOLE_LOG" |
          grep -a "^survey: " | tail -1 | sed 's/\r$//' || true)
echo "   $SURVEY1"

if [[ -n "$WIN_REL" ]]; then
    [[ "$SURVEY1" == *"1 to prepare"* ]] ||
        fail "the Windows ISO on p2 was not routed to the prepare queue: '$SURVEY1'"
    wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "FALCON-APPLIANCE-PREP-PENDING 1" 120
    seen_from "$RUN1_MARK" "p3 stays locked for it" ||
        fail "the appliance did not say preparation leaves p3 locked"
    # This stick carries both kinds of work, so the run must say so before the
    # first gate: two operations asked about separately is confusing unless
    # something names them.
    seen_from "$RUN1_MARK" "Two kinds of work are waiting" ||
        fail "a mixed run did not announce its two kinds of work"

    # ---- the prepare gate (DESIGN Section 7.7) ----
    #
    # Preparation is a bless operation and passes the same gate as signing, so
    # nothing may start before a marked set has been reviewed and confirmed.
    # Every clause of that is asserted here, because the failure this replaces
    # was an appliance that hashed two multi-gigabyte ISOs having asked
    # nothing: an unanswered gate would otherwise look exactly like a fast rig.
    WIN_BASE=$(basename "$WIN_REL")
    wait_prepare "$RUN1_MARK" 1 120
    seen_from "$RUN1_MARK" "Windows images that are not prepared yet:" ||
        fail "the appliance did not print the prepare list"
    seen_from "$RUN1_MARK" "[ ]  1  $WIN_BASE" ||
        fail "the Windows image is not offered as unmarked image 1"
    seen_from "$RUN1_MARK" "FALCON-APPLIANCE-PREP-SELECTED" &&
        fail "a preparation batch was released before the gate was answered"
    echo "   prepare gate is up with nothing pre-marked"

    echo "   marking image 1 ($WIN_REL)"
    say_to_appliance "1"
    wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "[x]  1  $WIN_BASE" 60
    wait_prepare "$RUN1_MARK" 2 60
    say_to_appliance "done"
    # The review has to state the cost BEFORE it is incurred: the path in
    # full, the image's size, the batch total, and that it will take a while.
    wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "GiB to hash and repack" 60
    seen_from "$RUN1_MARK" "  /$WIN_REL" ||
        fail "the prepare review did not print the image's full path"
    seen_from "$RUN1_MARK" "take a while" ||
        fail "the prepare review did not say the batch takes a while"
    echo "   review:"
    tail -c "+$(( RUN1_MARK + 1 ))" "$CONSOLE_LOG" |
        awk '/^review: / { f = 1 } f { print }' | sed 's/\r$//; s/^/     /'

    # Anything short of "yes" cancels, and must cost the marks nothing.
    echo "   declining the review once"
    say_to_appliance "no"
    wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "not confirmed; back to the list" 60
    seen_from "$RUN1_MARK" "FALCON-APPLIANCE-PREP-SELECTED" &&
        fail "a declined review still released the preparation batch"

    # The marks survived the decline, so the same "done" reaches the same
    # review. Both lines are sent without waiting in between: the appliance's
    # console is a byte stream it reads one character at a time, so the
    # answers are consumed by the prompts in the order they were written.
    wait_prepare "$RUN1_MARK" 3 60
    echo "   reviewing again and confirming"
    say_to_appliance "done"
    say_to_appliance "yes"
    wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "FALCON-APPLIANCE-PREP-SELECTED 1" 60

    echo "   preparing /$WIN_REL with p3 still locked; this takes minutes"
    T_PREP=$(date +%s%N)
    # Waited for by outcome rather than by clock: the work is proportional to
    # the image, so no fixed timeout would mean anything. Both outcomes end
    # the wait, and the prefix has to be the whole marker: "...-PREP" alone
    # would match the PREP-PENDING line printed a moment ago and return at
    # once with nothing to report.
    PREP1=
    prep_i=0
    while (( prep_i < 18000 )); do
        PREP1=$(tail -c "+$(( RUN1_MARK + 1 ))" "$CONSOLE_LOG" 2>/dev/null |
                grep -aE 'FALCON-APPLIANCE-(PREPARED|PREP-FAILED) ' |
                tail -1 | sed 's/\r$//' || true)
        [[ -n "$PREP1" ]] && break
        sleep 0.1
        prep_i=$(( prep_i + 1 ))
    done
    [[ -n "$PREP1" ]] ||
        fail "preparation of /$WIN_REL neither finished nor failed in 30 minutes"
    echo "   TIMING prepare $(( ($(date +%s%N) - T_PREP) / 1000000 )) ms" \
         "(hash the ISO, extract boot.wim, locate and prove its extent," \
         "inject, optimize, pad, verify, record)"
    echo "  $PREP1"
    [[ "$PREP1" == *"FALCON-APPLIANCE-PREPARED $WIN_REL"* ]] ||
        fail "preparation did not succeed for $WIN_REL: '$PREP1'"
else
    [[ "$SURVEY1" == *"0 to prepare"* ]] ||
        fail "nothing Windows is on p2, so nothing should be queued to prepare"
    seen_from "$RUN1_MARK" "FALCON-APPLIANCE-PREP-PENDING" &&
        fail "a preparation batch was announced with nothing to prepare"
    (( $(prepare_count "$RUN1_MARK") == 0 )) ||
        fail "a prepare gate was opened with nothing to prepare"
    echo "   no prepare gate, because nothing on p2 needs preparing"
    echo "   NOTE: the prepare gate of DESIGN Section 7.7 is NOT exercised by"
    echo "         this run. Rebuild with FALCON_WIN_ISO to cover it."
fi

unlock_appliance "$RUN1_MARK"

wait_for "$CONSOLE_LOG" "FALCON-APPLIANCE-PENDING" 120
PENDING1=$(grep -a "FALCON-APPLIANCE-PENDING" "$CONSOLE_LOG" | tail -1)
echo "   $PENDING1"
[[ "$PENDING1" == *"PENDING 2"* ]] ||
    fail "run 1 should see both staged ISOs pending, got '$PENDING1'"

# The listing is the loader's folder view in text (DESIGN Section 6 stage 1,
# Section 7.5): a header per folder, the root's images first, and a mark box
# per image. Checked as a shape, not as a blob of text, so a regression that
# flattens the grouping fails here rather than being read past.
wait_select "$RUN1_MARK" 1 60
seen_from "$RUN1_MARK" "pending unsigned images:" ||
    fail "the appliance did not print the pending list"
seen_line_from "$RUN1_MARK" "  $FOLDER/" ||
    fail "no '$FOLDER/' folder header: the listing is not grouped by folder"
seen_from "$RUN1_MARK" "[ ]  1  $ROOT_ISO_NAME" ||
    fail "the root ISO is not listed as unmarked image 1"
seen_from "$RUN1_MARK" "[ ]  2  $FOLDER_ISO_NAME" ||
    fail "the folder's ISO is not listed as unmarked image 2"
echo "   grouped listing:"
tail -c "+$(( RUN1_MARK + 1 ))" "$CONSOLE_LOG" |
    awk '/Mark what you vouch for/ { exit }
         /pending unsigned images/ { f = 1 }
         f { print }' | sed 's/\r$//; s/^/     /'

# A line that means nothing must cost one error line and nothing else. The
# selections are the user's vouch; losing them to a typo would push the user
# toward answering fast the second time.
echo "   sending junk at the selection prompt"
say_to_appliance "bogus"
wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "not understood: 'bogus'" 60

wait_select "$RUN1_MARK" 2 60
echo "   marking image 2 ($FOLDER/$FOLDER_ISO_NAME)"
say_to_appliance "2"
wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "[x]  2  $FOLDER_ISO_NAME" 60

wait_select "$RUN1_MARK" 3 60
say_to_appliance "done"
wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "review: 1 image marked" 60
seen_from "$RUN1_MARK" "  /$FOLDER/$FOLDER_ISO_NAME" ||
    fail "the review did not print the marked image's full path"
seen_from "$RUN1_MARK" "  /$ROOT_ISO_NAME" &&
    fail "the review listed an image that was never marked"
echo "   review shows exactly the one marked path; confirming"

T_SIGN=$(date +%s%N)
say_to_appliance "yes"
wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "FALCON-APPLIANCE-SELECTED 1" 60
wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "FALCON-APPLIANCE-SIGNED" 900
echo "   TIMING bless $(( ($(date +%s%N) - T_SIGN) / 1000000 )) ms" \
     "(hash the ISO, extract the kernel, ukify, sbsign, record)"
SIGNED1=$(tail -c "+$(( RUN1_MARK + 1 ))" "$CONSOLE_LOG" |
          grep -a "FALCON-APPLIANCE-SIGNED" | tail -1 | sed 's/\r$//')
echo "  $SIGNED1"
[[ "$SIGNED1" == *"$FOLDER/$FOLDER_ISO_NAME"* ]] ||
    fail "run 1 signed something other than the marked image: '$SIGNED1'"

wait_for "$CONSOLE_LOG" "FALCON-APPLIANCE-SUMMARY" 600
SUMMARY1=$(grep -a "FALCON-APPLIANCE-SUMMARY" "$CONSOLE_LOG" | tail -1)
echo "   $SUMMARY1"
[[ "$SUMMARY1" == *"signed=1 failed=0 skipped=1"* ]] ||
    fail "run 1 summary is '$SUMMARY1'"

# The order the run actually happened in, read back off the finished log:
# p3 is unlocked after the survey has said the bless half has work, and after
# any preparation has already finished. That is the whole of the "preparation
# needs no key" claim, and it is the one thing about this run that a passing
# summary would not catch.
before "$RUN1_MARK" "survey: " "p3 unlocked (read-only)"
if [[ -n "$WIN_REL" ]]; then
    [[ "$SUMMARY1" == *"prepared=1 prep-failed=0 prep-skipped=0"* ]] ||
        fail "run 1 summary does not report the prepared image: '$SUMMARY1'"
    before "$RUN1_MARK" "FALCON-APPLIANCE-PREPARED" "p3 unlocked (read-only)"
    echo "   p3 was unlocked only after the preparation batch had finished"
else
    [[ "$SUMMARY1" == *"prepared=0 prep-failed=0 prep-skipped=0"* ]] ||
        fail "run 1 summary claims preparation happened: '$SUMMARY1'"
    echo "   p3 was unlocked only after the survey, and nothing was prepared"
fi
# The appliance now waits for Enter before rebooting so a human can read
# the summary; the rig is that human here.
wait_for "$CONSOLE_LOG" "press Enter to reboot" 60
say_to_appliance ""

wait_for "$CONSOLE_LOG" "rebooting." 60
echo "   appliance rebooted"

# ---- run 2: idempotence, and a quit that signs nothing ----

echo
echo "== run 2: same stick, second visit"
RUN2_MARK=$(size_of "$CONSOLE_LOG")
enter_appliance

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "falcon-boot signing appliance" 180

# Idempotence for the preparation half, which is what DESIGN Section 6.3.1
# rests on: the second visit must recognise its own work and say so, not
# spend another three minutes redoing it and not drop the image silently.
wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "survey: " 300
SURVEY2=$(tail -c "+$(( RUN2_MARK + 1 ))" "$CONSOLE_LOG" |
          grep -a "^survey: " | tail -1 | sed 's/\r$//' || true)
echo "   $SURVEY2"
[[ "$SURVEY2" == *"0 to prepare"* ]] ||
    fail "run 2 queued something to prepare again: '$SURVEY2'"
if [[ -n "$WIN_REL" ]]; then
    seen_from "$RUN2_MARK" "prepared    /$WIN_REL (already; skipping)" ||
        fail "run 2 did not report the prepared Windows ISO as a visible skip"
    seen_from "$RUN2_MARK" "FALCON-APPLIANCE-PREP-PENDING" &&
        fail "run 2 started a preparation batch for an image already prepared"
    echo "   the prepared Windows ISO is recognised and skipped by name"
fi

unlock_appliance "$RUN2_MARK"

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "FALCON-APPLIANCE-PENDING" 120
PENDING2=$(tail -c "+$(( RUN2_MARK + 1 ))" "$CONSOLE_LOG" |
           grep -a "FALCON-APPLIANCE-PENDING" | tail -1)
echo "   $PENDING2"
[[ "$PENDING2" == *"PENDING 1"* ]] ||
    fail "run 2 should have exactly one pending ISO left, got '$PENDING2'"

wait_select "$RUN2_MARK" 1 120
# The blessed image must be reported as an explicit skip and must not appear
# in the selection list. A silent disappearance would read to the user as the
# appliance having lost it (DESIGN Section 6.3.1 applies the same rule to
# prepared media), so both halves of that are asserted.
seen_from "$RUN2_MARK" "blessed     /$FOLDER/$FOLDER_ISO_NAME (already; skipping)" ||
    fail "run 2 did not report the blessed ISO as a visible skip"
seen_from "$RUN2_MARK" "]  1  $FOLDER_ISO_NAME" &&
    fail "the blessed ISO is still listed as pending"
seen_line_from "$RUN2_MARK" "  $FOLDER/" &&
    fail "the '$FOLDER/' header survived though its only image is blessed"
seen_from "$RUN2_MARK" "[ ]  1  $ROOT_ISO_NAME" ||
    fail "the still-unsigned root ISO is not the one pending image"
echo "   only the unsigned root ISO is pending, and its folder header is gone"

echo "   quitting at the selection prompt"
say_to_appliance "quit"
wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "leaving without signing anything" 60

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "FALCON-APPLIANCE-SUMMARY" 120
SUMMARY2=$(tail -c "+$(( RUN2_MARK + 1 ))" "$CONSOLE_LOG" |
           grep -a "FALCON-APPLIANCE-SUMMARY" | tail -1)
echo "   $SUMMARY2"
[[ "$SUMMARY2" == *"signed=0 failed=0 skipped=1"* ]] ||
    fail "run 2 summary is '$SUMMARY2'"
seen_from "$RUN2_MARK" "FALCON-APPLIANCE-SELECTED" &&
    fail "run 2 reported a confirmed selection though it was quit"
seen_from "$RUN2_MARK" "FALCON-APPLIANCE-SIGNED" &&
    fail "run 2 signed something after quit"
echo "   quit confirmed nothing and signed nothing"

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "press Enter to reboot" 60
say_to_appliance ""
wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "rebooting." 60
echo "   appliance rebooted"

# Back at the loader menu, which is where a user would be. Nothing else to
# prove inside the machine, so it is stopped rather than left running.
sleep 5
mon "quit" || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
kill "$READER_PID" 2>/dev/null || true
READER_PID=

# ---- what actually landed on the stick ----

echo
echo "== checking p2 on the host"
LOOP=$(attach_img)
sudo udevadm settle 2>/dev/null || sleep 1
P2_UUID=$(sudo blkid -s UUID -o value "${LOOP}p2")
MNT=$(mktemp -d)
sudo mount -o ro,uid="$(id -u)",gid="$(id -g)" "${LOOP}p2" "$MNT"

UKI="$MNT/falcon/blessed/$BLESSED_UKI"
[[ -f "$UKI" ]] || fail "no $BLESSED_UKI on p2"
echo "   wrap:     $BLESSED_UKI ($(stat -c%s "$UKI") bytes)"

# One wrap and one record, because one image was confirmed. The unmarked one
# and the run that was quit must have left nothing behind at all.
WRAPS=$(find "$MNT/falcon/blessed" -maxdepth 1 -name '*.uki' | wc -l)
[[ "$WRAPS" == 1 ]] ||
    fail "$WRAPS wraps under falcon/blessed; exactly one image was confirmed"
WANT_RECORDS=1
[[ -n "$WIN_REL" ]] && WANT_RECORDS=2   # the blessed one, plus the prepared one
RECORDS=$(grep -ac '^iso=' "$MNT/falcon/blessed/manifest" || true)
[[ "$RECORDS" == "$WANT_RECORDS" ]] ||
    fail "$RECORDS manifest records; $WANT_RECORDS were produced"
echo "   exactly one wrap and $WANT_RECORDS manifest record(s)"

sbverify --cert "$CERT" "$UKI" >/dev/null 2>&1 ||
    fail "$BLESSED_UKI does not verify against $CERT"
echo "   signature verifies against the stick's own MOK certificate"

MAN="$MNT/falcon/blessed/manifest"
[[ -f "$MAN" ]] || fail "no manifest on p2"

# One key out of the block that names a given ISO. The file is blocks
# separated by iso= lines (DESIGN Section 8.1), and once two records exist a
# bare grep for a key answers from whichever block came first, which is the
# prepared one.
rec_get() {
    awk -v want="$1" -v key="$2" '
        { sub(/\r$/, "") }
        /^iso=/ { inrec = (substr($0, 5) == want) }
        inrec && index($0, key "=") == 1 { print substr($0, length(key) + 2); exit }
    ' "$MAN"
}

REC_ISO="$FOLDER/$FOLDER_ISO_NAME"
REC_SHA=$(rec_get "$REC_ISO" sha256)
REC_SIZE=$(rec_get "$REC_ISO" size)
[[ -n "$REC_SHA" && -n "$REC_SIZE" ]] ||
    fail "the manifest has no complete record for '$REC_ISO'"
REAL_SHA=$(sha256sum "$MNT/$REC_ISO" | cut -d' ' -f1)
REAL_SIZE=$(stat -c%s "$MNT/$REC_ISO")
[[ "$REC_SHA" == "$REAL_SHA" ]] ||
    fail "manifest sha256 $REC_SHA is not the ISO's $REAL_SHA"
[[ "$REC_SIZE" == "$REAL_SIZE" ]] ||
    fail "manifest size $REC_SIZE is not the ISO's $REAL_SIZE"
echo "   manifest: iso=$REC_ISO size=$REC_SIZE"
echo "             sha256=$REC_SHA (recomputed on the host: matches)"

# The cmdline is the half of the bless that tampering with p2 cannot reach,
# so it is worth reading back out of the signed image rather than trusting the
# tool's own report of it.
CMDLINE=$(objcopy -O binary --only-section=.cmdline "$UKI" /dev/stdout |
          tr -d '\0')
echo "   cmdline:  $CMDLINE"
[[ "$CMDLINE" == *"img_dev=UUID=$P2_UUID"* ]] ||
    fail "the signed cmdline does not name p2 (UUID=$P2_UUID): $CMDLINE"
[[ "$CMDLINE" == *"img_loop=/$REC_ISO"* ]] ||
    fail "the signed cmdline does not point at the ISO: $CMDLINE"
echo "             names p2 by the serial that partition really carries"

# ---- what preparation left behind (DESIGN Sections 6.3, 6.3.1, 8.1) ----
#
# The record and the sidecar are read back rather than trusted, for the same
# reason the blessed cmdline is: the appliance's own report of what it wrote
# is not evidence that it wrote it.
if [[ -n "$WIN_REL" ]]; then
    echo
    echo "== checking the prepared Windows media"
    PREP_SHA=$(rec_get "$WIN_REL" sha256)
    PREP_SIZE=$(rec_get "$WIN_REL" size)
    PREP_KIND=$(rec_get "$WIN_REL" kind)
    PREP_SPLICE=$(rec_get "$WIN_REL" splice)
    [[ "$PREP_KIND" == "prepared" ]] ||
        fail "the record for /$WIN_REL is kind='$PREP_KIND', not prepared"
    [[ "$PREP_SIZE" == "$WIN_SIZE" ]] ||
        fail "the record says size=$PREP_SIZE, the ISO is $WIN_SIZE bytes"

    # The ISO is the user's file and preparation opens it read-only. If its
    # hash still matches what the record claims, it was not written to, which
    # is the one property of this route the user has to be able to rely on.
    WIN_SHA=$(sha256sum "$MNT/$WIN_REL" | cut -d' ' -f1)
    [[ "$PREP_SHA" == "$WIN_SHA" ]] ||
        fail "record sha256 $PREP_SHA is not the ISO's $WIN_SHA; either the" \
             "hash is wrong or the ISO was modified in place"
    echo "   record:   iso=$WIN_REL size=$PREP_SIZE kind=prepared"
    echo "             sha256=$PREP_SHA (recomputed on the host: matches, so"
    echo "             the ISO itself was not written to)"

    # splice=<iso offset>:<length>:<file in the sidecar folder>
    SPL_OFF=${PREP_SPLICE%%:*}
    SPL_REST=${PREP_SPLICE#*:}
    SPL_LEN=${SPL_REST%%:*}
    SPL_NAME=${SPL_REST#*:}
    [[ "$SPL_OFF" =~ ^[0-9]+$ && "$SPL_LEN" =~ ^[0-9]+$ && -n "$SPL_NAME" ]] ||
        fail "splice='$PREP_SPLICE' is not <offset>:<length>:<name>"
    SIDECAR="$MNT/falcon/prepared/$PREP_SHA/$SPL_NAME"
    [[ -f "$SIDECAR" ]] ||
        fail "no sidecar at falcon/prepared/$PREP_SHA/$SPL_NAME"
    SIDECAR_SIZE=$(stat -c%s "$SIDECAR")
    [[ "$SIDECAR_SIZE" == "$SPL_LEN" ]] ||
        fail "the sidecar is $SIDECAR_SIZE bytes, the record promises $SPL_LEN"
    echo "   splice:   $SPL_OFF:$SPL_LEN:$SPL_NAME"
    echo "             sidecar is exactly the length the record promises"

    # The range the loader will serve the sidecar over has to be the range the
    # original occupies, or Windows reads our bytes at the wrong place. The
    # original's length is read out of the ISO here, independently of the tool.
    ORIG_LEN=$(7z l -ba -slt "$MNT/$WIN_REL" |
               awk '/^Path = / { p = tolower(substr($0, 8)) }
                    p ~ /sources\/boot\.wim$/ && /^Size = / { v = $3 }
                    END { print v }')
    [[ "$ORIG_LEN" == "$SPL_LEN" ]] ||
        fail "the splice covers $SPL_LEN bytes; the ISO's boot.wim is $ORIG_LEN"
    (( SPL_OFF % 2048 == 0 )) ||
        fail "the splice starts at $SPL_OFF, which is not a sector boundary"
    echo "             covering exactly the length the ISO's own directory"
    echo "             entry gives for /sources/boot.wim, sector-aligned"

    # The patched image has to still be a WIM, and it has to carry the three
    # files DESIGN Section 6.3 injects. Read with wimlib rather than believed.
    wimverify "$SIDECAR" >/dev/null 2>&1 ||
        fail "the staged boot.wim does not pass wimverify"
    WIM_BOOT=$(wiminfo "$SIDECAR" | awk '/^Boot Index:/ { v = $3 }
                                        END { print v }')
    [[ -n "$WIM_BOOT" && "$WIM_BOOT" != 0 ]] ||
        fail "the staged boot.wim names no bootable image"
    WIM_LS=$(wimdir "$SIDECAR" "$WIM_BOOT" 2>/dev/null | tr -d '\r' |
             tr 'A-Z\\' 'a-z/')
    for f in /windows/system32/winpeshl.exe \
             /windows/system32/winpeshl_orig.exe \
             /windows/system32/falcon-winjump.cfg; do
        # Whole-line match without a pipe. The listing is tens of thousands of
        # paths, so `printf | grep -qx` is the SIGPIPE trap described above.
        case $'\n'"$WIM_LS"$'\n' in
        *$'\n'"$f"$'\n'*) ;;
        *) fail "the patched image has no $f" ;;
        esac
    done
    echo "   image $WIM_BOOT: winpeshl.exe, winpeshl_orig.exe and"
    echo "             falcon-winjump.cfg are all present"

    # winpeshl.exe must be OUR binary now, byte for byte, and the preserved
    # copy must not be.
    EXDIR=$(mktemp -d)
    wimextract "$SIDECAR" "$WIM_BOOT" \
        /Windows/System32/winpeshl.exe \
        /Windows/System32/winpeshl_orig.exe \
        /Windows/System32/falcon-winjump.cfg \
        --dest-dir="$EXDIR" --no-acls >/dev/null 2>&1 ||
        fail "could not extract the injected files from the patched image"
    cmp -s "$EXDIR/winpeshl.exe" build/bin/winjump.exe ||
        fail "winpeshl.exe in the patched image is not build/bin/winjump.exe"
    cmp -s "$EXDIR/winpeshl_orig.exe" build/bin/winjump.exe &&
        fail "winpeshl_orig.exe is our jump binary; Microsoft's was lost"
    grep -qF "iso=$WIN_REL" "$EXDIR/falcon-winjump.cfg" ||
        fail "the WinPE config does not name the ISO: $(cat "$EXDIR/falcon-winjump.cfg")"
    grep -qF "size=$WIN_SIZE" "$EXDIR/falcon-winjump.cfg" ||
        fail "the WinPE config does not carry the ISO's size"
    rm -rf "$EXDIR"
    echo "             winpeshl.exe is winjump.exe byte for byte, Microsoft's"
    echo "             is preserved beside it, and the config names the ISO"
fi

# Nothing the appliance produced may contain the private key, and nothing
# outside p3 may hold a copy of it. Checked rather than asserted.
if grep -arl 'PRIVATE KEY' "$MNT" >/dev/null 2>&1; then
    fail "something on p2 contains a PEM private key"
fi
echo "   no private key material anywhere on p2"

sudo umount "$MNT"; rmdir "$MNT"; MNT=
detach_img "$LOOP"; LOOP=

echo
if [[ -n "$WIN_REL" ]]; then
    echo "PASS: the signing appliance prepared a Windows ISO with p3 locked,"
    echo "      then blessed a Linux ISO, both end to end under Secure Boot."
else
    echo "PASS: the signing appliance blessed an ISO end to end under Secure Boot."
    echo "      The Windows leg was not exercised: nothing under /$WIN_FOLDER on p2."
fi
echo "  loader log:    $SERIAL0"
echo "  appliance log: $CONSOLE_LOG"
