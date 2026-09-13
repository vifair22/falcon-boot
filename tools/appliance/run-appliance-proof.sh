#!/usr/bin/env bash
# airies-boot: the signing appliance's acceptance run (DESIGN Section 7.5,
# build order step 8).
#
# Boots build/appliance/appliance-stick.img under OVMF with Secure Boot ON and
# a varstore that trusts the stick's own MOK, drives the loader into the
# appliance, and drives the appliance itself. Two appliance runs in one QEMU
# session, because the second one is what proves the first one was recorded:
#
#   run 1  confirm the Arch ISO ("yes"), decline payload-test.iso
#          -> expect 2 pending, 1 signed, 1 skipped
#   run 2  a WRONG passphrase first (expect a clean retry, no key exposure),
#          then the right one; the Arch ISO must no longer be pending, and
#          confirming payload-test.iso must fail cleanly (it carries no
#          Linux kernel for the bless to wrap)
#          -> expect 1 pending, 0 signed, 1 failed
#
# Then the stick is mounted on the host and checked: the wrap exists, it
# verifies against the stick's certificate, the manifest's sha256 is the ISO's
# real hash, and the signed cmdline names p2 by the volume serial that
# partition actually carries.
#
# Two serial ports, and the reason matters: ttyS0 is the rig's usual
# write-only log (tools/run-qemu.sh --headless), which carries the firmware
# and the loader. The appliance needs a console it can READ from, so this
# script adds a second port as a pair of FIFOs and the appliance's cmdline
# puts its console there (tools/appliance/mk-appliance.sh). The loader menu is
# still driven the rig's usual way, by sendkey through the QEMU monitor.
#
# Usage: tools/appliance/run-appliance-proof.sh
# Env:   AIRIES_TEST_PASS  the stick's passphrase (default "airies-test")

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

PASS="${AIRIES_TEST_PASS:-airies-test}"
WRONG_PASS=wrongpass

ARCH_ISO_NAME=archlinux-2026.01.01-x86_64.iso
BLESSED_UKI=archlinux-2026.01.01-x86_64.uki

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
        sudo losetup -d "$LOOP" 2>/dev/null || true
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
        if tail -c "+$(( off + 1 ))" "$file" 2>/dev/null | grep -aqF "$pat"; then
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    fail "timed out after ${secs}s waiting for '$pat' in $file"
}

# Prompts are counted rather than searched for by offset: the appliance
# prints the whole list and the first prompt in one breath, so a driver that
# marks a position only after reading the list has already missed it. The Nth
# prompt of the session is an unambiguous thing to wait for.
prompt_count() { grep -ac "sign this ISO" "$CONSOLE_LOG" 2>/dev/null || true; }

wait_prompt() {
    local want="$1" secs="${2:-120}" i=0

    while (( i < secs * 10 )); do
        if (( $(prompt_count) >= want )); then
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    fail "timed out after ${secs}s waiting for confirmation prompt #$want"
}

# Which ISO the newest prompt is about: the appliance names it on the line
# immediately above.
current_iso() {
    # The console is a serial line, so every line ends CR LF; the CR would
    # otherwise ride along inside the file name and fail every comparison.
    grep -ao '\[[0-9]*/[0-9]*\] [^ ]*' "$CONSOLE_LOG" | tail -1 | tr -d '\r'
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

    # One UKI boots without a chooser; two or more are offered, and on this
    # stick the second one is the appliance (src/pos/pos.c says so in as many
    # words). Which row it lands on is the loader's business, so the row is
    # read back out of the screen rather than assumed.
    if wait_for_from "$SERIAL0" "$mark" "Personal OS: " 60; then
        sleep 1
        entries=$(tail -c "+$(( mark + 1 ))" "$SERIAL0" |
                  grep -ao 'Personal OS: [A-Za-z0-9_.-]*' | awk '!seen[$0]++')
        idx=$(printf '%s\n' "$entries" | grep -n 'appliance-uki.efi' | cut -d: -f1)
        [[ -n "$idx" ]] || fail "the appliance is not in the loader's UKI chooser"
        echo "   chooser: appliance-uki.efi is entry $idx of $(printf '%s\n' "$entries" | wc -l)"
        for (( i = 1; i < idx; i++ )); do
            mon "sendkey down"
            sleep 0.1
        done
        mon "sendkey ret"
    fi
    T_LAUNCH=$(date +%s%N)
}

# ---- start ----

# Run 1 exists to turn an unblessed stick into a blessed one, so a stick that
# has already been through it cannot prove anything. Checked here rather than
# discovered three minutes in as a confusing count mismatch.
LOOP=$(sudo losetup --show -f -P "$IMG")
sudo udevadm settle 2>/dev/null || sleep 1
MNT=$(mktemp -d)
sudo mount -o ro "${LOOP}p2" "$MNT"
if [[ -f "$MNT/airies/blessed/manifest" ]]; then
    sudo umount "$MNT"; rmdir "$MNT"; MNT=
    sudo losetup -d "$LOOP"; LOOP=
    echo "ERROR: $IMG already carries a manifest, so nothing on it is pending." >&2
    echo "       Rebuild it first: tools/mk-test-appliance-stick.sh" >&2
    exit 1
fi
sudo umount "$MNT"; rmdir "$MNT"; MNT=
sudo losetup -d "$LOOP"; LOOP=

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

# ---- run 1: bless the Arch ISO ----

echo
echo "== run 1: drive the loader into the appliance"
enter_appliance

wait_for "$CONSOLE_LOG" "airies-boot signing appliance" 180
wait_for "$CONSOLE_LOG" "passphrase for p3 (attempt 1 of 3)" 120
T_PROMPT=$(date +%s%N)
echo "   appliance is up and asking for the passphrase"
echo "   TIMING boot-to-prompt $(( (T_PROMPT - T_LAUNCH) / 1000000 )) ms" \
     "(from the menu selection, so it includes the loader reading the image)"
say_to_appliance "$PASS"

wait_for "$CONSOLE_LOG" "AIRIES-APPLIANCE-PENDING" 120
grep -a "AIRIES-APPLIANCE-PENDING" "$CONSOLE_LOG" | tail -1

# Two prompts, answered by what the prompt names: the Arch ISO is vouched
# for, the payload ISO is not. Answering per ISO is the point of the flow, so
# the driver answers per ISO too rather than replying to a count.
for n in 1 2; do
    wait_prompt "$n" 120
    iso=$(current_iso)
    if [[ "$iso" == *"$ARCH_ISO_NAME" ]]; then
        echo "   $iso -> confirming"
        T_SIGN=$(date +%s%N)
        say_to_appliance "yes"
        wait_for "$CONSOLE_LOG" "AIRIES-APPLIANCE-SIGNED" 900
        echo "   TIMING bless $(( ($(date +%s%N) - T_SIGN) / 1000000 )) ms" \
             "(hash 1.4 GiB, extract the kernel, ukify, sbsign, record)"
    else
        echo "   $iso -> declining"
        say_to_appliance "no"
    fi
done

wait_for "$CONSOLE_LOG" "AIRIES-APPLIANCE-SUMMARY" 600
SUMMARY1=$(grep -a "AIRIES-APPLIANCE-SUMMARY" "$CONSOLE_LOG" | tail -1)
echo "   $SUMMARY1"
[[ "$SUMMARY1" == *"signed=1 failed=0 skipped=1"* ]] ||
    fail "run 1 summary is '$SUMMARY1'"

wait_for "$CONSOLE_LOG" "rebooting to the loader" 60
echo "   appliance rebooted"

# ---- run 2: idempotence, a wrong passphrase, and a bless that must fail ----

echo
echo "== run 2: same stick, second visit"
RUN2_MARK=$(size_of "$CONSOLE_LOG")
enter_appliance

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "passphrase for p3 (attempt 1 of 3)" 180
echo "   sending a WRONG passphrase first"
say_to_appliance "$WRONG_PASS"
wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "wrong passphrase" 120
wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "passphrase for p3 (attempt 2 of 3)" 60
echo "   refused, and it asked again; sending the right one"
say_to_appliance "$PASS"

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "AIRIES-APPLIANCE-PENDING" 120
PENDING2=$(tail -c "+$(( RUN2_MARK + 1 ))" "$CONSOLE_LOG" |
           grep -a "AIRIES-APPLIANCE-PENDING" | tail -1)
echo "   $PENDING2"
[[ "$PENDING2" == *"PENDING 1"* ]] ||
    fail "run 2 should have exactly one pending ISO left, got '$PENDING2'"
tail -c "+$(( RUN2_MARK + 1 ))" "$CONSOLE_LOG" | grep -a "^  \[1\]" |
    grep -aq "$ARCH_ISO_NAME" &&
    fail "the Arch ISO is still listed as pending after being blessed"

wait_prompt 3 120
iso=$(current_iso)
[[ "$iso" == *payload-test.iso ]] ||
    fail "run 2 offered '$iso', not the one ISO that is still pending"
echo "   $iso -> confirming, and it has no kernel to wrap"
say_to_appliance "yes"
wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "AIRIES-APPLIANCE-FAILED" 300

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "AIRIES-APPLIANCE-SUMMARY" 300
SUMMARY2=$(tail -c "+$(( RUN2_MARK + 1 ))" "$CONSOLE_LOG" |
           grep -a "AIRIES-APPLIANCE-SUMMARY" | tail -1)
echo "   $SUMMARY2"
[[ "$SUMMARY2" == *"signed=0 failed=1 skipped=0"* ]] ||
    fail "run 2 summary is '$SUMMARY2'"

wait_for_from "$CONSOLE_LOG" "$RUN2_MARK" "rebooting to the loader" 60
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
LOOP=$(sudo losetup --show -f -P "$IMG")
sudo udevadm settle 2>/dev/null || sleep 1
P2_UUID=$(sudo blkid -s UUID -o value "${LOOP}p2")
MNT=$(mktemp -d)
sudo mount -o ro,uid="$(id -u)",gid="$(id -g)" "${LOOP}p2" "$MNT"

UKI="$MNT/airies/blessed/$BLESSED_UKI"
[[ -f "$UKI" ]] || fail "no $BLESSED_UKI on p2"
echo "   wrap:     $BLESSED_UKI ($(stat -c%s "$UKI") bytes)"

sbverify --cert "$CERT" "$UKI" >/dev/null 2>&1 ||
    fail "$BLESSED_UKI does not verify against $CERT"
echo "   signature verifies against the stick's own MOK certificate"

MAN="$MNT/airies/blessed/manifest"
[[ -f "$MAN" ]] || fail "no manifest on p2"
REC_SHA=$(grep -a '^sha256=' "$MAN" | head -1 | cut -d= -f2)
REC_SIZE=$(grep -a '^size=' "$MAN" | head -1 | cut -d= -f2)
REC_ISO=$(grep -a '^iso=' "$MAN" | head -1 | cut -d= -f2)
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

# Nothing the appliance produced may contain the private key, and nothing
# outside p3 may hold a copy of it. Checked rather than asserted.
if grep -arl 'PRIVATE KEY' "$MNT" >/dev/null 2>&1; then
    fail "something on p2 contains a PEM private key"
fi
echo "   no private key material anywhere on p2"

sudo umount "$MNT"; rmdir "$MNT"; MNT=
sudo losetup -d "$LOOP"; LOOP=

echo
echo "PASS: the signing appliance blessed an ISO end to end under Secure Boot."
echo "  loader log:    $SERIAL0"
echo "  appliance log: $CONSOLE_LOG"
