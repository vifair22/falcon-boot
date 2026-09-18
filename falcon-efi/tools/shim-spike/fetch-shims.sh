#!/usr/bin/env bash
# Download the distro shim packages the spike classifies and unpack the EFI
# binaries into build/shim/bins/ under stable names.
#
# Exact versions are pinned rather than tracking "current": a spike whose
# inputs move is a spike whose conclusions cannot be rechecked. Bump them
# deliberately and re-run the classification.
#
# rpm2cpio is not installed here; bsdtar reads both .rpm and .deb directly.

set -euo pipefail

cd "$(dirname "$0")/../../.."

OUT=build/shim
PKGS=$OUT/pkgs
EXTRACT=$OUT/extract
BINS=$OUT/bins

KOJI=https://kojipkgs.fedoraproject.org/packages/shim
# download.opensuse.org's redirector 404s on files its index still lists;
# a named mirror is the reliable path.
SUSE=https://provo-mirror.opensuse.org/tumbleweed/repo/oss/x86_64
DEBIAN=https://deb.debian.org/debian/pool/main/s/shim-signed

FEDORA_161=shim-x64-16.1-7.x86_64.rpm
FEDORA_158=shim-x64-15.8-5.x86_64.rpm
SUSE_161=shim-16.1-4.2.x86_64.rpm
DEBIAN_161=shim-signed_1.51+16.1-2_amd64.deb

mkdir -p "$PKGS" "$EXTRACT" "$BINS"

fetch() {
    local url=$1 file=$2
    [[ -f $PKGS/$file ]] && return 0
    echo "== fetching $file"
    curl -sSL --fail --max-time 300 -o "$PKGS/$file" "$url/$file"
}

fetch "$KOJI/16.1/7/x86_64" "$FEDORA_161"
fetch "$KOJI/15.8/5/x86_64" "$FEDORA_158"
fetch "$SUSE"               "$SUSE_161"
fetch "$DEBIAN"             "$DEBIAN_161"

unpack() {
    local dir=$1 file=$2
    rm -rf "${EXTRACT:?}/$dir"
    mkdir -p "$EXTRACT/$dir"
    ( cd "$EXTRACT/$dir" && bsdtar xf "../../pkgs/$file" )
    # .deb is an ar archive whose payload needs a second pass.
    if [[ -f $EXTRACT/$dir/data.tar.xz ]]; then
        ( cd "$EXTRACT/$dir" && bsdtar xf data.tar.xz )
    fi
}

unpack fedora-16.1  "$FEDORA_161"
unpack fedora-15.8  "$FEDORA_158"
unpack suse-16.1    "$SUSE_161"
unpack debian-16.1  "$DEBIAN_161"

cp "$EXTRACT/fedora-16.1/usr/lib/efi/shim/16.1-7/EFI/fedora/shimx64.efi" "$BINS/fedora-16.1-shimx64.efi"
cp "$EXTRACT/fedora-16.1/usr/lib/efi/shim/16.1-7/EFI/fedora/mmx64.efi"   "$BINS/fedora-16.1-mmx64.efi"
cp "$EXTRACT/fedora-15.8/usr/lib/efi/shim/15.8-5/EFI/fedora/shimx64.efi" "$BINS/fedora-15.8-shimx64.efi"
cp "$EXTRACT/fedora-15.8/usr/lib/efi/shim/15.8-5/EFI/fedora/mmx64.efi"   "$BINS/fedora-15.8-mmx64.efi"
cp "$EXTRACT/suse-16.1/usr/share/efi/x86_64/shim.efi"                    "$BINS/suse-16.1-shim.efi"
cp "$EXTRACT/suse-16.1/usr/share/efi/x86_64/shim-opensuse.efi"           "$BINS/suse-16.1-shim-opensuse.efi"
cp "$EXTRACT/suse-16.1/usr/share/efi/x86_64/MokManager.efi"              "$BINS/suse-16.1-MokManager.efi"
cp "$EXTRACT/debian-16.1/usr/lib/shim/shimx64.efi.signed"                "$BINS/debian-16.1-shimx64.efi"
chmod u+w "$BINS"/*.efi

echo "== staged:"
ls -la "$BINS"
