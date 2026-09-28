#!/usr/bin/env bash
# falcon-boot: build the LUKS2 fixtures the host test suite unlocks.
#
# falcon-core/src/luks is a from-scratch LUKS2 reader and falcon-core/src/crypto
# is hand-written crypto. Neither is worth anything unless it agrees with
# cryptsetup, so the fixtures are made by cryptsetup itself and the tests read
# them with our own code. That is the same argument
# falcon-core/tools/mk-test-exfat.sh makes for mkfs.exfat.
#
# Two images, one per KDF the loaders unlock:
#
#   build/test-luks.img        48 MiB, LUKS2, aes-xts-plain64, --key-size 512,
#                              one pbkdf2 keyslot, passphrase "falcon-test".
#                              An ordinary ext4 filesystem inside it with two
#                              known files, so the ext4 driver can be stacked
#                              on luks_read exactly as the loader stacks it.
#
#   build/test-luks-argon.img  20 MiB, LUKS2, argon2id keyslot, no filesystem.
#                              The kind of keyslot DESIGN Section 3 gives p3
#                              and cryptsetup gives a station's shelf: the
#                              tests unlock it with the caller's memory, and
#                              refuse it with LUKS_ERR_KDF_MEMORY when that
#                              memory is not supplied. Empty because the
#                              keyslot is all that is tested.
#
# ITERATION COUNT. --pbkdf-force-iterations 10000 is deliberately far below
# anything that belongs on real media: cryptsetup's own floor for pbkdf2 is
# 1000 and its benchmarked default is in the millions. 10000 keeps a fixture
# build and every unlock in the test suite down to milliseconds. The argon2id
# slot is likewise far below the stick's cost (mkstick.h, MKS_STICK_KDF_*);
# nothing here is evidence about that number, this only exercises the code
# path.
#
# The passphrase is a literal in this script and in the tests. That is correct
# for a throwaway fixture and must never be copied anywhere near a real
# volume.
#
# Products next to the images:
#   build/test-luks.manifest   type TAB path TAB size TAB sha256 for the files
#                              inside the encrypted ext4, plus '#' comment
#                              lines recording the header geometry cryptsetup
#                              chose. Same column shape as the ext4 fixture's
#                              manifest.
#   build/test-luks.ref64k     the first 64 KiB of PLAINTEXT, read off
#                              /dev/mapper while the volume is open here. The
#                              luks_read tests compare against this file, which
#                              is the whole reason it is extracted at build
#                              time: the alternative is a device-mapper open
#                              inside the test binary, and a unit test that
#                              needs sudo and leaves a dm node behind on a
#                              crash is not one worth having.
#
# Usage: falcon-core/tools/mk-test-luks.sh
# Needs cryptsetup, mkfs.ext4, sha256sum and passwordless sudo. Idempotent:
# it rebuilds both images from scratch every time.

set -euo pipefail

cd "$(dirname "$0")/../.."

IMG=build/test-luks.img
ARGON_IMG=build/test-luks-argon.img
MANIFEST=build/test-luks.manifest
REF=build/test-luks.ref64k
PASS='falcon-test'
SIZE_MIB=48
ARGON_SIZE_MIB=20
REF_KIB=64
PBKDF2_ITER=10000
ARGON_ITER=4
ARGON_MEM=65536

# $$ keeps two concurrent runs from fighting over one device-mapper name.
DM_NAME="falcon-fixture-$$"
MNT=
DM_OPEN=0

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
    if [[ "$DM_OPEN" -eq 1 ]]; then
        sudo cryptsetup close "$DM_NAME" 2>/dev/null || true
    fi
}
trap cleanup EXIT

for t in cryptsetup mkfs.ext4 sha256sum; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done

mkdir -p build

# ------------------------------------------------------- the pbkdf2 volume

rm -f "$IMG" "$MANIFEST" "$REF"
truncate -s "${SIZE_MIB}M" "$IMG"

printf '%s' "$PASS" | sudo cryptsetup luksFormat \
    --type luks2 \
    --cipher aes-xts-plain64 \
    --key-size 512 \
    --pbkdf pbkdf2 \
    --pbkdf-force-iterations "$PBKDF2_ITER" \
    --batch-mode \
    "$IMG" -

printf '%s' "$PASS" | sudo cryptsetup open "$IMG" "$DM_NAME" --key-file -
DM_OPEN=1

sudo mkfs.ext4 -q -L FALCON-LUKST "/dev/mapper/$DM_NAME"

MNT=$(mktemp -d)
sudo mount "/dev/mapper/$DM_NAME" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT" "$MNT/lost+found"

printf 'falcon-boot luks fixture: first known file.\n' > "$MNT/one.txt"
printf 'falcon-boot luks fixture: second known file, a little longer so the\n' \
    > "$MNT/two.txt"
printf 'two hashes cannot collide by being the same length.\n' >> "$MNT/two.txt"

sync
sudo umount "$MNT"
rmdir "$MNT"
MNT=

# The plaintext reference, taken through the kernel's dm-crypt so it is an
# independent decryption of the same bytes our XTS code will produce.
sudo dd if="/dev/mapper/$DM_NAME" of="$REF" bs=1K count="$REF_KIB" status=none
sudo chown "$(id -u):$(id -g)" "$REF"

# The manifest is built from a second mount rather than remembered from the
# first, so the recorded sizes and hashes are what the filesystem actually
# holds after the unmount rather than what was intended.
MNT=$(mktemp -d)
sudo mount -o ro "/dev/mapper/$DM_NAME" "$MNT"

SECTOR=$(sudo cryptsetup luksDump "$IMG" | sed -n 's/^\tsector: *\([0-9]*\).*/\1/p' | head -1)
OFFSET=$(sudo cryptsetup luksDump "$IMG" | sed -n 's/^\toffset: *\([0-9]*\).*/\1/p' | head -1)
UUID=$(sudo cryptsetup luksUUID "$IMG")

{
    echo "# falcon-boot LUKS2 fixture manifest"
    echo "# image: $IMG (${SIZE_MIB} MiB, luks2 aes-xts-plain64, key-size 512)"
    echo "# keyslot: pbkdf2, $PBKDF2_ITER iterations (LOW, fixture only)"
    echo "# passphrase: $PASS"
    echo "# uuid: $UUID"
    echo "# segment offset: $OFFSET bytes, sector size: $SECTOR bytes"
    echo "# plaintext reference: $REF (first ${REF_KIB} KiB)"
    echo "# columns: type TAB path TAB size TAB sha256 (inside the volume)"
    find "$MNT" -mindepth 1 -printf '%y\t%P\t%s\n' | LC_ALL=C sort -t'	' -k2,2 |
    while IFS=$'\t' read -r ty path size; do
        sum='-'
        if [[ "$ty" == "f" ]]; then
            sum=$(sha256sum "$MNT/$path" | cut -d' ' -f1)
        fi
        printf '%s\t/%s\t%s\t%s\n' "$ty" "$path" "$size" "$sum"
    done
} > "$MANIFEST"

sudo umount "$MNT"
rmdir "$MNT"
MNT=

sudo cryptsetup close "$DM_NAME"
DM_OPEN=0

sudo chown "$(id -u):$(id -g)" "$IMG"

# -------------------------------------------------------- the argon volume

rm -f "$ARGON_IMG"
truncate -s "${ARGON_SIZE_MIB}M" "$ARGON_IMG"

# Default argon2id parameters are benchmarked against the builder's machine
# and would make this the slowest step in the whole fixture build, and every
# test that unlocks it, for no benefit: small parameters exercise the same
# code.
printf '%s' "$PASS" | sudo cryptsetup luksFormat \
    --type luks2 \
    --cipher aes-xts-plain64 \
    --key-size 512 \
    --pbkdf argon2id \
    --pbkdf-memory "$ARGON_MEM" \
    --pbkdf-force-iterations "$ARGON_ITER" \
    --batch-mode \
    "$ARGON_IMG" -

sudo chown "$(id -u):$(id -g)" "$ARGON_IMG"

nobj=$(grep -cv '^#' "$MANIFEST")

echo "created $IMG (${SIZE_MIB} MiB, luks2/pbkdf2, passphrase '$PASS')"
echo "created $ARGON_IMG (${ARGON_SIZE_MIB} MiB, luks2/argon2id, no filesystem)"
echo "created $MANIFEST ($nobj objects inside the volume)"
echo "created $REF (${REF_KIB} KiB of plaintext via dm-crypt)"
echo "  segment offset: $OFFSET bytes, sector size: $SECTOR bytes"
