#!/usr/bin/env bash
# falcon-boot: build the stick the signing-appliance proof boots
# (docs/DESIGN.md Section 7.5).
#
# Unlike falcon-efi/tools/mk-test-stick.sh, this one is built by the PRODUCT
# tool: `falcon-boot-mkstick create` partitions it, generates a real MOK
# keypair, signs the loader and both UKIs with it, and puts the private key on
# p3 inside the LUKS volume. That matters for this proof specifically: the
# appliance's whole job is to read that key and sign with it, so a rig that
# handed it a key some shell script had placed would be testing the shell
# script.
#
#   p1  ESP        64M  shim -> our signed loader
#   p2  exFAT       2G  two pending ISOs, one at the root and one in a folder:
#                         /arch-mini-root.iso
#                         /Linux/arch-mini-linux.iso
#                       Both are the same small archiso-layout fixture, so both
#                       are really signable; the folder is what gives the
#                       appliance's grouped listing (DESIGN Section 7.5) and the
#                       loader's folder view (Section 6 stage 1) a real case,
#                       and signing one leaves the other pending, which is what
#                       the proof's second run reads back.
#   p3  LUKS2+ext4  1G  falcon-mok.key, /uki/appliance-uki.efi, /uki/fixture-uki.efi
#   p4  LUKS2      rest untouched
#
# Outputs:
#   build/appliance/appliance-stick.img          the stick
#   build/appliance/appliance-mok.crt            the MOK cert mkstick generated
#   build/appliance/OVMF_VARS.secboot.mok.fd     varstore trusting that cert
#
# Usage: falcon-appliance/tools/mk-test-appliance-stick.sh
# Env:   FALCON_TEST_PASS  p3/p4 passphrase (default "falcon-test")
#
# THIS IS RIG SCAFFOLDING in one respect only: the passphrase is a well-known
# string and the varstore skips the interactive MokManager enrollment a real
# machine does once. The stick itself, the keypair and the signatures are the
# product's.

set -euo pipefail

cd "$(dirname "$0")/../.."
. tools/rig-part.sh

APPLIANCE_ARCH="${APPLIANCE_ARCH:-x64}"
OUT=build/appliance
if [[ $APPLIANCE_ARCH == aa64 ]]; then
    # The same stick for an AArch64 machine: the AArch64 appliance and
    # fixture UKI on p3, and ISOs shaped like Fedora's arm64 media (the
    # kernel at /images/pxeboot/vmlinuz), since Arch ships none.
    SUFFIX=-aa64
    VARS="$OUT/AAVMF_VARS.secboot.mok.fd"
    VARS_TEMPLATE=build/mok-test/AAVMF_VARS.secboot.mok.fd
    FIXTURE_KERNEL=build/fedora-kernel/aarch64/vmlinuz
    FIXTURE_INITRD=build/test/fixture-initrd-aa64.cpio
else
    SUFFIX=
    VARS="$OUT/OVMF_VARS.secboot.mok.fd"
    VARS_TEMPLATE=
    FIXTURE_KERNEL=build/test/fixture-vmlinuz
    FIXTURE_INITRD=build/test/fixture-initrd.cpio
fi
IMG="$OUT/appliance-stick$SUFFIX.img"
CERT="$OUT/appliance-mok$SUFFIX.crt"
PASSFILE="$OUT/passphrase"

PASS="${FALCON_TEST_PASS:-falcon-test}"

# The pending library. One small archiso-layout ISO, staged twice under two
# names: a 17 MiB fixture keeps the proof's signing leg to seconds instead of
# the minutes a 1.4 GiB distro ISO spends being hashed, and `sign` treats it as
# the real thing (falcon-core/src/media/family.c fingerprints /arch/boot/x86_64,
# and nothing here passes --kernel, so family detection runs for real).
MINI_ISO="$OUT/arch-mini.iso"
MINI_STAGE="$OUT/arch-mini-root"
ROOT_ISO_NAME=arch-mini-root.iso
FOLDER=Linux
FOLDER_ISO_NAME=arch-mini-linux.iso

ISO_SIZE=2G
TOTAL_SIZE=4G

MKSTICK=build/bin/falcon-boot-mkstick
APPLIANCE_UKI="$OUT/appliance-uki$SUFFIX.efi"
FIXTURE_UKI=build/test/fixture-uki$SUFFIX.efi
LOADER=build/bin/falcon-boot.efi
SHIM_DIR=build/shim/bins

LOOP=
MNT=

# `losetup -d` is deferred while anything still holds the device and reports
# success either way. Left fire-and-forget, the loop number is handed straight
# back to whatever runs next - the acceptance run, usually - which then finds
# it tearing down under its own partition scan. Waited out here instead.
detach_loop() {
    local l="$1" i

    [[ -n "$l" ]] || return 0
    for i in 1 2 3 4 5 6 7 8 9 10; do
        sudo losetup -d "$l" 2>/dev/null || true
        losetup -a 2>/dev/null | grep -q "^$l:" || return 0
        sleep 0.5
    done
    echo "note: $l would not detach" >&2
}

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
        MNT=
    fi
    if [[ -n "$LOOP" ]]; then
        detach_loop "$LOOP"
        LOOP=
    fi
    rm -f "$PASSFILE"
}
trap cleanup EXIT

[[ -x "$MKSTICK" ]] || { echo "ERROR: $MKSTICK not built (run: make cli)" >&2; exit 1; }
[[ -f "$APPLIANCE_UKI" ]] || {
    echo "ERROR: $APPLIANCE_UKI missing (run: falcon-appliance/image/mk-appliance.sh)" >&2
    exit 1
}
[[ -f "$LOADER" ]] || { echo "ERROR: $LOADER not built (run: make)" >&2; exit 1; }
[[ -f "$FIXTURE_KERNEL" && -f "$FIXTURE_INITRD" ]] || {
    echo "ERROR: fixture kernel/initramfs missing (run: make test-uki)" >&2
    exit 1
}
command -v xorriso >/dev/null 2>&1 || { echo "ERROR: xorriso not found" >&2; exit 1; }
if [[ ! -f "$SHIM_DIR/fedora-16.1-shimx64.efi" ]]; then
    echo "== fetching the pinned shim packages"
    falcon-efi/tools/shim-spike/fetch-shims.sh
fi

mkdir -p "$OUT"
rm -f "$IMG"

# The passphrase reaches mkstick in a file rather than on its command line:
# an argument vector is world-readable through /proc while the process
# runs.
umask 077
printf '%s' "$PASS" > "$PASSFILE"

# Both UKIs are signed by mkstick with the stick's own new MOK on the way in:
# the appliance through --appliance (or --appliance-aa64), as every stick
# gets one, and the fixture so the chooser exists at all, which is the shape
# DESIGN Section 7.5 describes (personal OS + signing appliance).
if [[ -n "$SUFFIX" ]]; then
    UKI_ARGS=(--appliance-aa64 "$APPLIANCE_UKI")
else
    UKI_ARGS=(--appliance "$APPLIANCE_UKI")
fi
if [[ -f "$FIXTURE_UKI" ]]; then
    UKI_ARGS+=(--uki "$FIXTURE_UKI")
else
    echo "note: $FIXTURE_UKI missing; the chooser will hold one entry" >&2
fi

echo "== mkstick create"
sudo "$MKSTICK" create "$IMG" \
    --force \
    --size "$TOTAL_SIZE" \
    --p1-size 64M \
    --iso-size "$ISO_SIZE" \
    --p3-size 1G \
    --p4-size rest \
    --passphrase-file "$PASSFILE" \
    --shim "$SHIM_DIR" \
    --loader "$LOADER" \
    "${UKI_ARGS[@]}" \
    --cn "falcon-boot appliance rig" \
    --cert-out "$CERT"
sudo chown "$(id -u):$(id -g)" "$IMG" "$CERT"
rm -f "$PASSFILE"

# ---- the ISOs ----
#
# -r for Rock Ridge so the names inside survive verbatim: bsdtar (which is how
# the bless tool reads an ISO) would otherwise see ISO9660's uppercased,
# ";1"-suffixed forms and the /arch/boot/x86_64 fingerprint would not match.
rm -rf "$MINI_STAGE" "$MINI_ISO"
if [[ $APPLIANCE_ARCH == aa64 ]]; then
    echo "== building $MINI_ISO (arm64 kernel + initramfs, Fedora's layout)"
    mkdir -p "$MINI_STAGE/images/pxeboot" "$MINI_STAGE/LiveOS"
    cp "$FIXTURE_KERNEL" "$MINI_STAGE/images/pxeboot/vmlinuz"
    cp "$FIXTURE_INITRD" "$MINI_STAGE/images/pxeboot/initrd.img"
    printf 'fixture\n' > "$MINI_STAGE/LiveOS/README"
else
    echo "== building $MINI_ISO (fixture kernel + initramfs, archiso layout)"
    mkdir -p "$MINI_STAGE/arch/boot/x86_64"
    cp "$FIXTURE_KERNEL" "$MINI_STAGE/arch/boot/x86_64/vmlinuz-linux"
    cp "$FIXTURE_INITRD" "$MINI_STAGE/arch/boot/x86_64/initramfs-linux.img"
fi
xorriso -as mkisofs -r -J -V FALCON_MINI -o "$MINI_ISO" "$MINI_STAGE" \
    >/dev/null 2>&1
rm -rf "$MINI_STAGE"
echo "   $MINI_ISO ($(stat -c%s "$MINI_ISO") bytes)"

# Staged after the fact because mkstick creates the library, it does not fill
# it: filling it is what the user does by dragging files onto p2, and that is
# exactly the state the appliance exists to deal with. Two copies under two
# names, one of them in a folder, because "which of these do I vouch for" is
# only a real question when there is more than one and they are not all in the
# same place.
echo "== staging ISOs on p2"
P2N=$(rig_part "$IMG" library)
LOOP=$(sudo losetup --show -f -P "$IMG")
sudo udevadm settle 2>/dev/null || sleep 1
MNT=$(mktemp -d)
sudo mount -o uid="$(id -u)",gid="$(id -g)" "${LOOP}p$P2N" "$MNT"
cp "$MINI_ISO" "$MNT/$ROOT_ISO_NAME"
mkdir -p "$MNT/$FOLDER"
cp "$MINI_ISO" "$MNT/$FOLDER/$FOLDER_ISO_NAME"
sync
ls -lR "$MNT"
sudo umount "$MNT"
rmdir "$MNT"
MNT=
detach_loop "$LOOP"
LOOP=

# ---- the varstore ----
echo "== seeding a varstore that trusts this stick's MOK"
# Gentoo ships an enrolled OVMF_VARS.secboot.fd; Arch ships none, and there the
# rig's own Microsoft-enrolled varstore (falcon-efi/tools/mk-test-mok.sh) is
# the template.
TEMPLATE=()
if [[ -n $VARS_TEMPLATE ]]; then
    TEMPLATE=(--template "$VARS_TEMPLATE")
elif [[ ! -f "${OVMF_DIR:-/usr/share/edk2/OvmfX64}/OVMF_VARS.secboot.fd" &&
      -f build/mok-test/OVMF_VARS.secboot.mok.fd ]]; then
    TEMPLATE=(--template build/mok-test/OVMF_VARS.secboot.mok.fd)
fi
"$MKSTICK" varstore "${TEMPLATE[@]}" "$CERT" "$VARS"

echo
echo "created $IMG"
echo "  passphrase: $PASS  (override with FALCON_TEST_PASS)"
echo "  cert:       $CERT"
echo "  varstore:   $VARS"
echo
echo "run the proof with: falcon-appliance/tools/run-appliance-proof.sh"
