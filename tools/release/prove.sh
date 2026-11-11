#!/usr/bin/env bash
# falcon-boot: use a built release (release.mk) the way a user gets it,
# before any tag publishes it (docs/Manual.md Section 14.5).
#
# Runs in the harness container, as root, whose /usr is its own and gone
# when it exits: SHA256SUMS is checked over the release, the x86-64 tarball
# is unpacked, its MANIFEST checked and its usr/ copied over the container's
# /usr, as the Manual's install does. The installed mkstick reads its data
# from /usr/lib/falcon-boot, so run-core-rig.sh's release-stick cases make a
# stick with it naming no file of the build, and boot it under OVMF with
# Secure Boot and under SeaBIOS. The aarch64 tarball differs in usr/bin
# alone; its MANIFEST is checked.
#
# Usage: tools/release/prove.sh RELEASE_DIR   (build/release)
set -euo pipefail
cd "$(dirname "$0")/../.."

REL=${1:?usage: prove.sh RELEASE_DIR}
VER=$(cat release_version)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/falcon-prove.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL  release: $*"; exit 1; }

( cd "$REL" && sha256sum -c --quiet SHA256SUMS ) ||
    fail "SHA256SUMS does not match the files"
echo "PASS  release: SHA256SUMS matches the four files"

for a in x86_64 aarch64; do
    tar -xzf "$REL/falcon-boot-$VER-$a.tar.gz" -C "$WORK" ||
        fail "falcon-boot-$VER-$a.tar.gz does not unpack"
    ( cd "$WORK/falcon-boot-$VER" && sha256sum -c --quiet MANIFEST ) ||
        fail "the $a tarball's MANIFEST does not match its files"
    echo "PASS  release: the $a tarball unpacks and its MANIFEST matches"
    [[ $a == x86_64 ]] && cp -a "$WORK/falcon-boot-$VER/usr/." /usr/
    rm -rf "$WORK/falcon-boot-$VER"
done

/usr/bin/falcon-boot-mkstick --help >/dev/null 2>&1 ||
    fail "the installed mkstick does not run"
FALCON_RELEASE_MKSTICK=/usr/bin/falcon-boot-mkstick \
    falcon-efi/tools/run-core-rig.sh release-stick-uefi-sb release-stick-bios
