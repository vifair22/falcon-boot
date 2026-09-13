#!/usr/bin/env bash
# airies-boot: build the stick the signing-appliance proof boots
# (docs/DESIGN.md Section 7.5, build order step 8).
#
# Unlike tools/mk-test-stick.sh, this one is built by the PRODUCT tool:
# `airies-boot-mkstick create` partitions it, generates a real MOK keypair,
# signs the loader and both UKIs with it, and puts the private key on p3 inside
# the LUKS volume. That matters for this proof specifically: the appliance's
# whole job is to read that key and sign with it, so a rig that handed it a
# key some shell script had placed would be testing the shell script.
#
#   p1  ESP        64M  shim -> our signed loader
#   p2  exFAT       2G  the real Arch ISO (unsigned, and therefore pending)
#                       plus payload-test.iso (no kernel: the skip case)
#   p3  LUKS2+ext4  1G  airies-mok.key, /uki/appliance-uki.efi, /uki/fixture-uki.efi
#   p4  LUKS2      rest untouched
#
# Outputs:
#   build/appliance/appliance-stick.img          the stick
#   build/appliance/appliance-mok.crt            the MOK cert mkstick generated
#   build/appliance/OVMF_VARS.secboot.mok.fd     varstore trusting that cert
#
# Usage: tools/mk-test-appliance-stick.sh
# Env:   AIRIES_TEST_PASS  p3/p4 passphrase (default "airies-test")
#        ARCH_ISO          the ISO to stage on p2 and bless
#
# THIS IS RIG SCAFFOLDING in one respect only: the passphrase is a well-known
# string and the varstore skips the interactive MokManager enrollment a real
# machine does once. The stick itself, the keypair and the signatures are the
# product's.

set -euo pipefail

cd "$(dirname "$0")/.."

OUT=build/appliance
IMG="$OUT/appliance-stick.img"
CERT="$OUT/appliance-mok.crt"
VARS="$OUT/OVMF_VARS.secboot.mok.fd"
PASSFILE="$OUT/passphrase"

PASS="${AIRIES_TEST_PASS:-airies-test}"
ARCH_ISO="${ARCH_ISO:-/mnt/980pro/vtoy-backup/Linux/archlinux-2026.01.01-x86_64.iso}"
PAYLOAD_ISO=build/test-payload.iso

MKSTICK=build/bin/airies-boot-mkstick
APPLIANCE_UKI="$OUT/appliance-uki.efi"
FIXTURE_UKI=build/test/fixture-uki.efi
LOADER=build/bin/airies-boot.efi
SHIM_DIR=build/shim/bins

LOOP=
MNT=

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
    rm -f "$PASSFILE"
}
trap cleanup EXIT

[[ -x "$MKSTICK" ]] || { echo "ERROR: $MKSTICK not built (run: make cli)" >&2; exit 1; }
[[ -f "$APPLIANCE_UKI" ]] || {
    echo "ERROR: $APPLIANCE_UKI missing (run: tools/appliance/mk-appliance.sh)" >&2
    exit 1
}
[[ -f "$LOADER" ]] || { echo "ERROR: $LOADER not built (run: make)" >&2; exit 1; }
[[ -f "$ARCH_ISO" ]] || { echo "ERROR: $ARCH_ISO not found (set ARCH_ISO)" >&2; exit 1; }
if [[ ! -f "$SHIM_DIR/fedora-16.1-shimx64.efi" ]]; then
    echo "== fetching the pinned shim packages"
    tools/shim-spike/fetch-shims.sh
fi

mkdir -p "$OUT"
rm -f "$IMG"

# The passphrase reaches mkstick in a file rather than on its command line,
# for the same reason the appliance pipes it to cryptsetup: an argument vector
# is world-readable through /proc while the process runs.
umask 077
printf '%s' "$PASS" > "$PASSFILE"

# Both UKIs are signed by mkstick with the stick's own new MOK on the way in.
# The appliance is listed first so it is the first entry in the loader's UKI
# chooser; the fixture is there so the chooser exists at all, which is the
# shape DESIGN Section 7.5 describes (personal OS + signing appliance).
UKI_ARGS=(--uki "$APPLIANCE_UKI")
if [[ -f "$FIXTURE_UKI" ]]; then
    UKI_ARGS+=(--uki "$FIXTURE_UKI")
else
    echo "note: $FIXTURE_UKI missing; the chooser will hold one entry" >&2
fi

echo "== mkstick create"
sudo "$MKSTICK" create "$IMG" \
    --force \
    --size 4G \
    --p1-size 64M \
    --iso-size 2G \
    --p3-size 1G \
    --p4-size rest \
    --passphrase-file "$PASSFILE" \
    --shim "$SHIM_DIR" \
    --loader "$LOADER" \
    "${UKI_ARGS[@]}" \
    --cn "airies-boot appliance rig" \
    --cert-out "$CERT"
sudo chown "$(id -u):$(id -g)" "$IMG" "$CERT"
rm -f "$PASSFILE"

# ---- the ISOs ----
#
# Staged after the fact because mkstick creates the library, it does not fill
# it: filling it is what the user does by dragging files onto p2, and that is
# exactly the state the appliance exists to deal with.
echo "== staging ISOs on p2"
LOOP=$(sudo losetup --show -f -P "$IMG")
sudo udevadm settle 2>/dev/null || sleep 1
MNT=$(mktemp -d)
sudo mount -o uid="$(id -u)",gid="$(id -g)" "${LOOP}p2" "$MNT"
cp "$ARCH_ISO" "$MNT/$(basename "$ARCH_ISO")"
if [[ -f "$PAYLOAD_ISO" ]]; then
    # A real El Torito ISO with no Linux kernel in it. The appliance must list
    # it like any other pending ISO and must not sign it unless it is
    # confirmed, which is what makes it the skip case in the proof.
    cp "$PAYLOAD_ISO" "$MNT/payload-test.iso"
fi
sync
ls -l "$MNT"
sudo umount "$MNT"
rmdir "$MNT"
MNT=
sudo losetup -d "$LOOP"
LOOP=

# ---- the varstore ----
echo "== seeding a varstore that trusts this stick's MOK"
"$MKSTICK" varstore "$CERT" "$VARS"

echo
echo "created $IMG"
echo "  passphrase: $PASS  (override with AIRIES_TEST_PASS)"
echo "  cert:       $CERT"
echo "  varstore:   $VARS"
echo
echo "run the proof with: tools/appliance/run-appliance-proof.sh"
