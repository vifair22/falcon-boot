#!/usr/bin/env bash
# Download Fedora's kernel for x86-64 and AArch64, at one pinned build, and
# unpack each vmlinuz to build/fedora-kernel/<arch>/vmlinuz.
#
# The arm64 fixture UKI's kernel (falcon-efi/tools/mk-test-uki.sh,
# UKI_ARCH=aa64): Arch Linux has no arm64 ISO, and Fedora's arm64 vmlinuz is
# a zboot PE that sd-stub wraps as it is. Pinned like the shims, by build and
# by digest: a fixture whose inputs move is one whose results cannot be
# compared.
#
# Usage: tools/fetch-fedora-kernel.sh [x86_64|aarch64]...  (default: both)

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION=7.2.12
RELEASE=200.fc44
KOJI=https://kojipkgs.fedoraproject.org/packages/kernel/$VERSION/$RELEASE

declare -A SHA256=(
    [x86_64]=33bd29d63bb602f378047fe84c8e7aa540beae93fcf73d3e47f68c00d5fd0da6
    [aarch64]=d29c9d779462c64bd989d8226ac5b7f9a481c9a442197b82ca12dd8eebf1c706
)

OUT=build/fedora-kernel
PKGS=$OUT/pkgs
mkdir -p "$PKGS"

arches=("$@")
[[ ${#arches[@]} -gt 0 ]] || arches=(x86_64 aarch64)

for a in "${arches[@]}"; do
    [[ -n ${SHA256[$a]:-} ]] || { echo "ERROR: no pinned kernel for $a" >&2; exit 2; }
    rpm=kernel-core-$VERSION-$RELEASE.$a.rpm
    if [[ ! -f $PKGS/$rpm ]]; then
        echo "== fetching $rpm"
        curl -sSL --fail --max-time 600 -o "$PKGS/$rpm.part" "$KOJI/$a/$rpm"
        mv "$PKGS/$rpm.part" "$PKGS/$rpm"
    fi
    echo "${SHA256[$a]}  $PKGS/$rpm" | sha256sum -c --quiet - || {
        echo "ERROR: $rpm is not the pinned build" >&2
        exit 1
    }
    member=./lib/modules/$VERSION-$RELEASE.$a/vmlinuz
    mkdir -p "$OUT/$a"
    bsdtar -x -O -f "$PKGS/$rpm" "$member" > "$OUT/$a/vmlinuz"
    echo "== $OUT/$a/vmlinuz ($(stat -c%s "$OUT/$a/vmlinuz") bytes)"
done
