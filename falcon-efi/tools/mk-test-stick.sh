#!/usr/bin/env bash
# falcon-boot: build the four-partition test stick the QEMU rig boots
# (docs/DESIGN.md Sections 3 and 10).
#
# Produces build/test-stick.img: a REAL GPT disk with the Section 3 layout,
# shrunk to a size the rig can rebuild in seconds. It is attached to QEMU as
# an ordinary raw drive, not through vvfat, so the firmware does its own GPT
# parsing and the loader sees the same per-partition Block I/O handles it will
# see on hardware.
#
#   p1  ESP   FAT32   64 MiB  shim -> mmx64.efi -> our loader as grubx64.efi;
#                             the boot record, and BIOS stage 3 in its
#                             reserved sectors
#   p2  ISOs  exFAT   64 MiB  two fake ISOs, plus the real payload ISO if built,
#                             and /.falcon (role library)
#   p3  UKI   LUKS2   64 MiB  argon2id keyslot at the stick's fixed cost and a
#                             falcon token (role shelf),
#                             ext4 inside, /uki/*.efi
#   p4  root  LUKS2   32 MiB  argon2id keyslot, deliberately empty
#   p5  BIOS  EF02            stage 2 (mkstick bios --reserve); stage 1 in LBA 0
#
# Every record carries one install ID, drawn per build (DESIGN Section 3.1),
# and the BIOS stages are put on by `mkstick bios`, so the same image boots
# under OVMF and under SeaBIOS (DESIGN Section 3.3).
#
# p3's keyslot is argon2id at the cost mkstick create gives a stick (DESIGN
# Section 3), read out of falcon-cli/src/mkstick/mkstick.h so the rig and the
# product cannot drift apart. p4 is here so the loader's scan is tested
# honestly: a genuine LUKS2 volume with no record, which the scan has to leave
# alone without prompting for it.
#
# /uki holds two images so both halves of the Secure Boot story are in one
# fixture: personal.efi signed with the test MOK (the one that must boot) and
# unsigned.efi identical but unsigned (the one shim must refuse). A non-.efi
# decoy sits beside them so the discovery filter is exercised too. Beside those
# sit the label fixtures from falcon-efi/tools/mk-test-labels.sh, which test
# what the chooser CALLS an image rather than what it loads; see that script for
# why the two need separate fixtures.
#
# What those two images ARE is the real sd-stub UKI from
# falcon-efi/tools/mk-test-uki.sh when it has been built, because step 6 of the
# build order tests the passphrase handoff and only a genuine sd-stub can
# perform the pickup. The old stand-in payload remains available behind
# --payload-uki for the cases that only need "a PE the platform will judge" and
# want a 30 KB one.
#
# Usage: falcon-efi/tools/mk-test-stick.sh [--unsigned] [--payload-uki]
#                                          [--no-labels]
#   --unsigned     stage the unsigned loader as the shim next-stage and skip
#                  signing the UKI. For a plain-OVMF run when no test MOK
#                  exists.
#   --payload-uki  stage the small stand-in payload as /uki/personal.efi
#                  instead of the sd-stub fixture UKI.
#   --no-labels    leave the label fixtures off the shelf: personal.efi and
#                  unsigned.efi alone (the core rig's sticks).
#
# Env: FALCON_TEST_PASS  passphrase for p3 and p4 (default "falcon-test").
#      TEST_STICK_IMG    the image to write (default build/test-stick.img)
#      STICK_ID          the install ID, 32 hex digits (default: drawn)
#      STICK_ISOS        the fake ISOs on p2, space-separated (default
#                        "archlinux-fake.iso systemrescue-fake.iso"); the
#                        core rig names a second stick's apart from the
#                        first's
#      P3_KDF_MEMORY_KIB p3's argon2id memory instead of the stick's (the
#                        core rig's refusal case asks for more than its
#                        machine has)
#      STICK_ISO_FILES   real images to copy onto p2 beside those,
#                        space-separated SRC:NAME pairs (the core rig's
#                        BIOS-only case)
#
# THIS IS RIG SCAFFOLDING. The product's stick is built by the host tooling of
# DESIGN Section 8 (step 7), which generates a real MOK keypair and keeps the
# private key encrypted on p3. Nothing here is that; everything lands under
# build/ and the passphrase is a well-known string on purpose.
#
# Needs sgdisk, mkfs.fat, mkfs.exfat, cryptsetup, mkfs.ext4, the mkstick
# and BIOS stage builds (make cli bios) and passwordless sudo (loop devices
# and device-mapper).

set -euo pipefail

cd "$(dirname "$0")/../.."

IMG=${TEST_STICK_IMG:-build/test-stick.img}
SIZE_MIB=256
PASS="${FALCON_TEST_PASS:-falcon-test}"

# The stick's argon2id cost, from the one place it is written.
kdf_def() {     # kdf_def NAME: its value in mkstick.h
    local v
    v=$(sed -n "s/^#define[ \t]*$1[ \t]*\([0-9]*\).*/\1/p" \
        falcon-cli/src/mkstick/mkstick.h | head -1)
    [[ -n "$v" ]] || { echo "ERROR: $1 is not in mkstick.h" >&2; exit 1; }
    echo "$v"
}
P3_KDF_MEMORY=${P3_KDF_MEMORY_KIB:-$(kdf_def MKS_STICK_KDF_MEMORY_KIB)}
P3_KDF_TIME=$(kdf_def MKS_STICK_KDF_TIME)
P3_KDF_LANES=$(kdf_def MKS_STICK_KDF_LANES)

SHIM_BINS=build/shim/bins
SHIM_EFI="$SHIM_BINS/fedora-16.1-shimx64.efi"
MM_EFI="$SHIM_BINS/fedora-16.1-mmx64.efi"
EFI_PLAIN=build/bin/falcon-boot.efi
EFI_SIGNED=build/bin/falcon-boot-signed.efi
MOK_KEY=build/mok-test/falcon-test-mok.key
MOK_CRT=build/mok-test/falcon-test-mok.crt
MOK_CER=build/mok-test/falcon-test-mok.cer
PAYLOAD=build/test/payload.efi
PAYLOAD_ISO=build/test-payload.iso
FIXTURE_UKI=build/test/fixture-uki.efi
WORK_LABELS=build/test/labels
FIXTURE_UKI_SIGNED=build/test/fixture-uki-signed.efi
MKSTICK=build/bin/falcon-boot-mkstick
STAGE1=build/bin/falcon-bios-stage1.bin
STAGE2=build/bin/falcon-bios-stage2.bin
STAGE3=build/bin/falcon-bios-stage3.bin

UNSIGNED=0
PAYLOAD_UKI=0
LABELS=1
for arg in "$@"; do
    case "$arg" in
    --unsigned) UNSIGNED=1 ;;
    --payload-uki) PAYLOAD_UKI=1 ;;
    --no-labels) LABELS=0 ;;
    *)
        echo "ERROR: unknown option $arg" >&2
        exit 1
        ;;
    esac
done

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
for f in "$MKSTICK" "$STAGE1" "$STAGE2" "$STAGE3"; do
    [[ -f "$f" ]] || { echo "ERROR: $f not built (run: make cli bios)" >&2; exit 1; }
done

# One install ID for every record on the stick (DESIGN Section 3.1).
INSTALL_ID=${STICK_ID:-$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')}
[[ "$INSTALL_ID" =~ ^[0-9a-f]{32}$ ]] || {
    echo "ERROR: STICK_ID must be 32 lowercase hex digits" >&2
    exit 1
}
read -r -a ISOS <<< "${STICK_ISOS:-archlinux-fake.iso systemrescue-fake.iso}"
read -r -a ISO_FILES <<< "${STICK_ISO_FILES:-}"
for f in "${ISO_FILES[@]}"; do
    [[ -f "${f%%:*}" ]] || { echo "ERROR: ${f%%:*} missing" >&2; exit 1; }
done

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
    falcon-efi/tools/shim-spike/fetch-shims.sh
fi

mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
truncate -s "${SIZE_MIB}M" "$IMG"

# One MiB alignment throughout, which is both what real media wants and what
# keeps every partition start a whole number of 4 KiB filesystem blocks.
echo "== partitioning (GPT, four partitions per DESIGN Section 3)"
# The types per DESIGN Section 3.1; the records below, not the types or
# the -c names, are what the loaders identify a partition by:
#
#   p2  Microsoft basic data (ebd0a0a2-...), so Windows creates a volume for
#       it and gives it a drive letter, without which drag-and-drop from a
#       Windows host and the WinPE jump binary both have nothing to talk to.
#   p3  the falcon UKI-shelf type. Deliberately NOT basic data: Windows
#       offers to format a basic-data partition it cannot read, and this one
#       holds the stick's private key.
#   p1  the standard ESP type; p4 the standard Linux LUKS type (8309), also
#       unrecognised by Windows on purpose. The loader never reads p4.
#
# The constants are frozen in falcon-core/src/part/part.h and must match it
# exactly.
sgdisk \
    -n 1:1MiB:+64MiB   -t 1:ef00 -c 1:"FALCON-ESP" \
    -n 2:0:+64MiB      -t 2:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 \
                       -c 2:"FALCON-ISO" \
    -n 3:0:+64MiB      -t 3:9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF \
                       -c 3:"FALCON-UKI" \
    -n 4:0:+32MiB      -t 4:8309 -c 4:"FALCON-ROOT" \
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
# Same staging as falcon-efi/tools/run-qemu.sh --secboot, but on a real FAT32
# partition rather than a vvfat directory: BOOTX64.EFI is Fedora's shim, and our
# loader goes under grubx64.efi because that is the next-stage name compiled
# into it.
# The reserved sectors hold BIOS stage 3 and the marker, sized by the rule
# mkstick create uses (DESIGN Section 3.3).
echo "== p1: FAT32 ESP"
sudo mkfs.fat -F 32 -R "$("$MKSTICK" ident --reserved-sectors --stage3 "$STAGE3")" \
    -n FALCONESP "$P1" >/dev/null
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
    [[ -f "$MOK_CER" ]] && cp "$MOK_CER" "$MNT/falcon-boot.cer"
    echo "   shim -> mmx64.efi -> grubx64.efi (our signed loader)"
fi
unmount_rw

# ---- p2: the exFAT ISO library ----
echo "== p2: exFAT ISO library"
sudo mkfs.exfat -n FALCON-ISO "$P2" >/dev/null
mount_rw "$P2"
for iso in "${ISOS[@]}"; do
    dd if=/dev/zero of="$MNT/$iso" bs=1M count=1 status=none
done
for f in "${ISO_FILES[@]}"; do
    cp "${f%%:*}" "$MNT/${f#*:}"
    echo "   ${f#*:} staged (from ${f%%:*})"
done
if [[ -f "$PAYLOAD_ISO" ]]; then
    cp "$PAYLOAD_ISO" "$MNT/payload-test.iso"
    echo "   payload-test.iso staged (real El Torito, bootable)"
fi
printf 'falcon 1 library %s\n' "$INSTALL_ID" > "$MNT/.falcon"
unmount_rw

# ---- p3: LUKS2 with an argon2id keyslot, ext4 inside ----
echo "== p3: LUKS2 (argon2id, $P3_KDF_MEMORY KiB, time $P3_KDF_TIME, $P3_KDF_LANES lanes) + ext4"
printf '%s' "$PASS" | sudo cryptsetup luksFormat \
    --type luks2 \
    --pbkdf argon2id --pbkdf-memory "$P3_KDF_MEMORY" \
    --pbkdf-force-iterations "$P3_KDF_TIME" \
    --pbkdf-parallel "$P3_KDF_LANES" \
    --cipher aes-xts-plain64 --key-size 512 \
    --batch-mode "$P3" -
printf '{"type":"falcon","keyslots":[],"version":1,"role":"shelf","install_id":"%s"}' \
    "$INSTALL_ID" | sudo cryptsetup token import "$P3"
MAPPER=falcon-test-p3-$$
printf '%s' "$PASS" | sudo cryptsetup open "$P3" "$MAPPER" -
sudo mkfs.ext4 -q -L FALCON-UKI "/dev/mapper/$MAPPER"
mount_rw_owned "/dev/mapper/$MAPPER"

# The two UKIs. personal.efi is the one that must boot (signed with the
# enrolled test MOK) and unsigned.efi is the same image without a signature,
# which shim must refuse under Secure Boot.
#
# Default source is the sd-stub fixture UKI, because step 6 tests the
# credential handoff and nothing but a real sd-stub performs the pickup. The
# stand-in payload is kept behind --payload-uki (and is the automatic fallback
# when the fixture has not been built) for the cases that only need a PE the
# platform will judge.
UKI_SRC="$FIXTURE_UKI"
UKI_SRC_SIGNED="$FIXTURE_UKI_SIGNED"
UKI_KIND="sd-stub fixture UKI"
if [[ "$PAYLOAD_UKI" -eq 1 || ! -f "$FIXTURE_UKI" ]]; then
    if [[ "$PAYLOAD_UKI" -eq 0 ]]; then
        echo "note: $FIXTURE_UKI not built; using the stand-in payload" >&2
        echo "      (run 'make test-uki' for the credential-handoff rig)" >&2
    fi
    UKI_SRC="$PAYLOAD"
    UKI_SRC_SIGNED=
    UKI_KIND="stand-in payload"
fi

mkdir -p "$MNT/uki"
if [[ "$UNSIGNED" -eq 1 ]]; then
    cp "$UKI_SRC" "$MNT/uki/personal.efi"
elif [[ -n "$UKI_SRC_SIGNED" && -f "$UKI_SRC_SIGNED" ]]; then
    # Already signed by the tool that built it; copying beats re-signing so the
    # bytes under test are the bytes that tool emitted.
    cp "$UKI_SRC_SIGNED" "$MNT/uki/personal.efi"
else
    sbsign --key "$MOK_KEY" --cert "$MOK_CRT" \
        --output "$MNT/uki/personal.efi" "$UKI_SRC" >/dev/null 2>&1
fi
cp "$UKI_SRC" "$MNT/uki/unsigned.efi"
echo "   /uki populated from the $UKI_KIND"

# The label fixtures (falcon-efi/tools/mk-test-labels.sh). personal.efi and
# unsigned.efi test what the loader LOADS; these test what it CALLS things,
# which is a separate path and was for a while a broken one. They are tiny, they
# do not boot, and the chooser only appears when the shelf holds more than one
# image, so staging them also keeps that screen on the default rig run.
if [[ "$LABELS" -eq 0 ]]; then
    echo "   /uki label fixtures left off (--no-labels)"
elif [[ -x falcon-efi/tools/mk-test-labels.sh ]] && command -v ukify >/dev/null; then
    falcon-efi/tools/mk-test-labels.sh "$WORK_LABELS" >/dev/null
    cp "$WORK_LABELS"/*.efi "$MNT/uki/"
    echo "   /uki label fixtures: $(ls "$WORK_LABELS" | tr '\n' ' ')"
else
    echo "   /uki label fixtures skipped (no ukify)" >&2
fi
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
# Nothing inside it, and no record: its job is to be a LUKS2 volume the scan
# passes over because it is not this install's shelf.
echo "== p4: LUKS2 (argon2id, stock defaults), left empty"
printf '%s' "$PASS" | sudo cryptsetup luksFormat \
    --type luks2 --batch-mode "$P4" -

sudo losetup -d "$LOOP"
LOOP=

# ---- the boot record and the BIOS stages ----
#
# On the image file, with nothing attached: p1's boot sector (and its FAT32
# backup) gets the binary record, then `mkstick bios` finds p1 by it and
# puts stage 1 in LBA 0, stage 2 at the GPT place in a FALCON-BIOS
# partition, and stage 3 in p1's reserved sectors.
echo "== p1: boot record; the BIOS stages"
"$MKSTICK" --quiet ident "$IMG" --write-boot 1 --id "$INSTALL_ID" >/dev/null
"$MKSTICK" --quiet bios --reserve --no-backup --stage1 "$STAGE1" \
    --stage2 "$STAGE2" --stage3 "$STAGE3" "$IMG"

echo
echo "created $IMG (${SIZE_MIB} MiB, GPT, four partitions and the BIOS stages)"
echo "  install ID: $INSTALL_ID"
echo "  passphrase: $PASS  (override with FALCON_TEST_PASS)"
echo
echo "expected menu:"
echo "  Personal OS (encrypted)       <- pinned first, from p3's argon2id slot"
for iso in "${ISOS[@]}" $([[ -f "$PAYLOAD_ISO" ]] && echo payload-test.iso); do
    echo "  $iso"
done | sort
echo
echo "expected /uki listing after unlock:"
if [[ "$UNSIGNED" -eq 1 ]]; then
    echo "  Personal OS: personal.efi     (unsigned; SB-off rig only)"
else
    echo "  Personal OS: personal.efi     (signed with the test MOK: boots)"
fi
echo "  Personal OS: unsigned.efi     (unsigned: refused under Secure Boot)"
echo "  README.txt / decoy.bin        present, never offered"
if [[ "$LABELS" -eq 1 && -d "$WORK_LABELS" ]]; then
    echo
    echo "expected chooser, in full (the pair above carry ukify's autodetected"
    echo "\"Arch Linux\" too, so they join arch-b's group):"
    echo "  Personal OS: Arch Linux 7.2.6-arch2-1   only holder of that kernel"
    echo "  Personal OS: Arch Linux (arch-b.efi)    kernel shared with 2 below"
    echo "  Personal OS: Arch Linux (personal.efi)  same"
    echo "  Personal OS: Solo Linux 42              unique label: no qualifier"
    echo "  Personal OS: Twin Linux (twin-a.efi)    kernel shared with twin-b"
    echo "  Personal OS: Twin Linux (twin-b.efi)    same"
    echo "  Personal OS: Arch Linux (unsigned.efi)  kernel shared with arch-b"
fi
