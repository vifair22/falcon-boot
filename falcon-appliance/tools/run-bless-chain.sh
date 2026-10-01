#!/usr/bin/env bash
# falcon-boot: blessing from the loader's menu, end to end, in one QEMU
# session (docs/DESIGN.md Section 7.5).
#
# The chain, stage by stage:
#
#   1  p2 is reset to "nothing blessed" (falcon/blessed removed); the two
#      pending fixture ISOs stay.
#   2  the LOADER boots with Secure Boot on and the stick's own MOK enrolled.
#   3  on the root ISO's row, b asks to bless that one image; the confirmation
#      names exactly it; y confirms.
#   4  the loader sets FalconNextBoot, unlocks p3 and boots the appliance with
#      the passphrase and the list as credentials. The appliance signs the
#      listed image with no UI of its own (no select> prompt, no typed yes)
#      and reboots by itself.
#   5  after that reboot the loader consumes the marker, does NOT draw the
#      menu, and boots the entry blessed.
#   6  back on the host, the manifest records the listed image and not the
#      other pending one: the appliance signed what the menu confirmed and
#      nothing else.
#
# Usage: falcon-appliance/tools/run-bless-chain.sh
# Env:   FALCON_TEST_PASS  the stick's passphrase (default "falcon-test")
#        FALCON_KEEP       non-empty leaves the guest running at the end
#
# Prerequisite: build/appliance/appliance-stick.img
#   (falcon-appliance/tools/mk-test-appliance-stick.sh, or make test-appliance-stick)
#
# Output: build/bless-chain/ - the loader serial log, the appliance console
# log and screendumps.

set -euo pipefail

cd "$(dirname "$0")/../.."
. tools/rig-part.sh

OUT=build/appliance
RUN=build/bless-chain
IMG="$OUT/appliance-stick.img"
MOK_VARS="$OUT/OVMF_VARS.secboot.mok.fd"

VARS_RUN="$RUN/vars-run.fd"
SERIAL0="$RUN/loader-serial.log"
CONSOLE="$RUN/console"
CONSOLE_LOG="$RUN/appliance-console.log"
MON="$RUN/mon.sock"

PASS="${FALCON_TEST_PASS:-falcon-test}"
ROOT_ISO=arch-mini-root.iso
FOLDER_ISO=Linux/arch-mini-linux.iso

QEMU_PID=
READER_PID=
LOOP=
MNT=

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
    [[ -n "$LOOP" ]] && sudo losetup -d "$LOOP" 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    [[ -n "$READER_PID" ]] && kill "$READER_PID" 2>/dev/null || true
    if [[ -n "$QEMU_PID" && -z "${FALCON_KEEP:-}" ]]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$CONSOLE.in" "$CONSOLE.out"
    [[ -n "${FALCON_KEEP:-}" ]] || rm -f "$MON"
}
trap cleanup EXIT

[[ -f "$IMG" ]] || {
    echo "ERROR: $IMG missing (make test-appliance-stick)" >&2
    exit 1
}
# p2 by its identity record (DESIGN Section 3.1).
P2="p$(rig_part "$IMG" library)"
mkdir -p "$RUN"

fail() {
    echo
    echo "FAIL: $*" >&2
    echo "  loader log:    $SERIAL0"
    echo "  appliance log: $CONSOLE_LOG"
    exit 1
}

size_of() { stat -c%s "$1" 2>/dev/null || echo 0; }
mon() { printf '%s\n' "$*" | socat - "UNIX-CONNECT:$MON" >/dev/null; }
key() { mon "sendkey $1"; sleep "${2:-2}"; }
type_ascii() { falcon-efi/tools/win-spike/mon-type.sh "$MON" "$1" "${2:-}"; }

# awk rather than grep -q, so tail never dies of SIGPIPE under pipefail.
seen_from() {
    tail -c "+$(( $2 + 1 ))" "$1" 2>/dev/null |
        sed -e 's/\x1b\[[0-9;?=]*[a-zA-Z]//g' |
        awk -v pat="$3" 'index($0, pat) { f = 1 } END { exit f ? 0 : 1 }'
}
wait_for_from() {
    local i=0
    while (( i < ${4:-120} * 10 )); do
        seen_from "$1" "$2" "$3" && return 0
        sleep 0.1
        i=$(( i + 1 ))
    done
    fail "timed out after ${4:-120}s waiting for '$3' in $1"
}
count_from() {
    tail -c "+$(( $2 + 1 ))" "$1" 2>/dev/null |
        awk -v pat="$3" 'index($0, pat) { n++ } END { print n + 0 }'
}
# falcon-efi/tools/rig-shot.sh converts without Pillow.
shot() {
    MON_SOCK="$MON" falcon-efi/tools/rig-shot.sh "$1" "$RUN" >/dev/null 2>&1 ||
        true
    rm -f "$RUN/$1.ppm"
}

p2_mount() {   # p2_mount ro|rw
    sync
    LOOP=$(sudo losetup --show -f -P "$IMG")
    sudo udevadm settle 2>/dev/null || sleep 1
    MNT=$(mktemp -d)
    sudo mount -o "$1,uid=$(id -u),gid=$(id -g)" "${LOOP}$P2" "$MNT"
}
p2_umount() {
    sudo umount "$MNT"; rmdir "$MNT"; MNT=
    sudo losetup -d "$LOOP"; LOOP=
}

# ---- stage 1 ----

echo "== stage 1: resetting p2 to 'nothing blessed'"
p2_mount rw
[[ -f "$MNT/$ROOT_ISO" && -f "$MNT/$FOLDER_ISO" ]] ||
    fail "the fixture ISOs are not on p2"
rm -rf "$MNT/falcon/blessed"
p2_umount

# ---- stage 2 ----

echo "== stage 2: booting the loader (Secure Boot on, the stick's own MOK)"
rm -f "$VARS_RUN" "$SERIAL0" "$CONSOLE_LOG" "$CONSOLE.in" "$CONSOLE.out" \
      "$MON" "$RUN"/*.png
mkfifo "$CONSOLE.in" "$CONSOLE.out"

STICK_IMG="$IMG" MOK_VARS="$MOK_VARS" VARS_FILE="$VARS_RUN" \
SERIAL_LOG="$SERIAL0" MON_SOCK="$MON" QEMU_MEM=4096 \
    falcon-efi/tools/run-qemu.sh --stick --secboot --headless \
        -smp 2 -serial "pipe:$CONSOLE" &
QEMU_PID=$!
( cat "$CONSOLE.out" > "$CONSOLE_LOG" ) &
READER_PID=$!
exec 3> "$CONSOLE.in"

BOOT_MARK="BdsDxe: starting Boot"
wait_for_from "$SERIAL0" 0 "$BOOT_MARK" 120
wait_for_from "$SERIAL0" 0 "b bless" 180
shot 01-menu

# The root screen: the pinned personal OS, then the folders, then the root's
# images. The root ISO is the first image row.
DOWNS=2
seen_from "$SERIAL0" 0 "Windows/" && DOWNS=3
for (( i = 0; i < DOWNS; i++ )); do key down 1; done

# ---- stage 3 ----

echo "== stage 3: b on /$ROOT_ISO, and the confirmation"
MARK=$(size_of "$SERIAL0")
key b
wait_for_from "$SERIAL0" "$MARK" "Bless 1 image with the key on p3" 60
seen_from "$SERIAL0" "$MARK" "/$ROOT_ISO" ||
    fail "the confirmation does not name /$ROOT_ISO"
seen_from "$SERIAL0" "$MARK" "/$FOLDER_ISO" &&
    fail "the confirmation names /$FOLDER_ISO, which was not asked for"
seen_from "$SERIAL0" "$MARK" "reboots into /$ROOT_ISO" ||
    fail "the confirmation does not say where the reboot lands"
shot 02-confirm
echo "   the confirmation names /$ROOT_ISO alone"

# ---- stage 4 ----

echo "== stage 4: y, the passphrase, and the appliance"
MARK=$(size_of "$SERIAL0")
key y
wait_for_from "$SERIAL0" "$MARK" "starting the signing appliance." 60
wait_for_from "$SERIAL0" "$MARK" "passphrase for the encrypted partition" 60
sleep 1
type_ascii "$PASS" --enter
wait_for_from "$CONSOLE_LOG" 0 "falcon-boot signing appliance" 300
wait_for_from "$CONSOLE_LOG" 0 "bless list from the loader" 120
wait_for_from "$CONSOLE_LOG" 0 "confirmed in the loader's menu" 300
echo "   the appliance took the list and skipped its own selection"
wait_for_from "$CONSOLE_LOG" 0 "FALCON-APPLIANCE-SIGNED $ROOT_ISO" 600
seen_from "$CONSOLE_LOG" 0 "select>" &&
    fail "the appliance asked for a selection of its own"
seen_from "$CONSOLE_LOG" 0 "p3 unlocked with the passphrase from the loader" ||
    seen_from "$CONSOLE_LOG" 0 "unlocking p3 with the passphrase from the loader" ||
    fail "p3 was not unlocked with the loader's credential"
wait_for_from "$CONSOLE_LOG" 0 "rebooting to the loader." 120
echo "   signed /$ROOT_ISO and rebooting, with no prompt"

# ---- stage 5 ----

echo "== stage 5: the reboot lands on the entry, blessed"
MARK=$(size_of "$SERIAL0")
wait_for_from "$SERIAL0" "$MARK" "$BOOT_MARK" 180
(( $(count_from "$SERIAL0" 0 "$BOOT_MARK") == 2 )) ||
    fail "expected two firmware starts in one QEMU process"
wait_for_from "$SERIAL0" "$MARK" "booting blessed: $ROOT_ISO" 300
seen_from "$SERIAL0" "$MARK" "esc back" &&
    fail "the menu was drawn before the queued entry booted"
wait_for_from "$SERIAL0" "$MARK" "handing over to the image" 300
shot 03-blessed-boot
echo "   firmware start #2 booted /$ROOT_ISO blessed, without the menu"

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

# ---- stage 6 ----

echo "== stage 6: what p2 says afterwards"
p2_mount ro
MAN="$MNT/falcon/blessed/manifest"
[[ -f "$MAN" ]] || fail "no manifest on p2"
grep -q "^iso=$ROOT_ISO\$" "$MAN" || fail "the manifest has no record for /$ROOT_ISO"
grep -q "^iso=$FOLDER_ISO\$" "$MAN" &&
    fail "the manifest records /$FOLDER_ISO, which nobody confirmed"
echo "   manifest: /$ROOT_ISO recorded, /$FOLDER_ISO untouched"
p2_umount

echo
echo "PASS: blessed from the menu, one confirmation, one reboot"
