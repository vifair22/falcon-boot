#!/usr/bin/env bash
# falcon jump's PC Card rig: QEMU with falcon-pcc.c built in (docs/DESIGN.md
# Section 13.9).
#
# QEMU models no PC Card bridge on x86, so the rig builds one: this fetches
# the QEMU release the machine already runs (its BIOS files and behaviour
# otherwise the same), adds falcon-pcc.c as a device, turns on cmd646-ide
# (a PCI IDE controller in native mode, which no x86 machine type selects;
# the cd-native case), and builds qemu-system-i386 alone into
# build/qemu-pcc/. It also builds the release's own SeaBIOS with
# seabios-cardbus.patch (the patch says why) to build/qemu-pcc/share/, for
# the rig to put before /usr/share/qemu with -L. Idempotent: an existing
# tree is reused and only rebuilt.
#
# Usage: falcon-bios/tools/qemu-pcc/build-qemu.sh
# Output: build/qemu-pcc/build/qemu-system-i386; run it with
#         -L build/qemu-pcc/share -L /usr/share/qemu for the BIOS files and
#         -device falcon-pcc,drive=<id>, or -device cmd646-ide,id=<id>.
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
grep -q IDE_CMD646 "$SRC/configs/devices/i386-softmmu/default.mak" ||
    printf "\nCONFIG_IDE_CMD646=y\n" \
        >> "$SRC/configs/devices/i386-softmmu/default.mak"

# The release's own firmware path, so the ROMs it finds without -L are the
# ones the machine's QEMU finds: Debian splits them over /usr/share/qemu,
# /usr/share/seabios (the VGA BIOSes) and /usr/lib/ipxe/qemu, and upstream's
# default names none of those. A tree configured before this keeps its old
# path; remove build/qemu-pcc/build to reconfigure.
FWPATH=$(qemu-system-i386 -L help | paste -sd:)

if [[ ! -f "$DIR/build/build.ninja" ]]; then
    mkdir -p "$DIR/build"
    (cd "$DIR/build" && "$SRC/configure" --target-list=i386-softmmu \
        --firmwarepath="$FWPATH" \
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

# The release's SeaBIOS (roms/seabios in its tarball), patched, built the
# way the release builds its bios-256k.bin. The patch's own text is the
# guard against applying it twice.
SEABIOS="$SRC/roms/seabios"
PATCH="$ROOT/falcon-bios/tools/qemu-pcc/seabios-cardbus.patch"
grep -q "A CardBus bridge's header holds one BAR" "$SEABIOS/src/fw/pciinit.c" ||
    patch -s -d "$SEABIOS" -p1 < "$PATCH"
mkdir -p "$DIR/share"
if [[ ! -f "$DIR/share/bios-256k.bin" || "$DIR/share/bios-256k.bin" -ot "$PATCH" ]]; then
    if ! make -C "$SRC/roms" PYTHON=python3 build-seabios-config-seabios-256k \
            > "$DIR/seabios.log" 2>&1; then
        tail -20 "$DIR/seabios.log" >&2
        echo "ERROR: the SeaBIOS build failed; $DIR/seabios.log has all of it" >&2
        exit 1
    fi
    cp "$SEABIOS/builds/seabios-256k/bios.bin" "$DIR/share/bios-256k.bin"
fi
echo "$DIR/build/qemu-system-i386"
