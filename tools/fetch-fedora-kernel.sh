#!/usr/bin/env bash
# Download Fedora's kernel for x86-64 and AArch64, at one pinned build:
# build/fedora-kernel/<arch>/vmlinuz, and its core module tree under
# build/fedora-kernel/<arch>/modules/.
#
# The kernel both appliances (falcon-appliance/image/mk-appliance.sh) and
# the arm64 fixture UKI (falcon-efi/tools/mk-test-uki.sh, UKI_ARCH=aa64) are
# built on. One distribution for both architectures, so there is one source
# to keep current: Arch Linux has no arm64 ISO, and Fedora's arm64 vmlinuz is
# a zboot PE that sd-stub wraps as it is. kernel-core carries the kernel,
# kernel-modules-core the modules the appliance needs (exfat, vfat, loop,
# usb-storage, uas, i2c-hid). Pinned like the shims, by build and by digest:
# a fixture whose inputs move is one whose results cannot be compared.
#
# Usage: tools/fetch-fedora-kernel.sh [x86_64|aarch64]...  (default: both)

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION=7.2.12
RELEASE=200.fc44
KOJI=https://kojipkgs.fedoraproject.org/packages/kernel/$VERSION/$RELEASE

declare -A SHA256=(
    [kernel-core.x86_64]=33bd29d63bb602f378047fe84c8e7aa540beae93fcf73d3e47f68c00d5fd0da6
    [kernel-core.aarch64]=d29c9d779462c64bd989d8226ac5b7f9a481c9a442197b82ca12dd8eebf1c706
    [kernel-modules-core.x86_64]=5fc32470196d6eac20d387deb44cd183e21b6ec0fac131f364645a968db148f4
    [kernel-modules-core.aarch64]=7a37cebf15e6fef53c4bc749035cf7a6caa74eac8d71763b4f54f0abd3658b04
)

OUT=build/fedora-kernel
PKGS=$OUT/pkgs
mkdir -p "$PKGS"

fetch() {   # fetch PACKAGE ARCH -> the verified rpm's path
    local pkg=$1 a=$2 rpm
    rpm=$pkg-$VERSION-$RELEASE.$a.rpm
    if [[ ! -f $PKGS/$rpm ]]; then
        echo "== fetching $rpm" >&2
        curl -sSL --fail --max-time 900 -o "$PKGS/$rpm.part" "$KOJI/$a/$rpm"
        mv "$PKGS/$rpm.part" "$PKGS/$rpm"
    fi
    echo "${SHA256[$pkg.$a]}  $PKGS/$rpm" | sha256sum -c --quiet - || {
        echo "ERROR: $rpm is not the pinned build" >&2
        exit 1
    }
    printf '%s' "$PKGS/$rpm"
}

arches=("$@")
[[ ${#arches[@]} -gt 0 ]] || arches=(x86_64 aarch64)

for a in "${arches[@]}"; do
    [[ -n ${SHA256[kernel-core.$a]:-} ]] ||
        { echo "ERROR: no pinned kernel for $a" >&2; exit 2; }
    kver=$VERSION-$RELEASE.$a
    core=$(fetch kernel-core "$a")
    mods=$(fetch kernel-modules-core "$a")
    rm -rf "${OUT:?}/$a"
    mkdir -p "$OUT/$a/modules"
    bsdtar -x -O -f "$core" "./lib/modules/$kver/vmlinuz" > "$OUT/$a/vmlinuz"
    bsdtar -x -f "$core" -C "$OUT/$a/modules" --strip-components 4 \
        "./lib/modules/$kver/modules.builtin"
    bsdtar -x -f "$mods" -C "$OUT/$a/modules" --strip-components 4 \
        "./lib/modules/$kver/kernel"
    printf '%s\n' "$kver" > "$OUT/$a/kver"
    echo "== $OUT/$a: $kver, vmlinuz $(stat -c%s "$OUT/$a/vmlinuz") bytes," \
         "$(find "$OUT/$a/modules" -name '*.ko.xz' | wc -l) modules"
done
