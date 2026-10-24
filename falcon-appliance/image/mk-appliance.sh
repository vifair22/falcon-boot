#!/usr/bin/env bash
# falcon-boot: build the on-stick signing appliance as a UKI
# (docs/DESIGN.md Section 7.5).
#
# Output:
#   build/appliance/appliance-uki.efi          unsigned, the artifact mkstick
#                                              signs and stages on p3:/uki
#   build/appliance/appliance-uki-signed.efi   signed with the rig's test MOK,
#                                              for a hand-staged rig stick
# With APPLIANCE_ARCH=aa64, appliance-uki-aa64.efi and its signed copy: the
# same image for an AArch64 machine.
#
# The appliance is a kernel plus an initramfs that contains one program of our
# own (falcon-appliance/src, PID 1) and the programs blessing shells out to
# (DESIGN Section 7.5):
#
#   sbsign       signs the UKI wrap, and appends to a bare payload
#   sbverify     counts the signatures a bare payload carries before and after
#                (DESIGN Section 4.2: blessing one must ADD ours and keep the
#                vendor's, and the count is one of the three checks that says
#                so). Only the payload path runs it, and it refuses up front
#                without it rather than part way through
#
# p3 is opened in process with falcon-core's LUKS2 and ext4
# (falcon-appliance/src/shelf.c), and ISOs are read with its ISO9660 and UDF
# drivers, so neither cryptsetup nor bsdtar is in the image.
#
# plus the portable `sign` tool itself as build/bin/falcon-boot-sign-static,
# which also builds the UKI wrap (falcon-cli/src/sign/uki.c) and the stub it
# wraps with. That is the point of the static link: the appliance route and
# the host route run the SAME implementation (DESIGN Section 8), not two that
# agree today.
#
# Everything else in the image is closure: the shared libraries those programs
# need (walked with ldd, or by their ELF NEEDED entries for arm64, which ldd
# cannot run here), and the kernel modules Fedora ships outside its vmlinuz
# (modules.list, plus what each one's depends= names). The kernel is Fedora's
# for both architectures (tools/fetch-fedora-kernel.sh), so there is one
# source to keep current. Both sd-stubs are in the image, so either
# architecture's appliance blesses either architecture's media. There is no shell, no interpreter, no network tooling, and no
# package manager.
#
# What that costs on this host: 19 MB of initramfs uncompressed, 6.2 MB
# compressed, a 22.9 MB UKI of which 16.6 MB is the kernel, and under ten
# seconds to build. The Windows preparation route (7-Zip with its 7z.so, the
# libstdc++ only it needed, and libwim) is gone: the loader patches Windows
# media at boot instead (DESIGN Section 6.4). ukify's Python runtime (33 MB)
# and the ICU tables behind the host bsdtar's XAR support (32 MB) are gone
# the same way: neither tool runs here.
#
# Usage: falcon-appliance/image/mk-appliance.sh
# Env:
#   APPLIANCE_ARCH     x64 (default) or aa64
#   APPLIANCE_CMDLINE  kernel cmdline baked into the signed image
#   MOK_KEY/MOK_CRT    test MOK for the optional signed copy

set -euo pipefail

cd "$(dirname "$0")/../.."

APPLIANCE_ARCH="${APPLIANCE_ARCH:-x64}"
STUB_DIR=/usr/lib/systemd/boot/efi
case "$APPLIANCE_ARCH" in
x64)
    FARCH=x86_64 SUFFIX= STUB=$STUB_DIR/linuxx64.efi.stub
    SIGN_STATIC=build/bin/falcon-boot-sign-static
    INIT=build/bin/falcon-boot-appliance-init
    TOOLS_ROOT=
    SERIAL_TTY=ttyS1
    ;;
aa64)
    FARCH=aarch64 SUFFIX=-aa64 STUB=$STUB_DIR/linuxaa64.efi.stub
    SIGN_STATIC=build/bin/falcon-boot-sign-static-aa64
    INIT=build/bin/falcon-boot-appliance-init-aa64
    # Debian's arm64 sbsign and sbverify, unpacked in the harness image;
    # their libraries are the arm64 multiarch ones.
    TOOLS_ROOT=/opt/arm64-sbsigntool
    # virt's one serial port carries the loader's log and the typing.
    SERIAL_TTY=ttyAMA0
    ;;
*)
    echo "ERROR: APPLIANCE_ARCH is x64 or aa64" >&2
    exit 2
    ;;
esac

OUT=build/appliance
WORK="$OUT/work$SUFFIX"
ROOT="$WORK/root"
KDIR=build/fedora-kernel/$FARCH
KERNEL="${APPLIANCE_KERNEL:-$KDIR/vmlinuz}"
INITRD="$OUT/appliance-initrd$SUFFIX.zst"
UKI="$OUT/appliance-uki$SUFFIX.efi"
UKI_SIGNED="$OUT/appliance-uki$SUFFIX-signed.efi"

SIGN=build/bin/falcon-boot-sign
MOK_KEY="${MOK_KEY:-build/mok-test/falcon-test-mok.key}"
MOK_CRT="${MOK_CRT:-build/mok-test/falcon-test-mok.crt}"

# Both consoles are named because both are real places a user can be: tty0 is
# the screen of the machine the stick is plugged into, ttyS1 is the port the rig
# types into (ttyS0 carries the firmware and loader log, and is write-only
# there). The appliance opens every console it can and reads from whichever one
# answers (falcon-appliance/src/console.c), so the ORDER of
# these no longer decides whether its prompts can be answered: a machine
# with no ttyS1 would otherwise make /dev/console a dead device and leave
# every prompt reading EOF.
#
# Order still decides one thing: /dev/console is the LAST console= named, and
# that is what the helper tools the appliance runs inherit for their own
# output. tty0 is last so that output lands on the screen of a real machine;
# on a machine with no video the kernel does not register tty0 and it falls
# back to the serial port on its own.
#
# loglevel=3 keeps kernel chatter from interleaving with the prompts, and
# panic=15 makes a panic end in a reboot rather than a hang nobody can exit.
CMDLINE="${APPLIANCE_CMDLINE:-console=$SERIAL_TTY,115200 console=tty0 quiet loglevel=3 panic=15}"

# PID 1 is $INIT above, built by the Makefile (falcon-appliance/build.mk) from
# falcon-appliance/src with the repo's host flag set and linked static: the
# dynamic loader in this image exists for the other programs, not for this one.

# The image's contents are data beside this script: the programs `sign`
# execs (programs.list) and the kernel modules the flow needs by name
# (modules.list). Each file says why every entry is there. One entry per
# line; '#' starts a comment.
IMAGE_DIR=falcon-appliance/image
read_list() {
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' "$1"
}
mapfile -t BINARIES < <(read_list "$IMAGE_DIR/programs.list")

mapfile -t MODULES < <(read_list "$IMAGE_DIR/modules.list")

for t in bsdtar cpio readelf zstd xz strings; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done
for b in "${BINARIES[@]}"; do
    [[ -x "$TOOLS_ROOT$b" ]] || { echo "ERROR: $TOOLS_ROOT$b not found" >&2; exit 1; }
done
[[ -f "$STUB" ]] || {
    echo "ERROR: sd-stub not found at $STUB (install systemd-boot)" >&2
    exit 1
}
[[ -x "$INIT" ]] || {
    echo "ERROR: $INIT not built (run: make appliance)" >&2
    exit 1
}
[[ -x "$SIGN" ]] || {
    echo "ERROR: $SIGN not built (run: make cli)" >&2
    exit 1
}
[[ -f "$SIGN_STATIC" ]] || {
    echo "ERROR: $SIGN_STATIC not built (run: make cli)" >&2
    exit 1
}

START=$(date +%s)
mkdir -p "$OUT"
rm -rf "$WORK"
mkdir -p "$ROOT"

# ---- kernel ----
#
# Fedora's, at the build tools/fetch-fedora-kernel.sh pins: its vmlinuz (an
# EFI-stub bzImage on x86-64, a zboot PE on AArch64) and its core modules.
if [[ ! -f "$KERNEL" || ! -d "$KDIR/modules" ]]; then
    tools/fetch-fedora-kernel.sh "$FARCH"
fi
KVER=$(cat "$KDIR/kver")
echo "   kernel: $KERNEL ($(stat -c%s "$KERNEL") bytes, $KVER)"

# ---- kernel modules ----
#
# Fedora ships exFAT, loop, vfat and the USB storage drivers as modules, so
# the appliance cannot mount p2 or reach into a .img payload without
# carrying them. Decompressed here so /init needs nothing from the kernel
# beyond finit_module; the module signature is inside the .ko and survives,
# which is what lets these load on a kernel that came up under Secure Boot.
# The packages carry no modules.dep (depmod writes it at install), so each
# module's own .modinfo says what it needs (depends=), and anything in
# modules.builtin is in the kernel already.
MODDIR="$KDIR/modules"
BUILTIN="$MODDIR/modules.builtin"

# The module file for a name, as depends= or modules.list spells it: the
# kernel turns '-' into '_' in a module's name, the file keeps its own.
mod_file() {
    local n=$1 alt
    alt=${n//_/-}
    find "$MODDIR/kernel" \( -name "$n.ko.xz" -o -name "$alt.ko.xz" \
         -o -name "${n//-/_}.ko.xz" \) -print -quit
}

is_builtin() {
    local n=$1
    grep -qE "/(${n//_/[-_]}|${n//-/[-_]})\.ko$" "$BUILTIN"
}

declare -A SEEN=()
ORDER=()

# Depth-first, so a module is always staged after everything it needs; /init
# loads the staged files in name order, which is why they get a sequence
# number.
resolve() {
    local name=$1 f deps d
    [[ -n "${SEEN[$name]:-}" ]] && return 0
    SEEN[$name]=1
    is_builtin "$name" && return 0
    f=$(mod_file "$name")
    [[ -n "$f" ]] || { echo "ERROR: no $name module in $MODDIR" >&2; exit 1; }
    deps=$(xz -dc "$f" | strings | sed -n 's/^depends=//p' | head -1)
    for d in ${deps//,/ }; do
        resolve "$d"
    done
    ORDER+=("$f")
}

echo "== resolving kernel modules"
for want in "${MODULES[@]}"; do
    if is_builtin "$want"; then
        echo "   module: $want is built in"
        continue
    fi
    resolve "$want"
done

mkdir -p "$ROOT/modules"
n=0
for f in "${ORDER[@]}"; do
    base=$(basename "$f" .xz)
    dst=$(printf '%s/modules/%02d-%s' "$ROOT" "$n" "$base")
    xz -dc "$f" > "$dst"
    echo "   module: $(basename "$dst") ($(stat -c%s "$dst") bytes)"
    n=$(( n + 1 ))
done

# ---- /init ----
echo "== staging /init"
cp "$INIT" "$ROOT/init"
chmod 0755 "$ROOT/init"
echo "   init: $(stat -c%s "$ROOT/init") bytes (static)"

# ---- the programs the flow shells out to, with their library closure ----
#
# ldd-walk rather than a hand-written library list: a missing .so in an
# initramfs surfaces as "No such file or directory" on a binary that is
# plainly there, which is a bad half hour for whoever meets it next.
echo "== staging binaries and their library closure"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/sbin" "$ROOT/bin" "$ROOT/sbin"

copy_to_root() {
    # $1 absolute path in the image; staged from $2, or from the same path
    # here, because that is what the dynamic loader and every shebang expect.
    local dst="$ROOT$1" src="${2:-$1}"

    mkdir -p "$(dirname "$dst")"
    cp -L "$src" "$dst"
}

# The arm64 libraries, where Debian's multiarch puts them; the arm64 dynamic
# loader searches these by default, so the image needs no cache for them.
A64_LIBDIRS=(/usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu)

a64_find() {
    local d
    for d in "${A64_LIBDIRS[@]}"; do
        [[ -e "$d/$1" ]] && { printf '%s' "$d/$1"; return 0; }
    done
    return 1
}

# Prints the absolute path of every shared object a binary needs. x86-64:
# ldd, which runs the binary's loader. arm64: the NEEDED entries, followed
# by hand through the multiarch directories, since an arm64 loader does not
# run here.
closure() {
    if [[ -z "$TOOLS_ROOT" ]]; then
        ldd "$1" 2>/dev/null |
            sed -n -e 's/.*=> \(\/[^ ]*\).*/\1/p' -e 's/^[[:space:]]*\(\/[^ ]*\) (0x.*/\1/p'
        return
    fi
    local -A seen=()
    local queue=("$1") f lib path
    while [[ ${#queue[@]} -gt 0 ]]; do
        f=${queue[0]}
        queue=("${queue[@]:1}")
        for lib in $(readelf -d "$f" 2>/dev/null |
                     sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
            [[ -n "${seen[$lib]:-}" ]] && continue
            seen[$lib]=1
            path=$(a64_find "$lib") ||
                { echo "ERROR: $lib, which $f needs, is not installed" >&2; exit 1; }
            printf '%s\n' "$path"
            queue+=("$path")
        done
    done
}

# The ELF interpreter a program asks the kernel for (PT_INTERP), or nothing
# for a file that is not a dynamic ELF. Tolerant on purpose: it is also asked
# about scripts, and under pipefail readelf refusing one would end the build.
interp() {
    { readelf -l "$1" 2>/dev/null || true; } |
        sed -n 's/.*Requesting program interpreter: \(\/[^]]*\)\].*/\1/p'
}

LIBS=()
for b in "${BINARIES[@]}"; do
    if [[ "$(head -c2 "$TOOLS_ROOT$b")" == '#!' ]]; then
        echo "ERROR: $b is a script, and the image has no shell to run it" >&2
        exit 1
    fi
    copy_to_root "$b" "$TOOLS_ROOT$b"
    mapfile -t -O "${#LIBS[@]}" LIBS < <(closure "$TOOLS_ROOT$b")
done
mapfile -t LIBS < <(printf '%s\n' "${LIBS[@]}" | sort -u)
for l in "${LIBS[@]}"; do
    copy_to_root "$l"
done

# The dynamic loader, at the path each binary NAMES (PT_INTERP), which is not
# necessarily the path ldd prints. On Arch /lib64 is a symlink into /usr, so
# ldd reports /usr/lib64/ld-linux-x86-64.so.2 while every binary asks the
# kernel for /lib64/ld-linux-x86-64.so.2; staging only the former leaves every
# dynamic program unable to exec, ENOENT on an interpreter path that is
# plainly present under the other name.
for b in "${BINARIES[@]}"; do
    i=$(interp "$ROOT$b")
    if [[ -n "$i" && ! -e "$ROOT$i" ]]; then
        if [[ -n "$TOOLS_ROOT" ]]; then
            copy_to_root "$i" "$(a64_find "$(basename "$i")")"
        else
            copy_to_root "$i"
        fi
    fi
done
echo "   binaries: ${#BINARIES[@]} + bsdtar, libraries: ${#LIBS[@]}"

# The loader finds a library by SONAME through the cache, not by the path ldd
# printed, and this image has no cache of the host's. Without one, a library
# outside the default search path (on this host libstdc++ lives under
# /usr/lib/gcc/...) is staged correctly and still not found at runtime, which
# reads as "cannot open shared object file" for a file that is plainly there.
# So: one ld.so.conf naming every directory a staged library landed in, and a
# cache built from it.
mkdir -p "$ROOT/etc"
printf '%s\n' "${LIBS[@]}" | xargs -r -n1 dirname | sort -u > "$ROOT/etc/ld.so.conf"
# The host's ldconfig builds a cache for its own architecture only; the arm64
# libraries sit in the directories the arm64 loader searches anyway.
if [[ -z "$TOOLS_ROOT" ]]; then
    ldconfig -r "$ROOT" 2>/dev/null || {
        echo "ERROR: ldconfig could not build the image's library cache" >&2
        exit 1
    }
fi

# The tool the whole appliance exists to run, and the stubs it wraps with:
# both, so it blesses an ISO of either architecture.
cp "$SIGN_STATIC" "$ROOT/usr/bin/falcon-boot-sign-static"
copy_to_root "$STUB_DIR/linuxx64.efi.stub"
copy_to_root "$STUB_DIR/linuxaa64.efi.stub"

# ---- the rest of the filesystem ----
#
# Mount points only. /init creates nothing it can avoid creating, but the
# directories have to exist before the first mount.
mkdir -p "$ROOT"/{dev,proc,sys,tmp,run,mnt/p2,mnt/p3,root,etc}
# A minimal passwd so anything that asks who it is gets an answer rather than
# an error; there is exactly one user in here and it is root.
printf 'root:x:0:0:root:/root:/bin/false\n' > "$ROOT/etc/passwd"
printf 'root:x:0:\n' > "$ROOT/etc/group"

# ---- pack ----
#
# zstd because every megabyte of image is a megabyte of RAM at boot; the
# kernel this runs on has CONFIG_RD_ZSTD (verified against the ISO kernel's
# own config).
#
# Every program the image can exec must find its interpreter IN the image:
# the ELF loader it names, or the #! line of a script. Checked over /init and
# the bin directories as staged rather than over the BINARIES list, so a
# program that arrives some other way is held to it too, and through symlinks
# resolved inside the image. Libraries are not exec'd and are not checked.
# This is the check that would have caught the ENOENT failure above before
# it reached a stick.
echo "== checking every staged program's interpreter"
bad=0
while IFS= read -r -d '' f; do
    if [[ -L "$f" ]]; then
        t=$(readlink "$f")
        [[ "$t" == /* ]] && t="$ROOT$t" || t="$(dirname "$f")/$t"
        if [[ ! -e "$t" ]]; then
            echo "ERROR: ${f#"$ROOT"} points at ${t#"$ROOT"}, which is not in" \
                 "the image" >&2
            bad=1
            continue
        fi
        f="$t"
    fi
    i=$(interp "$f")
    if [[ -z "$i" && "$(head -c2 "$f")" == '#!' ]]; then
        i=$(head -n1 "$f" | sed 's/^#![[:space:]]*\([^[:space:]]*\).*/\1/')
    fi
    if [[ -n "$i" && ! -e "$ROOT$i" ]]; then
        echo "ERROR: ${f#"$ROOT"} needs $i, which is not in the image" >&2
        bad=1
    fi
done < <(find "$ROOT/init" "$ROOT"/{bin,sbin,usr/bin,usr/sbin} \
              \( -type f -perm -u+x -o -type l \) -print0)
[[ $bad -eq 0 ]] || exit 1

echo "== packing the initramfs"
( cd "$ROOT" && find . -mindepth 1 -printf '%P\n' | LC_ALL=C sort |
    cpio --quiet -o -H newc --owner=+0:+0 ) | zstd -q -19 -T0 -f -o "$INITRD"
RAW=$(du -sb "$ROOT" | cut -f1)
echo "   initramfs: $INITRD ($(stat -c%s "$INITRD") bytes compressed, $RAW raw)"

# ---- the UKI ----
#
# Built by the sign tool's own UKI builder, the one that wraps every blessed
# kernel, so the image and what it produces are laid out by one
# implementation. The .osrel below names what the image is explicitly, so it
# does not depend on where it was built.
echo "== building the UKI"
APPLIANCE_OSREL=$(mktemp)
cat > "$APPLIANCE_OSREL" <<'OSREL'
NAME="falcon-boot signing appliance"
ID=falcon-boot-appliance
PRETTY_NAME="falcon-boot signing appliance"
OSREL
"$SIGN" uki \
    --linux "$KERNEL" \
    --initrd "$INITRD" \
    --cmdline "$CMDLINE" \
    --stub "$STUB" \
    --os-release "$APPLIANCE_OSREL" \
    --output "$UKI"
rm -f "$APPLIANCE_OSREL"
echo "   unsigned UKI: $UKI ($(stat -c%s "$UKI") bytes)"

# ---- signature ----
#
# The unsigned image is the deliverable: `mkstick create --uki` signs it with
# the stick's own MOK on the way onto p3, which is the product path and the only
# one where the signing key is the key the machine will trust. The signed copy
# below exists for a stick staged by hand (falcon-efi/tools/mk-test-stick.sh
# builds one with a shell script rather than with the tool), where there is no
# mkstick run to do the signing.
rm -f "$UKI_SIGNED"
if [[ -f "$MOK_KEY" && -f "$MOK_CRT" ]]; then
    sbsign --key "$MOK_KEY" --cert "$MOK_CRT" --output "$UKI_SIGNED" "$UKI" \
        >/dev/null
    echo "   signed UKI:   $UKI_SIGNED ($(stat -c%s "$UKI_SIGNED") bytes)"
else
    echo "note: no test MOK at $MOK_KEY; unsigned only" >&2
fi

rm -rf "$WORK"

echo
echo "built in $(( $(date +%s) - START ))s"
echo "stage it with:  falcon-boot-mkstick create ... --uki $UKI"
echo "(mkstick signs it with the stick's own MOK and puts it on p3:/uki,"
echo " where the loader offers it beside the personal OS image.)"
