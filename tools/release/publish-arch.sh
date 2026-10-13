#!/usr/bin/env bash
# falcon-boot: build, sign and publish the Arch package of a built release
# (release.mk) to the falcon pacman repository on the dl host, served as
# https://dl.airies.net/arch/falcon/x86_64 (docs/Manual.md Section 14.3).
#
# packaging/arch/PKGBUILD is built in an archlinux container as an
# unprivileged user, with the version and the tarball's checksum filled in;
# the package and the repository database are signed with the falcon
# repository key, whose public half is published beside them as
# falcon/falcon.asc. The repository holds the current version alone.
#
# The push uses a deploy key that the dl host's forced command lets do one
# thing, rsync into airies-dl/arch, and the host key pinned in
# tools/release/dl-known-hosts.
#
# Usage: tools/release/publish-arch.sh RELEASE_DIR
# Env:   FALCON_ARCH_GPG_PRIVATE_KEY  the signing key, armored (a file path)
#        FALCON_ARCH_GPG_KEY_ID       its fingerprint
#        FALCON_DL_DEPLOY_SSH_KEY     the deploy key (a file path)
#        FALCON_ARCH_IMAGE            default archlinux:base-devel
#        NO_DEPLOY=1                  build and sign only; the repository is
#                                     left in the temporary directory it names
set -euo pipefail

cd "$(dirname "$0")/../.."
REL=${1:?usage: publish-arch.sh RELEASE_DIR}
VER=$(cat release_version)
TAR=$REL/falcon-boot-$VER-x86_64.tar.gz
IMAGE=${FALCON_ARCH_IMAGE:-archlinux:base-devel}
KEY=${FALCON_ARCH_GPG_PRIVATE_KEY:?the signing key}
KEYID=${FALCON_ARCH_GPG_KEY_ID:?the signing key fingerprint}
DL_HOST=root@172.20.0.50
DL_PATH=/mnt/cache/appdata/airies-dl/arch/falcon

[[ -f $TAR ]] || { echo "publish-arch: no $TAR" >&2; exit 1; }

# Not under build/: in a harness checkout that is the build directory's
# mount point, and root's.
OUT=$(mktemp -d "${TMPDIR:-/tmp}/falcon-arch.XXXXXX")
mkdir -p "$OUT/work" "$OUT/repo/x86_64"
cp packaging/arch/PKGBUILD packaging/arch/falcon-boot-bin.install "$TAR" "$OUT/work/"
sum=$(sha256sum "$TAR" | cut -d' ' -f1)
sed -i -e "s/^pkgver=.*/pkgver=$VER/" -e "s/^sha256sums=.*/sha256sums=($sum)/" \
    "$OUT/work/PKGBUILD"

# The key goes in and out of the build directory within this script; File
# variables lose their trailing newline, which gpg does not mind.
install -m 600 "$KEY" "$OUT/key.asc"
trap 'rm -f "$OUT/key.asc"' EXIT

docker run --rm -v "$OUT:/o" -e KEYID="$KEYID" -e OWNER="$(id -u):$(id -g)" \
    "$IMAGE" bash -euo pipefail -c '
    useradd -m builder
    chown -R builder /o/work
    su builder -c "cd /o/work && makepkg --nodeps --noconfirm" >/o/makepkg.log 2>&1 ||
        { cat /o/makepkg.log; exit 1; }
    export GNUPGHOME=$(mktemp -d)
    gpg --batch --quiet --import /o/key.asc
    cd /o/repo/x86_64
    cp /o/work/*.pkg.tar.zst .
    for p in *.pkg.tar.zst; do
        gpg --batch --quiet --detach-sign --no-armor -u "$KEYID" "$p"
    done
    repo-add --quiet --sign --key "$KEYID" falcon.db.tar.gz *.pkg.tar.zst
    gpg --batch --armor --export "$KEYID" > /o/repo/falcon.asc
    pacman-key --init >/dev/null 2>&1
    pacman-key --add /o/repo/falcon.asc >/dev/null 2>&1
    pacman-key --lsign-key "$KEYID" >/dev/null 2>&1
    for p in *.pkg.tar.zst; do pacman-key --verify "$p.sig" "$p" >/dev/null; done
    chown -R "$OWNER" /o'
rm -f "$OUT/key.asc"
ls -l "$OUT/repo/x86_64"

[[ ${NO_DEPLOY:-0} == 1 ]] && { echo "publish-arch: NO_DEPLOY, built in $OUT/repo"; exit 0; }

sshkey=$OUT/deploy_key
install -m 600 "${FALCON_DL_DEPLOY_SSH_KEY:?the deploy key}" "$sshkey"
printf '\n' >> "$sshkey"
trap 'rm -f "$OUT/key.asc" "$sshkey"' EXIT
# -L: the database's .db and .files are symlinks repo-add makes; the dl
# server is given files.
rsync -rtL --delete \
    -e "ssh -i $sshkey -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=$PWD/tools/release/dl-known-hosts" \
    "$OUT/repo/" "$DL_HOST:$DL_PATH/"
echo "publish-arch: falcon-boot-bin $VER published to https://dl.airies.net/arch/falcon/x86_64"
