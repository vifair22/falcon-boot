#!/usr/bin/env bash
# falcon-boot: build the fixture Unified Kernel Image the rig boots
# (docs/DESIGN.md Sections 5.1, 10 and 11 step 6).
#
# Produces a real sd-stub UKI, because the thing under test is sd-stub's
# companion-credential pickup and nothing else can stand in for it: the loader
# publishes a synthetic filesystem carrying the image plus
# `<uki>.efi.extra.d/cryptsetup.passphrase.cred`, and this image's sd-stub is
# what has to find that directory, pack it into a cpio and merge it into the
# initrd. The tiny /init then prints what arrived.
#
#   build/test/fixture-uki.efi          unsigned
#   build/test/fixture-uki-signed.efi   signed with the throwaway test MOK
#
# Ingredients:
#   kernel   /arch/boot/x86_64/vmlinuz-linux out of an Arch ISO, read with
#            bsdtar (ISO9660). The ISO is READ ONLY and never modified.
#   initrd   one static binary as /init (falcon-efi/tools/uki-init/init.c),
#            packed newc. No busybox on this machine and none wanted: the test
#            needs a program that lists a directory and powers off.
#   stub     /usr/lib/systemd/boot/efi/linuxx64.efi.stub (systemd 261)
#   cmdline  console=ttyS0 console=tty0: serial for the headless rig, tty0
#            last so /dev/console (what init prints to) is the SCREEN and a
#            human watching the QEMU window sees the result; the kernel still
#            mirrors its own messages to both.
#
# Usage: falcon-efi/tools/mk-test-uki.sh
# Env:   ARCH_ISO   source ISO (default: the one on this stick's p2;
#                   tools/arch-iso.sh)
#        UKI_KERNEL pre-extracted kernel, skips the ISO entirely
#
# THIS IS RIG SCAFFOLDING. The product's UKI is built by the host tooling of
# DESIGN Section 8 from the user's own kernel and initramfs, and signed with a
# real MOK whose private key lives encrypted on p3. Everything here lands under
# build/ and the passphrase it expects is a well-known string on purpose.

set -euo pipefail

cd "$(dirname "$0")/../.."

. tools/arch-iso.sh
trap arch_iso_release EXIT
ISO_KERNEL_PATH=arch/boot/x86_64/vmlinuz-linux
STUB=/usr/lib/systemd/boot/efi/linuxx64.efi.stub

OUT=build/test
WORK="$OUT/uki-work"
KERNEL="${UKI_KERNEL:-$OUT/fixture-vmlinuz}"
INITRD="$OUT/fixture-initrd.cpio"
UKI="$OUT/fixture-uki.efi"
UKI_SIGNED="$OUT/fixture-uki-signed.efi"

MOK_KEY=build/mok-test/falcon-test-mok.key
MOK_CRT=build/mok-test/falcon-test-mok.crt

CMDLINE="console=ttyS0,115200 console=tty0 panic=10 rd.systemd.unit=none"

for t in bsdtar cpio gcc ukify; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done
[[ -f "$STUB" ]] || {
    echo "ERROR: sd-stub not found at $STUB (install systemd-boot)" >&2
    exit 1
}

mkdir -p "$OUT"
rm -rf "$WORK"
mkdir -p "$WORK/root/dev"

# ---- kernel ----
#
# bsdtar reads ISO9660 directly, so no loop mount and no sudo. Extracting to a
# scratch directory keeps the one write anywhere near the ISO on our side of
# the fence.
if [[ ! -f "$KERNEL" ]]; then
    arch_iso_resolve
    [[ -f "$ARCH_ISO" ]] || {
        echo "ERROR: $ARCH_ISO not found (set ARCH_ISO or UKI_KERNEL)" >&2
        exit 1
    }
    echo "== extracting $ISO_KERNEL_PATH from $(basename "$ARCH_ISO")"
    bsdtar -x -f "$ARCH_ISO" -C "$WORK" "$ISO_KERNEL_PATH"
    cp "$WORK/$ISO_KERNEL_PATH" "$KERNEL"
fi
echo "   kernel: $KERNEL ($(stat -c%s "$KERNEL") bytes)"

# ---- initrd ----
#
# Static because it is PID 1 in an initramfs that contains exactly one file;
# there is no dynamic loader in there to find a library with.
echo "== building the fixture init"
gcc -std=c11 -O2 -static -Wall -Wextra -o "$WORK/root/init" falcon-efi/tools/uki-init/init.c

# Uncompressed newc. The kernel sniffs each initramfs segment for a compression
# magic and takes it raw when there is none, and an extra megabyte costs
# nothing in a rig.
( cd "$WORK/root" && find . -mindepth 1 -printf '%P\n' | LC_ALL=C sort |
    cpio --quiet -o -H newc ) > "$INITRD"
echo "   initrd: $INITRD ($(stat -c%s "$INITRD") bytes)"

# ---- the UKI ----
#
# No --os-release and no --uname is not the same as no sections: ukify 261
# autodetects the version out of the kernel and copies the BUILD HOST's
# /etc/os-release in, so this fixture carries .uname and a PRETTY_NAME of
# "Arch Linux" whether or not we ask for them. Left alone deliberately, because
# that is what the product's own UKIs carry too, but it does mean personal.efi
# and unsigned.efi are a label COLLISION on the rig's shelf rather than the two
# unlabelled rows the comment here used to claim.
echo "== ukify build"
ukify build \
    --linux="$KERNEL" \
    --initrd="$INITRD" \
    --cmdline="$CMDLINE" \
    --stub="$STUB" \
    --output="$UKI" >/dev/null
echo "   unsigned UKI: $UKI ($(stat -c%s "$UKI") bytes)"

# ---- signature ----
#
# Signed with the rig's throwaway MOK, which is what makes the Secure Boot ON
# half of the test run at all: shim verifies against the MokList the varstore
# was seeded with (falcon-efi/tools/mk-test-mok.sh).
rm -f "$UKI_SIGNED"
if [[ -f "$MOK_KEY" && -f "$MOK_CRT" ]]; then
    sbsign --key "$MOK_KEY" --cert "$MOK_CRT" --output "$UKI_SIGNED" "$UKI" \
        >/dev/null
    echo "   signed UKI:   $UKI_SIGNED ($(stat -c%s "$UKI_SIGNED") bytes)"
else
    echo "note: no test MOK at $MOK_KEY; unsigned only" >&2
    echo "      (run 'make sign' for the Secure Boot fixture)" >&2
fi

rm -rf "$WORK"

echo
echo "expected serial output once the loader launches it:"
echo "  FALCON-UKI-INIT"
echo "  FALCON-CRED-FILE cryptsetup.passphrase.cred 11 bytes"
echo "  FALCON-CRED-OK"
