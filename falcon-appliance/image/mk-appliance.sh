#!/usr/bin/env bash
# falcon-boot: build the on-stick signing appliance as a UKI
# (docs/DESIGN.md Section 7.5, build order step 8).
#
# Output:
#   build/appliance/appliance-uki.efi          unsigned, the artifact mkstick
#                                              signs and stages on p3:/uki
#   build/appliance/appliance-uki-signed.efi   signed with the rig's test MOK,
#                                              for a hand-staged rig stick
#
# The appliance is a kernel plus an initramfs that contains one program of our
# own (falcon-appliance/src, PID 1) and the programs blessing shells out to
# (DESIGN Section 7.5):
#
#   sbsign       signs the UKI wrap, and appends to a bare payload
#   sbverify     counts the signatures a bare payload carries before and after
#                (DESIGN Section 7.2: blessing one must ADD ours and keep the
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
# need (walked with ldd), and the kernel modules Arch ships outside its
# vmlinuz (exfat, loop and vfat, plus what modules.dep says they need). There is no shell, no interpreter, no network tooling, and no
# package manager.
#
# What that costs, measured 2026-09-19 on this host: 19 MB of initramfs
# uncompressed, 6.2 MB compressed, a 22.9 MB UKI of which 16.6 MB is the
# kernel, and under ten seconds to build. The day before it was 27 MB: the
# Windows preparation route (7-Zip with its 7z.so, the libstdc++ only it
# needed, and libwim) went when the loader took over patching Windows media
# (DESIGN Section 6.4). Before 2026-09-18 the initramfs was 97 MB: ukify's
# Python runtime (33 MB) and the ICU tables behind the host bsdtar's XAR
# support (32 MB) are what went.
#
# Usage: falcon-appliance/image/mk-appliance.sh
# Env:
#   ARCH_ISO      kernel and module source (default: the vtoy-backup copy)
#   APPLIANCE_CMDLINE  kernel cmdline baked into the signed image
#   MOK_KEY/MOK_CRT    test MOK for the optional signed copy
#
# Needs passwordless sudo: the kernel modules live inside the ISO's squashfs,
# which is read through two loop mounts (nothing is written to either).

set -euo pipefail

cd "$(dirname "$0")/../.."

ARCH_ISO="${ARCH_ISO:-/mnt/980pro/vtoy-backup/Linux/archlinux-2026.01.01-x86_64.iso}"
ISO_KERNEL_PATH=arch/boot/x86_64/vmlinuz-linux
ISO_SFS_PATH=arch/x86_64/airootfs.sfs
STUB=/usr/lib/systemd/boot/efi/linuxx64.efi.stub

OUT=build/appliance
WORK="$OUT/work"
ROOT="$WORK/root"
KERNEL="${APPLIANCE_KERNEL:-build/test/fixture-vmlinuz}"
INITRD="$OUT/appliance-initrd.zst"
UKI="$OUT/appliance-uki.efi"
UKI_SIGNED="$OUT/appliance-uki-signed.efi"

SIGN=build/bin/falcon-boot-sign
SIGN_STATIC=build/bin/falcon-boot-sign-static
MOK_KEY="${MOK_KEY:-build/mok-test/falcon-test-mok.key}"
MOK_CRT="${MOK_CRT:-build/mok-test/falcon-test-mok.crt}"

# Both consoles are named because both are real places a user can be: tty0 is
# the screen of the machine the stick is plugged into, ttyS1 is the port the rig
# types into (ttyS0 carries the firmware and loader log, and is write-only
# there). The appliance opens every console it can and reads from whichever one
# answers (falcon-appliance/src/console.c), so the ORDER of
# these no longer decides whether its prompts can be answered - which it did
# until 2026-09-13, when a laptop with no ttyS1 made /dev/console a dead device
# and every prompt read EOF.
#
# Order still decides one thing: /dev/console is the LAST console= named, and
# that is what the helper tools the appliance runs inherit for their own
# output. tty0 is last so that output lands on the screen of a real machine;
# on a machine with no video the kernel does not register tty0 and it falls
# back to the serial port on its own.
#
# loglevel=3 keeps kernel chatter from interleaving with the prompts, and
# panic=15 makes a panic end in a reboot rather than a hang nobody can exit.
CMDLINE="${APPLIANCE_CMDLINE:-console=ttyS1,115200 console=tty0 quiet loglevel=3 panic=15}"

# PID 1, built by the Makefile (falcon-appliance/build.mk) from
# falcon-appliance/src with the repo's host flag set and linked static: the
# dynamic loader in this image exists for the other programs, not for this one.
INIT=build/bin/falcon-boot-appliance-init

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

WORK_MNT_ISO=
WORK_MNT_SFS=

cleanup() {
    if [[ -n "$WORK_MNT_SFS" ]]; then
        sudo umount "$WORK_MNT_SFS" 2>/dev/null || true
        rmdir "$WORK_MNT_SFS" 2>/dev/null || true
        WORK_MNT_SFS=
    fi
    if [[ -n "$WORK_MNT_ISO" ]]; then
        sudo umount "$WORK_MNT_ISO" 2>/dev/null || true
        rmdir "$WORK_MNT_ISO" 2>/dev/null || true
        WORK_MNT_ISO=
    fi
}
trap cleanup EXIT

for t in bsdtar cpio readelf zstd sudo; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done
for b in "${BINARIES[@]}"; do
    [[ -x "$b" ]] || { echo "ERROR: $b not found" >&2; exit 1; }
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
# The same Arch vmlinuz the fixture UKI uses, extracted the same way
# (falcon-efi/tools/mk-test-uki.sh): bsdtar reads ISO9660 directly, so no mount
# and no sudo, and the ISO is only ever read.
if [[ ! -f "$KERNEL" ]]; then
    [[ -f "$ARCH_ISO" ]] || {
        echo "ERROR: $ARCH_ISO not found (set ARCH_ISO or APPLIANCE_KERNEL)" >&2
        exit 1
    }
    echo "== extracting $ISO_KERNEL_PATH"
    mkdir -p "$(dirname "$KERNEL")"
    bsdtar -x -f "$ARCH_ISO" -C "$WORK" "$ISO_KERNEL_PATH"
    cp "$WORK/$ISO_KERNEL_PATH" "$KERNEL"
fi
KVER=$(file -b "$KERNEL" | sed -n 's/.*version \([^ ]*\).*/\1/p')
[[ -n "$KVER" ]] || { echo "ERROR: cannot read the kernel version of $KERNEL" >&2; exit 1; }
echo "   kernel: $KERNEL ($(stat -c%s "$KERNEL") bytes, $KVER)"

# ---- kernel modules ----
#
# Arch ships exFAT, loop and vfat as modules, so the appliance cannot mount
# p2 or reach into a .img payload without carrying them. They live in the
# ISO's squashfs rather than in its initramfs, hence the two read-only loop
# mounts. Decompressed here so /init needs nothing from the kernel beyond
# finit_module; the module signature is inside the .ko and survives, which is
# what lets these load on a kernel that came up under Secure Boot.
echo "== extracting kernel modules from $(basename "$ARCH_ISO")"
[[ -f "$ARCH_ISO" ]] || { echo "ERROR: $ARCH_ISO not found" >&2; exit 1; }
WORK_MNT_ISO=$(mktemp -d)
sudo mount -o loop,ro "$ARCH_ISO" "$WORK_MNT_ISO"
WORK_MNT_SFS=$(mktemp -d)
sudo mount -o loop,ro "$WORK_MNT_ISO/$ISO_SFS_PATH" "$WORK_MNT_SFS"

MODDIR="$WORK_MNT_SFS/usr/lib/modules/$KVER"
[[ -d "$MODDIR" ]] || {
    echo "ERROR: the ISO has no modules for $KVER (found: $(ls "$WORK_MNT_SFS/usr/lib/modules" 2>/dev/null))" >&2
    exit 1
}
DEPFILE="$MODDIR/modules.dep"
[[ -f "$DEPFILE" ]] || { echo "ERROR: no modules.dep in the ISO's module tree" >&2; exit 1; }

# Depth-first over modules.dep so a module is always staged after everything
# it needs. /init loads the staged files in name order, which is why they get
# a sequence number: dependency order is decided here, once, against the
# kernel's own metadata, rather than rediscovered in PID 1.
declare -A SEEN=()
ORDER=()

resolve() {
    local mod="$1" deps d
    [[ -n "${SEEN[$mod]:-}" ]] && return 0
    SEEN[$mod]=1
    deps=$(awk -v m="$mod" -F':' '$1 == m { print $2 }' "$DEPFILE")
    # Listed nearest-first, so the far end of the list is loaded first.
    for d in $(printf '%s\n' $deps | tac); do
        resolve "$d"
    done
    ORDER+=("$mod")
}

for want in "${MODULES[@]}"; do
    path=$(awk -F':' -v w="/$want.ko.zst" \
        'index($1, w) == length($1) - length(w) + 1 { print $1; exit }' "$DEPFILE")
    [[ -n "$path" ]] || { echo "ERROR: no $want module in the ISO's tree" >&2; exit 1; }
    resolve "$path"
done

mkdir -p "$ROOT/modules"
n=0
for m in "${ORDER[@]}"; do
    src="$MODDIR/$m"
    [[ -f "$src" ]] || { echo "ERROR: $m missing from the ISO's module tree" >&2; exit 1; }
    base=$(basename "$m" .zst)
    dst=$(printf '%s/modules/%02d-%s' "$ROOT" "$n" "$base")
    zstd -dqf "$src" -o "$dst"
    echo "   module: $(basename "$dst") ($(stat -c%s "$dst") bytes)"
    n=$(( n + 1 ))
done
cleanup

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
    # $1 absolute source path; staged at the same absolute path in the image,
    # because that is what the dynamic loader and every shebang expect.
    local src="$1"
    local dst="$ROOT$1"

    mkdir -p "$(dirname "$dst")"
    cp -L "$src" "$dst"
}

closure() {
    # Prints the absolute path of every shared object a binary needs,
    # including the dynamic loader itself.
    ldd "$1" 2>/dev/null |
        sed -n -e 's/.*=> \(\/[^ ]*\).*/\1/p' -e 's/^[[:space:]]*\(\/[^ ]*\) (0x.*/\1/p'
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
    if [[ "$(head -c2 "$b")" == '#!' ]]; then
        echo "ERROR: $b is a script, and the image has no shell to run it" >&2
        exit 1
    fi
    copy_to_root "$b"
    mapfile -t -O "${#LIBS[@]}" LIBS < <(closure "$b")
done
mapfile -t LIBS < <(printf '%s\n' "${LIBS[@]}" | sort -u)
for l in "${LIBS[@]}"; do
    copy_to_root "$l"
done

# The dynamic loader, at the path each binary NAMES (PT_INTERP), which is not
# necessarily the path ldd prints. On Arch /lib64 is a symlink into /usr, so
# ldd reports /usr/lib64/ld-linux-x86-64.so.2 while every binary asks the
# kernel for /lib64/ld-linux-x86-64.so.2. Staging only the former built an
# image (2026-09-15) in which every dynamic program failed to exec with
# ENOENT: bsdtar and cryptsetup both "missing" while plainly present. The
# Gentoo host this was first built on has a real /lib64, which is why the
# walk above was enough there.
for b in "${BINARIES[@]}"; do
    i=$(interp "$ROOT$b")
    if [[ -n "$i" && ! -e "$ROOT$i" ]]; then
        copy_to_root "$i"
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
ldconfig -r "$ROOT" 2>/dev/null || {
    echo "ERROR: ldconfig could not build the image's library cache" >&2
    exit 1
}

# The tool the whole appliance exists to run, and the stub it wraps with.
cp "$SIGN_STATIC" "$ROOT/usr/bin/falcon-boot-sign-static"
copy_to_root "$STUB"

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
# resolved inside the image. Libraries are not exec'd and are not checked. This is the check that would have stopped
# the 2026-09-15 image before it reached a stick.
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
# implementation. The .osrel names what the image is, which does not depend on
# where it was built: the first appliance, built with ukify's host default on
# the Gentoo workstation, announced itself in the loader menu as "Gentoo
# Linux".
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
