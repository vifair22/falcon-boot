#!/usr/bin/env bash
# falcon-boot: build, sign and publish the Arch package of a built release
# (release.mk) to the falcon pacman repository on the dl host, served as
# https://dl.airies.net/arch/falcon/x86_64 and .../aarch64 (docs/Manual.md
# Section 14.3). The aarch64 package is built here too: package() only
# copies files, so makepkg runs with CARCH set to the other architecture.
#
# packaging/arch/PKGBUILD is built in an archlinux container as an
# unprivileged user, with the version and the tarball's checksum filled in;
# the package and the repository database are signed with the falcon
# repository key, whose public half is published beside them as
# falcon/falcon.asc. The repository holds the current version alone.
#
# The main tarballs go to https://dl.airies.net/falcon/src too, once per
# version and never overwritten, then falcon-boot.latest naming the version:
# vifair-overlay's bumper polls that pointer and fetches the tarballs from
# there for app-misc/falcon-boot-bin (its files/bumper.conf).
#
# The push uses a deploy key that the dl host's forced command lets do one
# thing, rsync into airies-dl/arch or airies-dl/falcon/src, and the host key
# pinned in tools/release/dl-known-hosts.
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
ARCHES=(x86_64 aarch64)
IMAGE=${FALCON_ARCH_IMAGE:-archlinux:base-devel}
KEY=${FALCON_ARCH_GPG_PRIVATE_KEY:?the signing key}
KEYID=${FALCON_ARCH_GPG_KEY_ID:?the signing key fingerprint}
DL_HOST=root@172.20.0.50
DL_PATH=/mnt/cache/appdata/airies-dl/arch/falcon
DL_SRC=/mnt/cache/appdata/airies-dl/falcon/src

for a in "${ARCHES[@]}"; do
    [[ -f $REL/falcon-boot-$VER-$a.tar.gz ]] ||
        { echo "publish-arch: no $REL/falcon-boot-$VER-$a.tar.gz" >&2; exit 1; }
done

# Not under build/: in a harness checkout that is the build directory's
# mount point, and root's.
OUT=$(mktemp -d "${TMPDIR:-/tmp}/falcon-arch.XXXXXX")
# mktemp's 700 would keep out the container's builder, who is not this uid.
chmod 755 "$OUT"
mkdir -p "$OUT/work"
cp packaging/arch/PKGBUILD packaging/arch/falcon-boot-bin.install "$OUT/work/"
sed -i -e "s/^pkgver=.*/pkgver=$VER/" "$OUT/work/PKGBUILD"
for a in "${ARCHES[@]}"; do
    mkdir -p "$OUT/repo/$a"
    cp "$REL/falcon-boot-$VER-$a.tar.gz" "$OUT/work/"
    sum=$(sha256sum "$REL/falcon-boot-$VER-$a.tar.gz" | cut -d' ' -f1)
    sed -i -e "s/^sha256sums_$a=.*/sha256sums_$a=($sum)/" "$OUT/work/PKGBUILD"
done

# The key goes in and out of the build directory within this script; File
# variables lose their trailing newline, which gpg does not mind.
install -m 600 "$KEY" "$OUT/key.asc"
trap 'rm -f "$OUT/key.asc"' EXIT

docker run --rm -v "$OUT:/o" -e KEYID="$KEYID" -e OWNER="$(id -u):$(id -g)" \
    -e ARCHES="${ARCHES[*]}" "$IMAGE" bash -euo pipefail -c '
    useradd -m builder
    chown -R builder /o/work
    for a in $ARCHES; do
        sed "s/^CARCH=.*/CARCH=\"$a\"/" /etc/makepkg.conf > /o/work/makepkg-$a.conf
        su builder -c "cd /o/work && makepkg --config makepkg-$a.conf --nodeps --noconfirm" \
            >/o/makepkg-$a.log 2>&1 || { cat /o/makepkg-$a.log; exit 1; }
    done
    export GNUPGHOME=$(mktemp -d)
    gpg --batch --quiet --import /o/key.asc
    pacman-key --init >/dev/null 2>&1
    gpg --batch --armor --export "$KEYID" > /o/repo/falcon.asc
    pacman-key --add /o/repo/falcon.asc >/dev/null 2>&1
    pacman-key --lsign-key "$KEYID" >/dev/null 2>&1
    for a in $ARCHES; do
        cd /o/repo/$a
        cp /o/work/*-$a.pkg.tar.zst .
        for p in *.pkg.tar.zst; do
            gpg --batch --quiet --detach-sign --no-armor -u "$KEYID" "$p"
        done
        repo-add --quiet --sign --key "$KEYID" falcon.db.tar.gz *.pkg.tar.zst
        for p in *.pkg.tar.zst; do pacman-key --verify "$p.sig" "$p" >/dev/null; done
    done
    chown -R "$OWNER" /o'
rm -f "$OUT/key.asc"
ls -l "$OUT"/repo/*

[[ ${NO_DEPLOY:-0} == 1 ]] && { echo "publish-arch: NO_DEPLOY, built in $OUT/repo"; exit 0; }

sshkey=$OUT/deploy_key
install -m 600 "${FALCON_DL_DEPLOY_SSH_KEY:?the deploy key}" "$sshkey"
printf '\n' >> "$sshkey"
trap 'rm -f "$OUT/key.asc" "$sshkey"' EXIT
# -L: the database's .db and .files are symlinks repo-add makes; the dl
# server is given files.
ssh_cmd="ssh -i $sshkey -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=$PWD/tools/release/dl-known-hosts"
rsync -rtL --delete -e "$ssh_cmd" "$OUT/repo/" "$DL_HOST:$DL_PATH/"
echo "publish-arch: falcon-boot-bin $VER published to https://dl.airies.net/arch/falcon/{x86_64,aarch64}"

# The overlay's Manifest is the review of these bytes, so a version once
# published is never replaced; the pointer goes last, naming only what is
# there.
srcs=()
for a in "${ARCHES[@]}"; do
    srcs+=("$REL/falcon-boot-$VER-$a.tar.gz")
done
printf '%s\n' "$VER" > "$OUT/falcon-boot.latest"
rsync -t --ignore-existing --chmod=F644 -e "$ssh_cmd" "${srcs[@]}" "$DL_HOST:$DL_SRC/"
rsync -t --chmod=F644 -e "$ssh_cmd" "$OUT/falcon-boot.latest" "$DL_HOST:$DL_SRC/"
echo "publish-arch: falcon-boot $VER tarballs and pointer at https://dl.airies.net/falcon/src"
