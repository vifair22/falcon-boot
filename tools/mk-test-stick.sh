#!/usr/bin/env bash
# airies-boot: build the four-partition test stick the QEMU rig boots
# (docs/DESIGN.md Sections 3 and 10).
#
# Produces build/test-stick.img: a REAL GPT disk with the Section 3 layout,
# shrunk to a size the rig can rebuild in seconds. It is attached to QEMU as
# an ordinary raw drive, not through vvfat, so the firmware does its own GPT
# parsing and the loader sees the same per-partition Block I/O handles it will
# see on hardware.
#
#   p1  ESP   FAT32   64 MiB  shim -> mmx64.efi -> our loader as grubx64.efi
#   p2  ISOs  exFAT   64 MiB  two fake ISOs, plus the real payload ISO if built
#   p3  UKI   LUKS2   64 MiB  pbkdf2 keyslot, ext4 inside, /uki/*.efi
#   p4  root  LUKS2   32 MiB  argon2id keyslot, deliberately empty
#
# p3's keyslot is pbkdf2 with a pinned iteration count because DESIGN Section 3
# mandates it: argon2id is memory-hard and brutal single-threaded in EFI-land.
# p4 is here precisely so the loader's probe-and-skip is tested honestly rather
# than assumed: it is a genuine LUKS2 volume that our in-EFI unlock cannot open,
# so the scan has to recognise it and leave it alone without prompting for it.
#
# /uki holds two images so both halves of the Secure Boot story are in one
# fixture: personal.efi signed with the test MOK (the one that must boot) and
# unsigned.efi identical but unsigned (the one shim must refuse). A non-.efi
# decoy sits beside them so the discovery filter is exercised too.
#
# Usage: tools/mk-test-stick.sh [--unsigned]
#   --unsigned   stage the unsigned loader as the shim next-stage and skip
#                signing the UKI. For a plain-OVMF run when no test MOK exists.
#
# Env: AIRIES_TEST_PASS  passphrase for p3 and p4 (default "airies-test").
#
# THIS IS RIG SCAFFOLDING. The product's stick is built by the host tooling of
# DESIGN Section 8 (step 7), which generates a real MOK keypair and keeps the
# private key encrypted on p3. Nothing here is that; everything lands under
# build/ and the passphrase is a well-known string on purpose.
#
# Needs sgdisk, mkfs.fat, mkfs.exfat, cryptsetup, mkfs.ext4 and passwordless
# sudo (loop devices and device-mapper).

set -euo pipefail

cd "$(dirname "$0")/.."

IMG=build/test-stick.img
SIZE_MIB=256
PASS="${AIRIES_TEST_PASS:-airies-test}"

# DESIGN Section 3 mandates pbkdf2 on p3. The iteration count is pinned rather
# than benchmarked: cryptsetup's own benchmark picks millions, which is seconds
# of unlock in unoptimised freestanding C. 100k is the rig's working number;
# Section 8 records the shipped one once the EFI implementation is measured.
P3_ITER=100000

SHIM_BINS=build/shim/bins
SHIM_EFI="$SHIM_BINS/fedora-16.1-shimx64.efi"
MM_EFI="$SHIM_BINS/fedora-16.1-mmx64.efi"
EFI_PLAIN=build/bin/airies-boot.efi
EFI_SIGNED=build/bin/airies-boot-signed.efi
MOK_KEY=build/mok-test/airies-test-mok.key
MOK_CRT=build/mok-test/airies-test-mok.crt
MOK_CER=build/mok-test/airies-test-mok.cer
PAYLOAD=build/test/payload.efi
PAYLOAD_ISO=build/test-payload.iso

UNSIGNED=0
[[ "${1:-}" == "--unsigned" ]] && UNSIGNED=1

LOOP=
MAPPER=
MNT=

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
        MNT=
    fi
    if [[ -n "$MAPPER" ]]; then
        sudo cryptsetup close "$MAPPER" 2>/dev/null || true
        MAPPER=
    fi
    if [[ -n "$LOOP" ]]; then
        sudo losetup -d "$LOOP" 2>/dev/null || true
        LOOP=
    fi
}
trap cleanup EXIT

for t in sgdisk mkfs.fat mkfs.exfat mkfs.ext4 cryptsetup losetup; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done

# The loader is what the whole stick exists to boot, so its absence is fatal
# rather than a degraded fixture.
[[ -f "$EFI_PLAIN" ]] || {
    echo "ERROR: $EFI_PLAIN not built (run: make)" >&2
    exit 1
}
[[ -f "$PAYLOAD" ]] || {
    echo "ERROR: $PAYLOAD not built (run: make payload)" >&2
    exit 1
}

if [[ "$UNSIGNED" -eq 0 && ! -f "$MOK_KEY" ]]; then
    echo "note: no test MOK at $MOK_KEY; falling back to --unsigned" >&2
    echo "      (run 'make sign' for the Secure Boot fixture)" >&2
    UNSIGNED=1
fi
if [[ "$UNSIGNED" -eq 0 && ! -f "$EFI_SIGNED" ]]; then
    echo "ERROR: $EFI_SIGNED missing (run: make sign)" >&2
    exit 1
fi
if [[ "$UNSIGNED" -eq 0 && ( ! -f "$SHIM_EFI" || ! -f "$MM_EFI" ) ]]; then
    echo "== fetching the pinned shim packages"
    tools/shim-spike/fetch-shims.sh
fi

mkdir -p build
rm -f "$IMG"
truncate -s "${SIZE_MIB}M" "$IMG"

# One MiB alignment throughout, which is both what real media wants and what
# keeps every partition start a whole number of 4 KiB filesystem blocks.
echo "== partitioning (GPT, four partitions per DESIGN Section 3)"
sgdisk \
    -n 1:1MiB:+64MiB   -t 1:ef00 -c 1:"AIRIES-ESP" \
    -n 2:0:+64MiB      -t 2:0700 -c 2:"AIRIES-ISO" \
    -n 3:0:+64MiB      -t 3:8309 -c 3:"AIRIES-UKI" \
    -n 4:0:+32MiB      -t 4:8309 -c 4:"AIRIES-ROOT" \
    "$IMG" >/dev/null

LOOP=$(sudo losetup --show -f -P "$IMG")
P1="${LOOP}p1"; P2="${LOOP}p2"; P3="${LOOP}p3"; P4="${LOOP}p4"
# udev can take a moment to publish the partition nodes; without this the
# first mkfs races it and fails with a confusing ENOENT.
sudo udevadm settle 2>/dev/null || sleep 1

# FAT and exFAT have no on-disk ownership, so they synthesize it from mount
# options and the population below needs no further sudo.
mount_rw() {
    MNT=$(mktemp -d)
    sudo mount -o uid="$(id -u)",gid="$(id -g)" "$1" "$MNT"
}

# ext4 does have on-disk ownership and rejects uid=/gid= outright, so the
# mount point is chowned once instead.
mount_rw_owned() {
    MNT=$(mktemp -d)
    sudo mount "$1" "$MNT"
    sudo chown "$(id -u):$(id -g)" "$MNT"
}

unmount_rw() {
    sync
    sudo umount "$MNT"
    rmdir "$MNT"
    MNT=
}

# ---- p1: the ESP and the Secure Boot chain ----
#
# Same staging as tools/run-qemu.sh --secboot, but on a real FAT32 partition
# rather than a vvfat directory: BOOTX64.EFI is Fedora's shim, and our loader
# goes under grubx64.efi because that is the next-stage name compiled into it.
echo "== p1: FAT32 ESP"
sudo mkfs.fat -F 32 -n AIRIESESP "$P1" >/dev/null
mount_rw "$P1"
mkdir -p "$MNT/EFI/BOOT"
if [[ "$UNSIGNED" -eq 1 ]]; then
    cp "$EFI_PLAIN" "$MNT/EFI/BOOT/BOOTX64.EFI"
    echo "   unsigned loader staged directly as BOOTX64.EFI (no shim)"
else
    cp "$SHIM_EFI" "$MNT/EFI/BOOT/BOOTX64.EFI"
    cp "$MM_EFI" "$MNT/EFI/BOOT/mmx64.efi"
    cp "$EFI_SIGNED" "$MNT/EFI/BOOT/grubx64.efi"
    # DESIGN Section 4: the public cert travels on p1 for enrollment. The
    # private key does not travel; it is not on this partition and never is.
    [[ -f "$MOK_CER" ]] && cp "$MOK_CER" "$MNT/airies-boot.cer"
    echo "   shim -> mmx64.efi -> grubx64.efi (our signed loader)"
fi
unmount_rw

# ---- p2: the exFAT ISO library ----
echo "== p2: exFAT ISO library"
sudo mkfs.exfat -n AIRIES-ISO "$P2" >/dev/null
mount_rw "$P2"
dd if=/dev/zero of="$MNT/archlinux-fake.iso" bs=1M count=1 status=none
dd if=/dev/zero of="$MNT/systemrescue-fake.iso" bs=1M count=1 status=none
if [[ -f "$PAYLOAD_ISO" ]]; then
    cp "$PAYLOAD_ISO" "$MNT/payload-test.iso"
    echo "   payload-test.iso staged (real El Torito, bootable)"
fi
unmount_rw

# ---- p3: LUKS2 with a pbkdf2 keyslot, ext4 inside ----
echo "== p3: LUKS2 (pbkdf2, $P3_ITER iterations) + ext4"
printf '%s' "$PASS" | sudo cryptsetup luksFormat \
    --type luks2 \
    --pbkdf pbkdf2 --pbkdf-force-iterations "$P3_ITER" \
    --cipher aes-xts-plain64 --key-size 512 \
    --batch-mode "$P3" -
MAPPER=airies-test-p3
printf '%s' "$PASS" | sudo cryptsetup open "$P3" "$MAPPER" -
sudo mkfs.ext4 -q -L AIRIES-UKI "/dev/mapper/$MAPPER"
mount_rw_owned "/dev/mapper/$MAPPER"

# The UKI stand-in. Until step 6 builds a real sd-stub image, what matters is
# that these bytes are a PE the platform will judge: signed with the enrolled
# test MOK for the case that must boot, and byte-identical but unsigned for the
# case shim must refuse.
mkdir -p "$MNT/uki"
if [[ "$UNSIGNED" -eq 1 ]]; then
    cp "$PAYLOAD" "$MNT/uki/personal.efi"
else
    sbsign --key "$MOK_KEY" --cert "$MOK_CRT" \
        --output "$MNT/uki/personal.efi" "$PAYLOAD" >/dev/null 2>&1
fi
cp "$PAYLOAD" "$MNT/uki/unsigned.efi"
# Decoy: proves the discovery filter takes *.efi regular files and not
# whatever else happens to be in the directory.
echo "not a kernel image" > "$MNT/uki/README.txt"
echo "this file is the decoy; the loader must never offer it" \
    > "$MNT/uki/decoy.bin"

unmount_rw
sudo cryptsetup close "$MAPPER"
MAPPER=

# ---- p4: LUKS2 argon2id, empty on purpose ----
#
# Nothing inside it. Its whole job is to be a LUKS2 volume the in-EFI unlock
# must recognise and skip, so the "argon2id is reported, never misread"
# contract in src/luks/luks.h is exercised by the rig and not just asserted.
echo "== p4: LUKS2 (argon2id, stock defaults), left empty"
printf '%s' "$PASS" | sudo cryptsetup luksFormat \
    --type luks2 --batch-mode "$P4" -

sudo losetup -d "$LOOP"
LOOP=

echo
echo "created $IMG (${SIZE_MIB} MiB, GPT, four partitions)"
echo "  passphrase: $PASS  (override with AIRIES_TEST_PASS)"
echo
echo "expected menu:"
echo "  Personal OS (encrypted)       <- pinned first, from p3's pbkdf2 slot"
echo "  archlinux-fake.iso"
if [[ -f "$PAYLOAD_ISO" ]]; then
    echo "  payload-test.iso"
fi
echo "  systemrescue-fake.iso"
echo
echo "expected /uki listing after unlock:"
if [[ "$UNSIGNED" -eq 1 ]]; then
    echo "  Personal OS: personal.efi     (unsigned; SB-off rig only)"
else
    echo "  Personal OS: personal.efi     (signed with the test MOK: boots)"
fi
echo "  Personal OS: unsigned.efi     (unsigned: refused under Secure Boot)"
echo "  README.txt / decoy.bin        present, never offered"
