#!/usr/bin/env bash
# falcon-boot: the Windows auto-prep chain, end to end, in one QEMU session
# (docs/DESIGN.md Sections 6.3 and 6.3.1).
#
# falcon-appliance/tools/run-appliance-proof.sh proves the appliance's two
# halves and reads back what preparation wrote. It stops there. This script
# proves the two joints that one never touches, and it has to be one continuous
# run because the joints ARE the continuity:
#
#   (a) the loader serving a SPLICED image. Every Windows Setup proof before
#       this one booted an ISO whose boot.wim had been patched IN PLACE. Here
#       the ISO on p2 is byte-for-byte the user's own file and the patched
#       boot.wim lives in a sidecar under /falcon/prepared/<sha256>/, reached
#       only through the override layer of falcon-efi/src/iso. That is why the
#       ISO's hash is recomputed on the host at the end: if the hash is
#       unchanged and Windows Setup still found its install source, the bytes
#       Setup read came from the sidecar and from nowhere else. There is no
#       third place they could have come from.
#   (b) the FalconNextBoot round trip through a REAL reboot. The guest reboots
#       itself; this script never restarts QEMU, and it asserts that by
#       counting firmware starts in a serial log written by one QEMU process
#       whose pid it holds the whole time.
#
# The chain, stage by stage:
#
#   1  p2 is reset to "nothing prepared, nothing blessed" (falcon/blessed and
#      falcon/prepared removed), so the stick is in the state a user's is the
#      first time they drop a Windows ISO on it. The ISO itself is not touched.
#   2  the LOADER boots, and the Windows entry must be labelled [not prepared].
#   3  selecting it must explain what will happen and ask; 'p' confirms.
#   4  the loader sets FalconNextBoot and launches the appliance, which
#      prepares the ISO and reboots.
#   5  after that reboot the loader consumes the marker, does NOT draw the
#      menu, and boots the prepared entry.
#   6  that boot goes through the override layer ("booting prepared media; N
#      byte range(s) served from the stick").
#   7  cdboot.efi wants a keypress (DESIGN Section 6.3: it draws its prompt
#      over GOP, never on ConOut, so the serial log shows nothing at all).
#   8  winjump attaches the ISO from WinPE and Setup comes up. The evidence is
#      taken from Shift+F10: X:\falcon-winjump.log, and a dism /get-wiminfo
#      against the attached media's \sources\install.wim.
#
# Known environment artefact, recorded so nobody reads it as a failure: Setup's
# own click-through stalls on the language page under OVMF/QEMU even with real
# CD media, so the dism enumeration is the accepted evidence that Setup has an
# install source, not the GUI advancing.
#
# Usage: falcon-appliance/tools/run-winprep-chain.sh
# Env:   FALCON_TEST_PASS  the stick's passphrase (default "falcon-test")
#        FALCON_KEEP       non-empty leaves the guest running at the end
#
# Prerequisite: build/appliance/appliance-stick.img, built with a real Windows
# ISO staged on p2:
#   FALCON_WIN_ISO=/path/to/Win11.iso falcon-appliance/tools/mk-test-appliance-stick.sh
#
# Output: build/winprep-chain/ - the loader serial log, the appliance console
# log, and every screendump as PNG. The serial log carries the loader's ConOut
# and NOTHING from Windows, which is why the shots exist.

set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=build/appliance
RUN=build/winprep-chain
IMG="$OUT/appliance-stick.img"
MOK_VARS="$OUT/OVMF_VARS.secboot.mok.fd"

VARS_RUN="$RUN/vars-run.fd"
SERIAL0="$RUN/loader-serial.log"
CONSOLE="$RUN/console"          # $CONSOLE.in / $CONSOLE.out, the ttyS1 FIFOs
CONSOLE_LOG="$RUN/appliance-console.log"
MON="$RUN/mon.sock"

PASS="${FALCON_TEST_PASS:-falcon-test}"

# Windows Setup wants real memory and more than one CPU; 4096/2 is what the
# premise check (falcon-efi/tools/win-spike) reached the Setup GUI with, and the
# appliance half wants 2 GiB of its own for the initramfs, so the larger number
# wins.
GUEST_MEM=6144
GUEST_CPUS=4

WIN_FOLDER=Windows

QEMU_PID=
READER_PID=
LOOP=
MNT=

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
    if [[ -n "$QEMU_PID" && -z "${FALCON_KEEP:-}" ]]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=
    fi
    rm -f "$CONSOLE.in" "$CONSOLE.out"
    [[ -n "${FALCON_KEEP:-}" ]] || rm -f "$MON"
}
trap cleanup EXIT

for t in qemu-system-x86_64 socat sha256sum python3; do
    command -v "$t" >/dev/null 2>&1 || { echo "ERROR: $t not found" >&2; exit 1; }
done
python3 -c 'import PIL' 2>/dev/null || {
    echo "ERROR: python3 Pillow not found; screendumps could not be read" >&2
    exit 1
}
[[ -f "$IMG" ]] || {
    echo "ERROR: $IMG missing. Build it with a real Windows ISO:" >&2
    echo "  FALCON_WIN_ISO=<iso> falcon-appliance/tools/mk-test-appliance-stick.sh" >&2
    exit 1
}

mkdir -p "$RUN"

# ---- helpers (the conventions are run-appliance-proof.sh's) ----

fail() {
    echo
    echo "FAIL: $*" >&2
    echo "  loader log:    $SERIAL0"
    echo "  appliance log: $CONSOLE_LOG"
    echo "  screendumps:   $RUN/*.png"
    exit 1
}

size_of() { stat -c%s "$1" 2>/dev/null || echo 0; }

mon() { printf '%s\n' "$*" | socat - "UNIX-CONNECT:$MON" >/dev/null; }

# Nothing here is done on a timer where evidence exists to wait for instead:
# every step waits for the line the previous one produces.
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

# awk rather than `grep -q`: grep stops reading at the first match, tail dies
# of SIGPIPE, and under `set -o pipefail` the pipeline reports 141 even though
# the pattern WAS found.
seen_from() {
    local file="$1" off="$2" pat="$3"

    tail -c "+$(( off + 1 ))" "$file" 2>/dev/null |
        awk -v pat="$pat" 'index($0, pat) { found = 1 }
                           END { exit found ? 0 : 1 }'
}

count_from() {
    local file="$1" off="$2" pat="$3"

    tail -c "+$(( off + 1 ))" "$file" 2>/dev/null |
        awk -v pat="$pat" 'index($0, pat) { n++ } END { print n + 0 }'
}

say_to_appliance() { printf '%s\n' "$1" >&3; }

key() { mon "sendkey $1"; sleep "${2:-2}"; }

# The loader menu is driven one key at a time with a gap: batched sendkeys get
# dropped by the firmware's PS/2 input queue.
type_ascii() { falcon-efi/tools/win-spike/mon-type.sh "$MON" "$1" "${2:-}"; }

SHOT_N=0
shot() {
    local tag="$1" ppm png

    SHOT_N=$(( SHOT_N + 1 ))
    ppm=$(printf '%s/%02d-%s.ppm' "$RUN" "$SHOT_N" "$tag")
    png="${ppm%.ppm}.png"
    mon "screendump $PWD/$ppm"
    sleep 1
    python3 -c "
from PIL import Image
Image.open('$ppm').save('$png')
" 2>/dev/null || { echo "note: could not convert $ppm" >&2; return 0; }
    rm -f "$ppm"
    echo "   shot: $png"
}

attach_img() {
    local l i j

    sync
    for j in 1 2 3 4 5; do
        l=$(sudo losetup --show -f -P "$IMG")
        for i in $(seq 1 20); do
            [[ -b "${l}p2" ]] && break
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

# ---- stage 1: a stick with a real, UNPREPARED Windows ISO on p2 ----
#
# The reset is the cheap half of a rebuild: mkstick leaves /falcon empty and
# everything under it is the appliance's own output, so removing those two
# directories puts the stick back in the state a freshly created one is in,
# without recopying 7.7 GB. The ISO is read, never written.

echo "== stage 1: resetting p2 to 'nothing prepared, nothing blessed'"
LOOP=$(attach_img)
sudo udevadm settle 2>/dev/null || sleep 1
MNT=$(mktemp -d)
sudo mount -o rw,uid="$(id -u)",gid="$(id -g)" "${LOOP}p2" "$MNT"

WIN_REL=$(cd "$MNT" && find "$WIN_FOLDER" -maxdepth 1 -type f -iname '*.iso' \
    2>/dev/null | head -1 || true)
[[ -n "$WIN_REL" ]] || {
    sudo umount "$MNT"; rmdir "$MNT"; MNT=
    detach_img "$LOOP"; LOOP=
    echo "ERROR: no ISO under /$WIN_FOLDER on p2. Rebuild the stick with" >&2
    echo "  FALCON_WIN_ISO=<iso> falcon-appliance/tools/mk-test-appliance-stick.sh" >&2
    exit 1
}
WIN_NAME=$(basename "$WIN_REL")
WIN_SIZE=$(stat -c%s "$MNT/$WIN_REL")

rm -rf "$MNT/falcon/blessed" "$MNT/falcon/prepared"
sync
[[ -e "$MNT/falcon/blessed" || -e "$MNT/falcon/prepared" ]] &&
    fail "p2 still carries prepared or blessed artifacts after the reset"

echo "   ISO:  /$WIN_REL ($WIN_SIZE bytes)"
echo "   hashing it on the host, so 'unmodified' can be proven afterwards"
WIN_SHA_BEFORE=$(sha256sum "$MNT/$WIN_REL" | cut -d' ' -f1)
echo "   sha256=$WIN_SHA_BEFORE"

sudo umount "$MNT"; rmdir "$MNT"; MNT=
detach_img "$LOOP"; LOOP=

# ---- stage 2: boot the LOADER ----

echo
echo "== stage 2: booting the loader (Secure Boot ON, stick's own MOK)"
rm -f "$VARS_RUN" "$SERIAL0" "$CONSOLE_LOG" "$CONSOLE.in" "$CONSOLE.out" "$MON"
rm -f "$RUN"/*.png "$RUN"/*.ppm
mkfifo "$CONSOLE.in" "$CONSOLE.out"

STICK_IMG="$IMG" MOK_VARS="$MOK_VARS" VARS_FILE="$VARS_RUN" \
SERIAL_LOG="$SERIAL0" MON_SOCK="$MON" QEMU_MEM="$GUEST_MEM" \
    falcon-efi/tools/run-qemu.sh --stick --secboot --headless \
        -smp "$GUEST_CPUS" -serial "pipe:$CONSOLE" &
QEMU_PID=$!
echo "   qemu pid $QEMU_PID; it is never restarted, which is what makes the"
echo "   reboot in stage 5 provably the guest's own"

( cat "$CONSOLE.out" > "$CONSOLE_LOG" ) &
READER_PID=$!
exec 3> "$CONSOLE.in"

# The firmware start line is the boot counter for the whole run.
BOOT_MARK="BdsDxe: starting Boot0001"
wait_for_from "$SERIAL0" 0 "$BOOT_MARK" 120
wait_for_from "$SERIAL0" 0 "esc back" 180
BOOTS_AT_MENU=$(count_from "$SERIAL0" 0 "$BOOT_MARK")
(( BOOTS_AT_MENU == 1 )) ||
    fail "expected exactly one firmware start before the menu, saw $BOOTS_AT_MENU"
echo "   menu is up after firmware start #1"
shot "loader-menu"

# Into the Windows folder. The menu is a folder view: Personal OS is pinned at
# the top, then the folders, then the root's images.
echo "   opening the $WIN_FOLDER/ folder"
MARK_MENU=$(size_of "$SERIAL0")
key down
key down
key ret
wait_for_from "$SERIAL0" "$MARK_MENU" "$WIN_NAME" 60
shot "windows-folder"

seen_from "$SERIAL0" "$MARK_MENU" "[not prepared]" ||
    fail "the Windows entry is not labelled [not prepared]"
LABEL=$(tail -c "+$(( MARK_MENU + 1 ))" "$SERIAL0" |
        sed -e 's/\x1b\[[0-9;?]*[a-zA-Z]//g' -e 's/\x1b\[=[0-9]*h//g' |
        tr -d '\r' | grep -ao "$WIN_NAME[^|]*\[not prepared\]" | head -1)
echo "   label: '$LABEL'"

# ---- stage 3: select it, read the offer, confirm ----

echo
echo "== stage 3: selecting the unprepared entry"
MARK_OFFER=$(size_of "$SERIAL0")
key down          # past the '..' row
key ret
wait_for_from "$SERIAL0" "$MARK_OFFER" "[p] prepare now (one reboot)" 60
seen_from "$SERIAL0" "$MARK_OFFER" "This Windows ISO has not been prepared." ||
    fail "the offer screen did not say the ISO is unprepared"
shot "prepare-offer"
echo "   the loader explained the flow and offered [p]/[b]/[esc]"

# ---- stage 4: FalconNextBoot is set and the appliance runs ----

echo
echo "== stage 4: confirming with 'p'"
MARK_APP=$(size_of "$SERIAL0")
CONSOLE_MARK=$(size_of "$CONSOLE_LOG")
key p
wait_for_from "$SERIAL0" "$MARK_APP" "starting the signing appliance." 60
echo "   the loader says it is starting the appliance (the marker is written"
echo "   before this line, falcon-efi/src/efi/main.c launch_appliance)"

wait_for_from "$SERIAL0" "$MARK_APP" "passphrase for the encrypted partition" 60
sleep 1
type_ascii "$PASS" --enter
wait_for_from "$SERIAL0" "$MARK_APP" "reading appliance-uki.efi" 120
echo "   p3 unlocked and the appliance image is being read"

wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" "falcon-boot signing appliance" 300
echo "   appliance is up"

wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" "survey: " 180
SURVEY=$(tail -c "+$(( CONSOLE_MARK + 1 ))" "$CONSOLE_LOG" |
         grep -a "^survey: " | tail -1 | sed 's/\r$//' || true)
echo "   $SURVEY"
[[ "$SURVEY" == *"1 to prepare"* ]] ||
    fail "the Windows ISO was not routed to the prepare queue: '$SURVEY'"

wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" "FALCON-APPLIANCE-PREP-PENDING 1" 120

# The prepare gate of DESIGN Section 7.7: the batch does not start until the
# image has been marked, reviewed and confirmed. run-appliance-proof.sh is
# where that gate is proved clause by clause; here it is simply answered, so
# the chain this script exists to show can carry on.
wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" "Windows images that are not" 120
echo "   prepare gate: marking the image, reviewing it and confirming"
say_to_appliance "all"
say_to_appliance "done"
say_to_appliance "yes"
wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" \
    "FALCON-APPLIANCE-PREP-SELECTED 1" 60

echo "   preparing /$WIN_REL; this takes minutes and p3 is still locked"
T_PREP=$(date +%s)
PREP=
prep_i=0
while (( prep_i < 18000 )); do
    PREP=$(tail -c "+$(( CONSOLE_MARK + 1 ))" "$CONSOLE_LOG" 2>/dev/null |
           grep -aE 'FALCON-APPLIANCE-(PREPARED|PREP-FAILED) ' |
           tail -1 | sed 's/\r$//' || true)
    [[ -n "$PREP" ]] && break
    sleep 0.1
    prep_i=$(( prep_i + 1 ))
done
[[ -n "$PREP" ]] || fail "preparation neither finished nor failed in 30 minutes"
echo "   $PREP  ($(( $(date +%s) - T_PREP ))s)"
[[ "$PREP" == *"FALCON-APPLIANCE-PREPARED $WIN_REL"* ]] ||
    fail "preparation did not succeed for $WIN_REL: '$PREP'"

# The bless half still has the rig stick's two unsigned Linux ISOs pending, so
# the appliance asks. Nothing here is what this script exists to prove, so it
# is quit: no key is used and nothing is signed.
#
# The loader normally hands the appliance the passphrase it already collected
# (DESIGN Section 5.1), so p3 unlocks with no prompt; the prompt is the
# fallback for a missing credential. Which one happens is the loader's
# business, so it is waited for rather than assumed.
unlock_i=0
while (( unlock_i < 3000 )); do
    if seen_from "$CONSOLE_LOG" "$CONSOLE_MARK" "p3 unlocked (read-only)"; then
        echo "   p3 unlocked with the credential the loader delivered"
        break
    fi
    if seen_from "$CONSOLE_LOG" "$CONSOLE_MARK" \
            "passphrase for p3 (attempt 1 of 3)"; then
        echo "   no usable credential; typing the passphrase"
        say_to_appliance "$PASS"
        wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" \
            "p3 unlocked (read-only)" 120
        break
    fi
    if seen_from "$CONSOLE_LOG" "$CONSOLE_MARK" "nothing to sign"; then
        echo "   the bless half had nothing to do, so p3 stayed locked"
        break
    fi
    sleep 0.1
    unlock_i=$(( unlock_i + 1 ))
done

sel_i=0
while (( sel_i < 3000 )); do
    if seen_from "$CONSOLE_LOG" "$CONSOLE_MARK" "select>"; then
        echo "   quitting the bless half; this run signs nothing"
        say_to_appliance "quit"
        wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" \
            "leaving without signing anything" 60
        break
    fi
    if seen_from "$CONSOLE_LOG" "$CONSOLE_MARK" "FALCON-APPLIANCE-SUMMARY"; then
        break
    fi
    sleep 0.1
    sel_i=$(( sel_i + 1 ))
done
wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" "FALCON-APPLIANCE-SUMMARY" 300
SUMMARY=$(tail -c "+$(( CONSOLE_MARK + 1 ))" "$CONSOLE_LOG" |
          grep -a "FALCON-APPLIANCE-SUMMARY" | tail -1 | sed 's/\r$//')
echo "   $SUMMARY"
[[ "$SUMMARY" == *"prepared=1 prep-failed=0"* ]] ||
    fail "the summary does not report the prepared image: '$SUMMARY'"

wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" "press Enter to reboot" 60
say_to_appliance ""
wait_for_from "$CONSOLE_LOG" "$CONSOLE_MARK" "rebooting." 60
echo "   the appliance is rebooting the machine"

# ---- stage 5: the marker is consumed and the menu is NOT drawn ----

echo
echo "== stage 5: the reboot, and the one-shot that follows it"
MARK_REBOOT=$(size_of "$SERIAL0")
wait_for_from "$SERIAL0" "$MARK_REBOOT" "$BOOT_MARK" 180
kill -0 "$QEMU_PID" 2>/dev/null ||
    fail "the QEMU process died; the second boot was not the guest's own"
BOOTS_NOW=$(count_from "$SERIAL0" 0 "$BOOT_MARK")
(( BOOTS_NOW == 2 )) ||
    fail "expected 2 firmware starts in this one QEMU process, saw $BOOTS_NOW"
echo "   firmware start #2 in the same qemu pid $QEMU_PID: a genuine reboot"

wait_for_from "$SERIAL0" "$MARK_REBOOT" "booting prepared: " 300
seen_from "$SERIAL0" "$MARK_REBOOT" "esc back" &&
    fail "the menu was drawn before the queued entry booted"
echo "   the menu was NOT drawn: the marker booted the entry directly"

# ---- stage 6: through the override layer ----

wait_for_from "$SERIAL0" "$MARK_REBOOT" \
    "booting prepared media; " 120
OVL=$(tail -c "+$(( MARK_REBOOT + 1 ))" "$SERIAL0" |
      sed -e 's/\x1b\[[0-9;?]*[a-zA-Z]//g' | tr -d '\r' |
      grep -ao 'booting prepared media; [0-9]* byte range(s) served from the stick.' |
      head -1)
echo "== stage 6: $OVL"
shot "prepared-boot"

# ---- stage 7: cdboot.efi wants a key ----

echo
echo "== stage 7: feeding cdboot.efi a keypress (it draws over GOP, so the"
echo "   serial log shows nothing whatever from here on)"
for i in $(seq 1 12); do
    mon "sendkey ret"
    sleep 1
done
shot "after-keys"

# ---- stage 8: WinPE, winjump, and Setup's install source ----

echo
echo "== stage 8: waiting for WinPE and Setup (shots every 20s)"
for i in $(seq 1 15); do
    sleep 20
    shot "setup-$(printf '%02d' "$i")"
done

echo "   opening Shift+F10"
mon "sendkey shift-f10"
sleep 5
shot "shift-f10"

echo "   reading X:\\falcon-winjump.log"
type_ascii 'cls' --enter
sleep 2
type_ascii 'type X:\falcon-winjump.log' --enter
sleep 5
shot "winjump-log"

# The attached ISO's drive letter is assigned by WinPE's PnP and is not fixed,
# so every plausible letter is tried and only the one that exists runs dism.
# `if exist` keeps the other four silent, so the screen ends up holding exactly
# one enumeration.
echo "   enumerating the install source with dism"
type_ascii 'cls' --enter
sleep 2
for L in c d e f g; do
    type_ascii "if exist $L:\\sources\\install.wim dism /get-wiminfo /wimfile:$L:\\sources\\install.wim" --enter
    sleep 3
done
sleep 45
shot "dism-get-wiminfo"

echo
echo "== stopping the guest"
if [[ -z "${FALCON_KEEP:-}" ]]; then
    mon "quit" || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
    kill "$READER_PID" 2>/dev/null || true
    READER_PID=
fi

# ---- what is on the stick afterwards ----
#
# One check carries the whole of joint (a): the ISO's hash. Setup found an
# install source; if the ISO is still byte for byte what it was before the run,
# the patched boot.wim it read came from the sidecar through the override
# layer, because there is nowhere else it could have come from.

echo
echo "== checking p2 on the host"
LOOP=$(attach_img)
sudo udevadm settle 2>/dev/null || sleep 1
MNT=$(mktemp -d)
sudo mount -o ro,uid="$(id -u)",gid="$(id -g)" "${LOOP}p2" "$MNT"

MAN="$MNT/falcon/blessed/manifest"
[[ -f "$MAN" ]] || fail "no manifest on p2"
rec_get() {
    awk -v want="$1" -v key="$2" '
        { sub(/\r$/, "") }
        /^iso=/ { inrec = (substr($0, 5) == want) }
        inrec && index($0, key "=") == 1 { print substr($0, length(key) + 2); exit }
    ' "$MAN"
}
PREP_KIND=$(rec_get "$WIN_REL" kind)
PREP_SHA=$(rec_get "$WIN_REL" sha256)
PREP_SPLICE=$(rec_get "$WIN_REL" splice)
[[ "$PREP_KIND" == "prepared" ]] ||
    fail "the record for /$WIN_REL is kind='$PREP_KIND', not prepared"

WIN_SHA_AFTER=$(sha256sum "$MNT/$WIN_REL" | cut -d' ' -f1)
[[ "$WIN_SHA_AFTER" == "$WIN_SHA_BEFORE" ]] ||
    fail "the ISO's hash changed over the run: it was written to"
[[ "$PREP_SHA" == "$WIN_SHA_BEFORE" ]] ||
    fail "the record's sha256 is not the ISO's"
echo "   the ISO is byte for byte what it was before the run"

SPL_OFF=${PREP_SPLICE%%:*}
SPL_REST=${PREP_SPLICE#*:}
SPL_LEN=${SPL_REST%%:*}
SPL_NAME=${SPL_REST#*:}
SIDECAR="$MNT/falcon/prepared/$PREP_SHA/$SPL_NAME"
[[ -f "$SIDECAR" ]] || fail "no sidecar at falcon/prepared/$PREP_SHA/$SPL_NAME"
[[ "$(stat -c%s "$SIDECAR")" == "$SPL_LEN" ]] ||
    fail "the sidecar is not the length the record promises"
echo "   splice:  $SPL_OFF:$SPL_LEN:$SPL_NAME"
echo "   sidecar: falcon/prepared/$PREP_SHA/$SPL_NAME ($SPL_LEN bytes)"

sudo umount "$MNT"; rmdir "$MNT"; MNT=
detach_img "$LOOP"; LOOP=

echo
echo "PASS: loader -> [not prepared] -> confirm -> appliance -> reboot ->"
echo "      FalconNextBoot -> spliced boot, in one QEMU session."
echo "  loader log:    $SERIAL0"
echo "  appliance log: $CONSOLE_LOG"
echo "  screendumps:   $RUN/*.png  (READ THEM: the serial log carries nothing"
echo "                 from Windows)"
