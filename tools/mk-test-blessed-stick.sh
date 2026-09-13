#!/usr/bin/env bash
# falcon-boot: add a blessed ISO (and a stale one) to the test stick, so the
# rig can boot the blessed path end to end (docs/DESIGN.md Section 8.1).
#
# What this produces, on top of whatever tools/mk-test-stick.sh already put on
# p2 of build/test-stick.img:
#
#   p2:/blessed-test.iso                an ISO carrying the fixture kernel and
#                                       initramfs, blessed
#   p2:/falcon/blessed/blessed-test.uki the signed UKI wrap the real bless tool
#                                       built out of it
#   p2:/falcon/blessed/manifest         two records: the one above, plus a
#                                       DELIBERATELY WRONG size for
#                                       payload-test.iso
#
# So the menu ends up carrying one entry of each kind, which is the point:
#
#   blessed-test.iso   [blessed]          boots the signed UKI (bless path)
#   payload-test.iso   [blessed, stale]   boots the ISO the ordinary way,
#                                         because the file no longer has the
#                                         size it had when it was blessed
#   archlinux-fake.iso                    no record at all
#
# The UKI is built by the REAL tool (build/bin/falcon-boot-sign), not by a
# hand-rolled ukify call here: the thing under test on the loader side is that
# what the bless tool writes is what the loader finds, so a fixture that wrote
# it a second way would prove nothing about the pair.
#
# Why a purpose-built ISO rather than a distro one: the bless tool extracts the
# kernel and the initramfs from inside the ISO, so the image has to contain
# them, and the fixture kernel plus the one-binary initramfs of
# tools/mk-test-uki.sh are the pair whose boot prints markers the rig can
# grep for. A real Arch ISO works too (bless it by hand with
# tools/mk-test-bless.sh), but its initramfs looks for real media and a
# headless rig cannot tell a successful boot from a stuck one.
#
# THIS IS RIG SCAFFOLDING. It signs with the throwaway MOK from
# tools/mk-test-mok.sh, whose private key sits in the clear under build/, and
# that is not the product's key model (DESIGN Section 7.5 keeps the real
# private key encrypted on p3). Everything lands under build/.
#
# Usage: tools/mk-test-blessed-stick.sh
# Env:   STICK_IMG  the stick to modify (default: build/test-stick.img)
#        SIGN       the bless tool (default: build/bin/falcon-boot-sign)
#
# Needs xorriso, sbsign/sbverify, ukify, losetup and passwordless sudo.

set -euo pipefail

cd "$(dirname "$0")/.."

STICK_IMG="${STICK_IMG:-build/test-stick.img}"
SIGN="${SIGN:-build/bin/falcon-boot-sign}"

KERNEL=build/test/fixture-vmlinuz
INITRD=build/test/fixture-initrd.cpio
MOK_KEY=build/mok-test/falcon-test-mok.key
MOK_CRT=build/mok-test/falcon-test-mok.crt

ISO=build/test-blessed.iso
STAGE=build/test/blessed-isoroot
ISO_NAME=blessed-test.iso
UKI_NAME=blessed-test.uki

# Where the kernel and its initramfs sit inside the ISO. The layout is Arch's,
# so the ISO looks like the thing the bless tool was written against; the
# --kernel override below names it explicitly anyway, so nothing depends on
# family detection guessing right.
ISO_KERNEL_DIR=arch/boot/x86_64
ISO_KERNEL="$ISO_KERNEL_DIR/vmlinuz-linux"

# console=ttyS0 so the whole blessed boot lands in the rig's serial log, and
# rd.systemd.unit=none for the same reason tools/mk-test-uki.sh uses it: the
# fixture init is PID 1 and there is no systemd in there to hand over to.
CMDLINE="console=ttyS0,115200 panic=10 rd.systemd.unit=none"

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
}
trap cleanup EXIT

for t in xorriso losetup sbverify ukify bsdtar; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done
[[ -x "$SIGN" ]] || {
    echo "ERROR: $SIGN not built (run: make cli)" >&2
    exit 1
}
[[ -f "$STICK_IMG" ]] || {
    echo "ERROR: $STICK_IMG not built (run: make test-stick)" >&2
    exit 1
}
[[ -f "$KERNEL" && -f "$INITRD" ]] || {
    echo "ERROR: fixture kernel/initramfs missing (run: make test-uki)" >&2
    exit 1
}
[[ -f "$MOK_KEY" && -f "$MOK_CRT" ]] || {
    echo "ERROR: no test MOK; run tools/mk-test-mok.sh first" >&2
    exit 1
}

# ---- the ISO to bless ----
#
# -r for Rock Ridge so the names survive verbatim: bsdtar (which is how the
# bless tool reads the ISO) would otherwise see ISO9660's uppercased,
# ";1"-suffixed forms and the --kernel path would not match.
echo "== building $ISO (fixture kernel + initramfs, ISO9660)"
rm -rf "$STAGE"
mkdir -p "$STAGE/$ISO_KERNEL_DIR"
cp "$KERNEL" "$STAGE/$ISO_KERNEL"
cp "$INITRD" "$STAGE/$ISO_KERNEL_DIR/initramfs-linux.img"
rm -f "$ISO"
xorriso -as mkisofs -r -J -V FALCON_BLESSED -o "$ISO" "$STAGE" >/dev/null 2>&1
rm -rf "$STAGE"
echo "   $ISO ($(stat -c%s "$ISO") bytes)"

# ---- p2 ----
LOOP=$(sudo losetup --show -f -P "$STICK_IMG")
sudo udevadm settle 2>/dev/null || sleep 1
MNT=$(mktemp -d)
sudo mount -o uid="$(id -u)",gid="$(id -g)" "${LOOP}p2" "$MNT"

echo
echo "== staging $ISO_NAME on p2"
cp "$ISO" "$MNT/$ISO_NAME"

# --kernel and --cmdline are both explicit so the bless does not depend on the
# family registry recognising a fixture ISO: an unknown family is exactly the
# case those two flags exist for, and a known one takes them just as well.
# Embedding the initramfs is the tool's default (DESIGN Section 8.1): the
# blessed boot chain is signed end to end, and without an initramfs the
# fixture kernel would panic instead of printing anything.
echo
echo "== bless (the real tool, the test MOK)"
"$SIGN" "$MNT/$ISO_NAME" \
    --stick "$MNT" \
    --key "$MOK_KEY" --cert "$MOK_CRT" \
    --name "$UKI_NAME" \
    --kernel "$ISO_KERNEL" \
    --cmdline "$CMDLINE"

MANIFEST="$MNT/falcon/blessed/manifest"
UKI="$MNT/falcon/blessed/$UKI_NAME"

[[ -f "$UKI" ]] || { echo "FAIL: $UKI was not written" >&2; exit 1; }
sbverify --cert "$MOK_CRT" "$UKI" >/dev/null
echo "   $UKI_NAME verifies against the test MOK"

# ---- the stale case ----
#
# Appended after the bless, because the tool rewrites the manifest from the
# records it parsed and would drop anything added before it ran.
#
# The size is deliberately one byte off the real one. That is the whole of
# Section 8.1's staleness rule: the path still matches, the size does not, so
# the loader must label the entry stale and boot it the ordinary way. It
# points at the same UKI on purpose, to prove the loader never reaches for it:
# a stale entry that launched the wrap anyway would look like it worked.
if [[ -f "$MNT/payload-test.iso" ]]; then
    STALE_REAL=$(stat -c%s "$MNT/payload-test.iso")
    STALE_WRONG=$((STALE_REAL + 1))
    {
        echo
        echo "# staleness fixture: payload-test.iso really is $STALE_REAL"
        echo "# bytes, so this record can never be fresh."
        echo "iso=payload-test.iso"
        echo "size=$STALE_WRONG"
        echo "uki=$UKI_NAME"
        echo "family=unknown"
    } >> "$MANIFEST"
    echo "   stale record added for payload-test.iso"
    echo "   (real size $STALE_REAL, manifest says $STALE_WRONG)"
else
    echo "note: payload-test.iso is not on p2; no stale entry staged" >&2
    echo "      (run 'make test-iso test-stick' first)" >&2
fi

echo
echo "== manifest"
sed 's/^/   /' "$MANIFEST"

echo
echo "== p2 usage"
df -h "$MNT" | sed 's/^/   /'

sync
sudo umount "$MNT"
rmdir "$MNT"
MNT=
sudo losetup -d "$LOOP"
LOOP=

cat <<EOF

OK. $STICK_IMG now carries a blessed entry and a stale one.

Run it:  tools/run-qemu.sh --stick            (Secure Boot off)
         tools/run-qemu.sh --stick --secboot  (Secure Boot on, the real case)

Expected menu labels:
  blessed-test.iso   ...  [blessed]
  payload-test.iso   ...  [blessed, stale]
  archlinux-fake.iso ...                  (no record)

Expected serial output, picking blessed-test.iso:
  FALCON-UKI-INIT
  FALCON-CRED-DIR-MISSING /.extra/credentials
  FALCON-CRED-FAIL
  FALCON-UKI-DONE
then the machine powers off. The two CRED lines are the SUCCESS case here:
the blessed path publishes the image and no credential (the wrap carries its
own initrd and cmdline), so the fixture init finds no credential directory
and says so. Reaching FALCON-UKI-DONE means the wrap was loaded, verified by
shim under --secboot, started, and ran its initramfs.

Expected serial output, picking payload-test.iso (the stale one):
  FALCON-PAYLOAD-OK
i.e. exactly the ordinary ISO path, because a stale blessing boots as if it
were not there at all. Nothing should mention the UKI.

Expected output if the UKI is refused (shim without this MOK enrolled, which
you can reproduce by booting the plain OVMF varstore with --secboot):
  boot failed while LoadImage of the blessed UKI (shim): 0x800000000000001a
  the blessed image is not signed with a key this machine has enrolled.
and the menu comes back. It must NOT fall through to booting the ISO the
ordinary way: that is the path the user blessed the ISO to avoid.
EOF
