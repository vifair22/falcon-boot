#!/usr/bin/env bash
# falcon jump's PC Card rig: QEMU with falcon-pcc.c built in (docs/DESIGN.md
# Section 13.9).
#
# QEMU models no PC Card bridge on x86, so the rig builds one: this fetches
# the QEMU release the machine already runs (its BIOS files and behaviour
# otherwise the same), adds falcon-pcc.c as a device, and builds
# qemu-system-i386 alone into build/qemu-pcc/. Idempotent: an existing tree
# is reused and only rebuilt.
#
# Usage: falcon-bios/tools/qemu-pcc/build-qemu.sh
# Output: build/qemu-pcc/build/qemu-system-i386; run it with
#         -L /usr/share/qemu for the BIOS files and
#         -device falcon-pcc,drive=<id>.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
# The release qemu-system-i386 on PATH reports (Debian's "10.0.13+ds" is
# upstream 10.0.13), so the BIOS files under /usr/share/qemu are its own.
VER=${QEMU_VERSION:-$(qemu-system-i386 --version |
    sed -n '1s/^QEMU emulator version \([0-9][0-9.]*\).*/\1/p')}
[[ -n "$VER" ]] || { echo "ERROR: no qemu-system-i386 to take a version from; set QEMU_VERSION" >&2; exit 1; }
DIR="$ROOT/build/qemu-pcc"
SRC="$DIR/qemu-$VER"

mkdir -p "$DIR"
if [[ ! -d "$SRC" ]]; then
    curl -sfL -o "$DIR/qemu-$VER.tar.xz" \
        "https://download.qemu.org/qemu-$VER.tar.xz"
    tar -xf "$DIR/qemu-$VER.tar.xz" -C "$DIR"
fi

cp "$ROOT/falcon-bios/tools/qemu-pcc/falcon-pcc.c" "$SRC/hw/misc/falcon-pcc.c"
grep -q falcon-pcc "$SRC/hw/misc/meson.build" ||
    printf "\nsystem_ss.add(when: 'CONFIG_FALCON_PCC', if_true: files('falcon-pcc.c'))\n" \
        >> "$SRC/hw/misc/meson.build"
grep -q FALCON_PCC "$SRC/hw/misc/Kconfig" ||
    printf "\nconfig FALCON_PCC\n    bool\n    default y if TEST_DEVICES\n    depends on PCI\n" \
        >> "$SRC/hw/misc/Kconfig"

if [[ ! -f "$DIR/build/build.ninja" ]]; then
    mkdir -p "$DIR/build"
    (cd "$DIR/build" && "$SRC/configure" --target-list=i386-softmmu \
        --disable-docs --disable-werror --disable-gtk --disable-sdl \
        --disable-vnc --disable-spice --disable-opengl --disable-curl \
        --disable-xen --disable-tools --disable-guest-agent \
        --disable-debug-info >/dev/null)
fi
# Quiet when it works; the whole log when it does not, since the likely
# failure is falcon-pcc.c meeting a QEMU whose internal headers moved.
if ! ninja -C "$DIR/build" qemu-system-i386 > "$DIR/ninja.log" 2>&1; then
    grep -E 'error|FAILED' "$DIR/ninja.log" | head -20 >&2
    echo "ERROR: the QEMU build failed; $DIR/ninja.log has all of it" >&2
    exit 1
fi
echo "$DIR/build/qemu-system-i386"
