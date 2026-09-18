#!/usr/bin/env bash
# falcon-boot: build the bsdtar the signing appliance carries.
#
# Output: build/appliance/bsdtar
#
# Why not the host's. The sign tool reads ISO9660 and Rock Ridge through
# bsdtar and nothing else, but a distro libarchive is built for every format
# it knows. Arch's links libxml2 for XAR (an old macOS archive format), and
# libxml2 links ICU, so staging the host's bsdtar staged libxml2, libicuuc and
# the 32 MB libicudata with it: measured 2026-09-17, the largest single file
# in the image and never read. The links are DT_NEEDED, so the libraries
# cannot simply be left out; the image would carry a bsdtar that fails to exec
# with ENOENT. The fix is a libarchive built without them.
#
# What is kept. zlib, for zisofs (compressed files inside ISO9660, which some
# Linux media uses), and whatever glibc gives for free. libmd comes along
# because configure takes it whenever it is present and has no switch to
# refuse it; it is a few tens of kilobytes. Everything else that would add a
# library is off. libarchive is linked in static, so the result needs only
# libc, libz and libmd, and the image's closure walk stages those.
#
# The source is pinned by version and SHA-256. The pin was taken from the
# release tarball after checking its detached signature against the key Arch
# Linux names for libarchive's maintainer (Martin Matuska,
# 659C84C0E23EA1FA97E0B58CC040B508D63D2B36). The hash is what this script
# enforces; the signature check is how the hash was chosen, and is the check
# to repeat when the pin moves.
#
# Usage: falcon-appliance/image/mk-bsdtar.sh
# Env:
#   LIBARCHIVE_SRC  a local copy of the pinned tarball, for building offline.
#                   Held to the same hash as a download.

set -euo pipefail

cd "$(dirname "$0")/../.."

VER=3.8.9
SHA256=888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde
URL="https://github.com/libarchive/libarchive/releases/download/v$VER/libarchive-$VER.tar.xz"

OUT=build/appliance
CACHE="$OUT/src"
TARBALL="$CACHE/libarchive-$VER.tar.xz"
WORK="$OUT/libarchive-work"
BSDTAR="$OUT/bsdtar"

for t in curl sha256sum tar make cc; do
    command -v "$t" >/dev/null 2>&1 || { echo "ERROR: $t not found" >&2; exit 1; }
done

mkdir -p "$CACHE"
if [[ -n "${LIBARCHIVE_SRC:-}" ]]; then
    cp "$LIBARCHIVE_SRC" "$TARBALL.part"
    mv "$TARBALL.part" "$TARBALL"
elif [[ ! -f "$TARBALL" ]]; then
    echo "== fetching libarchive $VER"
    curl -fsSL -o "$TARBALL.part" "$URL"
    mv "$TARBALL.part" "$TARBALL"
fi

# Checked on every build, cached copy included: the cache is under build/
# and nothing else vouches for it.
echo "$SHA256  $TARBALL" | sha256sum -c --quiet - || {
    echo "ERROR: $TARBALL does not match the pinned SHA-256" >&2
    rm -f "$TARBALL"
    exit 1
}

rm -rf "$WORK"
mkdir -p "$WORK/build"
tar -C "$WORK" -xf "$TARBALL"

echo "== building bsdtar (libarchive $VER, ISO-reading subset)"
(
    cd "$WORK/build"
    "../libarchive-$VER/configure" \
        --disable-shared --enable-bsdtar=static \
        --disable-bsdcat --disable-bsdcpio --disable-bsdunzip \
        --disable-acl --disable-xattr \
        --without-xml2 --without-expat \
        --without-openssl --without-mbedtls --without-nettle --without-cng \
        --without-libb2 --without-lz4 --without-zstd --without-lzma \
        --without-bz2lib \
        > configure.log 2>&1 || {
            echo "ERROR: libarchive configure failed; see $WORK/build/configure.log" >&2
            exit 1
        }
    make -j"$(nproc)" bsdtar > make.log 2>&1 || {
        echo "ERROR: bsdtar build failed; see $WORK/build/make.log" >&2
        exit 1
    }
)

install -m 0755 -s "$WORK/build/bsdtar" "$BSDTAR"
rm -rf "$WORK"

# The reason this file exists, checked rather than trusted: configure quietly
# takes any library it finds, and a new release could grow a dependency.
if ldd "$BSDTAR" | grep -E 'libxml2|libicu|libexpat|libarchive'; then
    echo "ERROR: $BSDTAR still links the libraries it was built to drop" >&2
    exit 1
fi
echo "   bsdtar: $BSDTAR ($(stat -c%s "$BSDTAR") bytes)"
