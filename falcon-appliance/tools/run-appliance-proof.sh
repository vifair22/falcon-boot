#!/usr/bin/env bash
# falcon-boot: the signing appliance's acceptance run (DESIGN Section 7.5).
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
# Two serial ports, and the reason matters: ttyS0 is the rig's usual write-only
# log (falcon-efi/tools/run-qemu.sh --headless), which carries the firmware and
# the loader. The appliance needs a console it can READ from, so this script
# adds a second port as a pair of FIFOs, and ttyS1 is one of the consoles the
# appliance opens (falcon-appliance/src/console.c writes to all of them and
# reads from whichever answers, so which console= came last on the cmdline does
# not decide whether this drive works). The loader menu is still driven the
# rig's usual way, by sendkey through the QEMU monitor.
#
# What this log does NOT carry is the output of the helper tools the appliance
# runs: those inherit /dev/console, which the cmdline points at the screen so
# a real machine shows its own progress. Only the appliance's own lines, the
# ones asserted below, come down the serial port.
#
# Usage: falcon-appliance/tools/run-appliance-proof.sh
# Env:   FALCON_TEST_PASS  the stick's passphrase (default "falcon-test")

set -euo pipefail

cd "$(dirname "$0")/../.."
. tools/rig-part.sh

OUT=build/appliance
APPLIANCE_ARCH="${APPLIANCE_ARCH:-x64}"
if [[ $APPLIANCE_ARCH == aa64 ]]; then
    # virt under AAVMF, emulated: one serial port for the loader's log and
    # the appliance's console both, and every wait SCALE times as long.
    SUFFIX=-aa64
    MOK_VARS="$OUT/AAVMF_VARS.secboot.mok.fd"
    QEMU_BIN=qemu-system-aarch64
    ARCH_ARGS=(--aa64)
    SCALE=${AA64_SCALE:-6}
else
    SUFFIX=
    MOK_VARS="$OUT/OVMF_VARS.secboot.mok.fd"
    QEMU_BIN=qemu-system-x86_64
    ARCH_ARGS=()
    SCALE=1
fi
IMG="$OUT/appliance-stick$SUFFIX.img"
CERT="$OUT/appliance-mok$SUFFIX.crt"
VARS_RUN="$OUT/vars-run.fd"
SERIAL0="$OUT/loader-serial.log"
CONSOLE="$OUT/console"          # $CONSOLE.in / $CONSOLE.out, the ttyS1 FIFOs
CONSOLE_LOG="$OUT/appliance-console.log"
MON="$OUT/mon.sock"

PASS="${FALCON_TEST_PASS:-falcon-test}"

# What falcon-appliance/tools/mk-test-appliance-stick.sh staged on p2, and what
# signing the second of them must produce under falcon/blessed.
ROOT_ISO_NAME=arch-mini-root.iso
FOLDER=Linux
FOLDER_ISO_NAME=arch-mini-linux.iso
BLESSED_UKI=arch-mini-linux.uki

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

for t in "$QEMU_BIN" socat sbverify sha256sum blkid; do
    command -v "$t" >/dev/null 2>&1 || { echo "ERROR: $t not found" >&2; exit 1; }
done
[[ -f "$IMG" ]] || {
    echo "ERROR: $IMG missing (run: falcon-appliance/tools/mk-test-appliance-stick.sh)" >&2
    exit 1
}
# p2 is the partition whose identity record says library (DESIGN Section
# 3.1), read off the image before anything attaches it.
P2="p$(rig_part "$IMG" library)"

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

    while (( i < secs * 10 * SCALE )); do
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

    while (( i < secs * 10 * SCALE )); do
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
    # labelled from each image's .osrel (falcon-core/src/media/osrel.c), so the
    # appliance is found by the name it gives itself, not by its file name, and
    # the chooser puts it LAST (falcon-efi/src/pos/pos.c, uki_before), which is
    # asserted here: a stick whose chooser opens on the appliance boots it on
    # Enter.
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
            if [[ -b "${l}$P2" ]]; then
                break
            fi
            sudo udevadm settle 2>/dev/null || true
            sleep 0.25
        done
        if [[ -b "${l}$P2" ]]; then
            sudo blockdev --flushbufs "$l" 2>/dev/null || true
            sudo blockdev --flushbufs "${l}$P2" 2>/dev/null || true
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
sudo mount -o ro "${LOOP}$P2" "$MNT"
if [[ -f "$MNT/falcon/blessed/manifest" ]]; then
    sudo umount "$MNT"; rmdir "$MNT"; MNT=
    detach_img "$LOOP"; LOOP=
    echo "ERROR: $IMG already carries a manifest, so nothing on it is pending." >&2
    echo "       Rebuild it first: falcon-appliance/tools/mk-test-appliance-stick.sh" >&2
    exit 1
fi
sudo umount "$MNT"; rmdir "$MNT"; MNT=
detach_img "$LOOP"; LOOP=
echo "== booting $IMG (Secure Boot ON, MokList = this stick's own cert)"
rm -f "$VARS_RUN" "$SERIAL0" "$CONSOLE_LOG" "$CONSOLE.in" "$CONSOLE.out" "$MON"
mkfifo "$CONSOLE.in" "$CONSOLE.out"

# 2 GiB because the initramfs is ~77 MiB uncompressed and the signing run
# keeps the extracted kernel and the unsigned wrap in a tmpfs on purpose.
if [[ $APPLIANCE_ARCH == aa64 ]]; then
    STICK_IMG="$IMG" MOK_VARS_AA64="$MOK_VARS" VARS_FILE="$VARS_RUN" \
    SERIAL_PIPE="$CONSOLE" MON_SOCK="$MON" QEMU_MEM=2048 \
        falcon-efi/tools/run-qemu.sh "${ARCH_ARGS[@]}" --stick --secboot \
            --headless -smp 2 &
else
    STICK_IMG="$IMG" MOK_VARS="$MOK_VARS" VARS_FILE="$VARS_RUN" \
    SERIAL_LOG="$SERIAL0" MON_SOCK="$MON" QEMU_MEM=2048 \
        falcon-efi/tools/run-qemu.sh --stick --secboot --headless \
            -serial "pipe:$CONSOLE" &
fi
QEMU_PID=$!

# The reader has to be running before the guest writes anything, or the FIFO
# backs up; the writer end is held open for the whole session so the guest
# never sees EOF on its console. With one port it is both logs.
if [[ $APPLIANCE_ARCH == aa64 ]]; then
    ( cat "$CONSOLE.out" | tee "$SERIAL0" > "$CONSOLE_LOG" ) &
else
    ( cat "$CONSOLE.out" > "$CONSOLE_LOG" ) &
fi
READER_PID=$!
exec 3> "$CONSOLE.in"

# The offset of the appliance's banner at or after MARK. On a machine with
# one serial port the loader's menu is in the same log, folder rows and all,
# so a run's checks start at its banner.
banner_mark() {   # banner_mark MARK -> offset
    local at
    at=$(tail -c "+$(( $1 + 1 ))" "$CONSOLE_LOG" |
         grep -a -b -o "falcon-boot signing appliance" | head -n 1 | cut -d: -f1)
    echo $(( $1 + ${at:-0} ))
}

# ---- run 1: mark one image, review it, sign it ----

echo
echo "== run 1: drive the loader into the appliance"
RUN1_MARK=$(size_of "$CONSOLE_LOG")
enter_appliance

wait_for "$CONSOLE_LOG" "falcon-boot signing appliance" 180
T_UP=$(date +%s%N)
RUN1_MARK=$(banner_mark "$RUN1_MARK")
echo "   appliance is up"
echo "   TIMING boot-to-console $(( (T_UP - T_LAUNCH) / 1000000 )) ms" \
     "(from the menu selection, so it includes the loader reading the image)"

wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "surveying p2..." 120
wait_for_from "$CONSOLE_LOG" "$RUN1_MARK" "survey: " 120
SURVEY1=$(tail -c "+$(( RUN1_MARK + 1 ))" "$CONSOLE_LOG" |
          grep -a "^survey: " | tail -1 | sed 's/\r$//' || true)
echo "   $SURVEY1"
[[ "$SURVEY1" == *"2 to sign"* ]] ||
    fail "run 1 should survey both staged ISOs as pending: '$SURVEY1'"

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
     "(hash the ISO, extract the kernel, wrap, sbsign, record)"
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

# p3 is unlocked only after the survey has said there is work, read back off
# the finished log: a run with nothing to sign must never decrypt the key.
before "$RUN1_MARK" "survey: " "p3 unlocked (read-only)"
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
RUN2_MARK=$(banner_mark "$RUN2_MARK")

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "survey: " 300
SURVEY2=$(tail -c "+$(( RUN2_MARK + 1 ))" "$CONSOLE_LOG" |
          grep -a "^survey: " | tail -1 | sed 's/\r$//' || true)
echo "   $SURVEY2"
[[ "$SURVEY2" == *"1 to sign, 1 already blessed"* ]] ||
    fail "run 2 survey is '$SURVEY2'"

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
# appliance having lost it, so both halves of that are asserted.
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
P2_UUID=$(sudo blkid -s UUID -o value "${LOOP}$P2")
MNT=$(mktemp -d)
sudo mount -o ro,uid="$(id -u)",gid="$(id -g)" "${LOOP}$P2" "$MNT"

UKI="$MNT/falcon/blessed/$BLESSED_UKI"
[[ -f "$UKI" ]] || fail "no $BLESSED_UKI on p2"
echo "   wrap:     $BLESSED_UKI ($(stat -c%s "$UKI") bytes)"

# One wrap and one record, because one image was confirmed. The unmarked one
# and the run that was quit must have left nothing behind at all.
WRAPS=$(find "$MNT/falcon/blessed" -maxdepth 1 -name '*.uki' | wc -l)
[[ "$WRAPS" == 1 ]] ||
    fail "$WRAPS wraps under falcon/blessed; exactly one image was confirmed"
RECORDS=$(grep -ac '^iso=' "$MNT/falcon/blessed/manifest" || true)
[[ "$RECORDS" == 1 ]] ||
    fail "$RECORDS manifest records; exactly one was produced"
echo "   exactly one wrap and one manifest record"

sbverify --cert "$CERT" "$UKI" >/dev/null 2>&1 ||
    fail "$BLESSED_UKI does not verify against $CERT"
echo "   signature verifies against the stick's own MOK certificate"

MAN="$MNT/falcon/blessed/manifest"
[[ -f "$MAN" ]] || fail "no manifest on p2"

# One key out of the block that names a given ISO. The file is blocks
# separated by iso= lines (DESIGN Section 8.1), and once two records exist a
# bare grep for a key answers from whichever block came first.
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
# llvm-objcopy where there is one: GNU objcopy here reads x86 PE alone, and
# llvm-objcopy's -O binary writes a PE whole, so the section is dumped.
OBJCOPY=$(command -v llvm-objcopy || command -v objcopy)
"$OBJCOPY" --dump-section ".cmdline=$OUT/cmdline.bin" "$UKI" "$OUT/uki.tmp"
CMDLINE=$(tr -d '\0' < "$OUT/cmdline.bin")
rm -f "$OUT/cmdline.bin" "$OUT/uki.tmp"
echo "   cmdline:  $CMDLINE"
if [[ $APPLIANCE_ARCH == aa64 ]]; then
    # Fedora's family: the ISO named by its path, which dracut's iso-scan
    # looks for on every partition.
    [[ "$CMDLINE" == *"iso-scan/filename=/$REC_ISO"* ]] ||
        fail "the signed cmdline does not point at the ISO: $CMDLINE"
    echo "             names the ISO by its path inside p2"
else
    [[ "$CMDLINE" == *"img_dev=UUID=$P2_UUID"* ]] ||
        fail "the signed cmdline does not name p2 (UUID=$P2_UUID): $CMDLINE"
    [[ "$CMDLINE" == *"img_loop=/$REC_ISO"* ]] ||
        fail "the signed cmdline does not point at the ISO: $CMDLINE"
    echo "             names p2 by the serial that partition really carries"
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
echo "PASS: the signing appliance blessed an ISO end to end under Secure Boot."
echo "  loader log:    $SERIAL0"
echo "  appliance log: $CONSOLE_LOG"
