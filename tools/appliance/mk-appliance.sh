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
# own (tools/appliance/init.c, PID 1) and the programs the two flows shell out
# to. Blessing (DESIGN Section 7.5):
#
#   cryptsetup   unlocks p3 so the MOK private key can be read
#   bsdtar       lists and extracts the ISO (this is what `sign` reads with)
#   ukify        wraps the extracted kernel into a UKI
#   sbsign       signs that wrap
#
# and preparing Windows media (DESIGN Sections 6.3 and 6.3.1), which holds no
# key and signs nothing:
#
#   7z             reads the UDF namespace. Windows install media keeps every
#                  real file there and only a stub in ISO9660, so without this
#                  such an ISO lists as almost empty and classifies as unknown
#   wimlib-imagex  rewrites \sources\boot.wim: extract, update, optimize,
#                  verify. The separately installed wimupdate/wimoptimize
#                  names are aliases for its subcommands, so the one binary is
#                  the whole dependency
#   winjump.exe    not run here at all. It is the Win64 binary that replaces
#                  WinPE's winpeshl.exe inside the patched image, and it is
#                  staged beside the sign tool because that is the first place
#                  src/cli/sign/prepare.c looks for it
#
# plus the portable `sign` tool itself as build/bin/falcon-boot-sign-static.
# That is the point of the static link: the appliance route and the host route
# run the SAME implementation (DESIGN Section 8), not two that agree today.
#
# Everything else in the image is closure: the shared libraries those programs
# need (walked with ldd), the Python runtime ukify is written in, and the
# kernel modules Arch ships outside its vmlinuz (dm-crypt and exfat, plus what
# modules.dep says they need). There is no shell, no network tooling, and no
# package manager.
#
# What that costs, measured 2026-09-13 on this host: 83 MB of initramfs
# uncompressed, 24 MB compressed, a 40 MB UKI next to a 17 MB kernel, and ten
# seconds to build. Preparation's whole share of that is 3.5 MB raw and 1.3 MB
# compressed: 7-Zip is 2.9 MB of it and libwim 0.4 MB, and the libstdc++ 7-Zip
# needs was already in the image. That is the entire price of the Windows route
# living on the stick rather than on a host. The two large single items are
# still the Python tree (16 MB) and libicudata (32 MB), which libxml2 drags in
# behind libarchive; the ICU tables are never read and the way to be rid of
# them is a libarchive built without xar support, not a smaller Python. See the
# PYTHON note below.
#
# Usage: tools/appliance/mk-appliance.sh
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

SIGN_STATIC=build/bin/falcon-boot-sign-static
WINJUMP=build/bin/winjump.exe
MOK_KEY="${MOK_KEY:-build/mok-test/falcon-test-mok.key}"
MOK_CRT="${MOK_CRT:-build/mok-test/falcon-test-mok.crt}"

# The rig's console is a second QEMU serial port (ttyS1); ttyS0 carries the
# firmware and loader log, and the appliance needs a port it can READ from.
# A product build would drop ttyS1 and keep console=tty0 alone. loglevel=3
# keeps kernel chatter from interleaving with the confirmation prompts, and
# panic=15 makes a panic end in a reboot rather than a hang nobody can exit.
CMDLINE="${APPLIANCE_CMDLINE:-console=tty0 console=ttyS1,115200 quiet loglevel=3 panic=15}"

# Host flag set from the Makefile (HOST_CFLAGS), copied verbatim: /init is
# ordinary Linux userland C and is held to exactly the same warnings as the
# rest of src/cli. -static because it is PID 1 in an initramfs; the dynamic
# loader in here exists for the other four programs, not for this one.
HOST_CFLAGS=(
    -std=c11 -O2 -D_GNU_SOURCE
    -Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion
    -Wformat=2 -Wformat-truncation
    -Wmissing-prototypes -Wstrict-prototypes
    -Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference
    -Wconversion -Wsign-conversion
    -fstack-protector-strong -fstack-clash-protection
    -Isrc -Itools
)

# src/part/part.c identifies p2 and p3 by the falcon type GUIDs, and
# src/manifest/manifest.c decides what "already blessed" means. Both are the
# loader's own modules, EFI-free by construction, linked here so the appliance
# cannot disagree with the menu about either question.
#
# The rest is how the survey tells Windows media from everything else:
# src/cli/sign/isoprobe.c is the bless tool's own ISO reader (two helper
# listings, unioned; no mount and no loop device) and src/family/family.c is
# the registry it fingerprints against, so the queue an image lands in here is
# the queue `falcon-boot-sign` will agree with when it is handed the file.
# util.c and the two crypto units come along because isoprobe calls into them,
# not as anything this program uses directly.
INIT_SRCS=(
    tools/appliance/init.c
    tools/appliance/route.c
    src/part/part.c
    src/manifest/manifest.c
    src/family/family.c
    src/cli/sign/isoprobe.c
    src/cli/sign/util.c
    src/crypto/sha256.c
    src/crypto/crypto.c
)

# The programs `sign` execs, plus the interpreter ukify is written in. Order
# does not matter; the closure walk below takes them as a set.
BINARIES=(
    /usr/bin/cryptsetup
    /usr/bin/sbsign
    /usr/bin/bsdtar
    /usr/bin/7z
    /usr/bin/wimlib-imagex
)

# Wanted by name; their dependencies are resolved from the kernel's own
# modules.dep below. dm-crypt alone pulls in five modules on this kernel
# (encrypted-keys, trusted, asn1_encoder, tee), which is exactly the kind of
# list that should not be maintained by hand.
MODULES=(dm-crypt exfat)

# PYTHON: ukify is a Python program, so the appliance carries an interpreter.
# Everything here is dropped rather than kept: test suites, the GUI and REPL
# support, the documentation database, and anything that exists to install
# more Python. The result is measured at the end of this script; if it ever
# stops being a fraction of the kernel it sits next to, the conversation to
# have is about a compiled ukify, not about trimming this list further.
PY_PRUNE=(
    test idlelib tkinter turtledemo pydoc_data ensurepip lib2to3 venv
    __pycache__ sqlite3 dbm wsgiref xmlrpc zoneinfo curses asyncio
    multiprocessing unittest distutils
)

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

for t in bsdtar cpio gcc ukify zstd sudo; do
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
[[ -f "$SIGN_STATIC" ]] || {
    echo "ERROR: $SIGN_STATIC not built (run: make cli)" >&2
    exit 1
}
[[ -f "$WINJUMP" ]] || {
    echo "ERROR: $WINJUMP not built (run: make winjump). Without it the" >&2
    echo "       appliance can survey Windows media but not prepare it." >&2
    exit 1
}

PYTHON=$(head -1 /usr/bin/ukify | sed 's|^#!||')
[[ -x "$PYTHON" ]] || { echo "ERROR: ukify's interpreter $PYTHON is missing" >&2; exit 1; }
PY_VER=$("$PYTHON" -c 'import sys; print("python%d.%d" % sys.version_info[:2])')
PY_LIB="/usr/lib/$PY_VER"
[[ -d "$PY_LIB" ]] || { echo "ERROR: no standard library at $PY_LIB" >&2; exit 1; }
PEFILE=$("$PYTHON" -c 'import pefile; print(pefile.__file__)' 2>/dev/null) || {
    echo "ERROR: ukify needs the pefile module and it is not installed" >&2
    exit 1
}

START=$(date +%s)
mkdir -p "$OUT"
rm -rf "$WORK"
mkdir -p "$ROOT"

# ---- kernel ----
#
# The same Arch vmlinuz the fixture UKI uses, extracted the same way
# (tools/mk-test-uki.sh): bsdtar reads ISO9660 directly, so no mount and no
# sudo, and the ISO is only ever read.
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
# Arch builds ext4 into its vmlinuz but ships device-mapper and exFAT as
# modules, so the appliance cannot unlock p3 or mount p2 without carrying
# them. They live in the ISO's squashfs rather than in its initramfs (which
# has no dm-crypt: archiso never unlocks anything), hence the two read-only
# loop mounts. Decompressed here so /init needs nothing from the kernel beyond
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
echo "== building /init"
gcc "${HOST_CFLAGS[@]}" -static -o "$ROOT/init" "${INIT_SRCS[@]}"
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

LIBS=()
for b in "${BINARIES[@]}" "$PYTHON"; do
    copy_to_root "$b"
    mapfile -t -O "${#LIBS[@]}" LIBS < <(closure "$b")
done
mapfile -t LIBS < <(printf '%s\n' "${LIBS[@]}" | sort -u)
for l in "${LIBS[@]}"; do
    copy_to_root "$l"
done
echo "   binaries: ${#BINARIES[@]} + $PY_VER, libraries: ${#LIBS[@]}"

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

# Beside the tool, because that is where src/cli/sign/prepare.c looks first
# (find_winjump resolves /proc/self/exe and tries its directory). It is a PE
# for WinPE to run, never anything this image executes.
cp "$WINJUMP" "$ROOT/usr/bin/winjump.exe"
chmod 0644 "$ROOT/usr/bin/winjump.exe"

# ukify, at the path its shebang names.
cp /usr/bin/ukify "$ROOT/usr/bin/ukify"
chmod 0755 "$ROOT/usr/bin/ukify"

# ---- the Python runtime ukify needs ----
echo "== staging $PY_VER (ukify is a Python program)"
mkdir -p "$ROOT$PY_LIB"
EXCL=()
for p in "${PY_PRUNE[@]}"; do
    EXCL+=(--exclude="$p")
done
tar -C "$PY_LIB" -cf - "${EXCL[@]}" --exclude=site-packages . |
    tar -C "$ROOT$PY_LIB" -xf -
mkdir -p "$ROOT$PY_LIB/site-packages"
cp "$PEFILE" "$ROOT$PY_LIB/site-packages/"
# pefile's only dependency, a package of Windows ordinal tables.
if [[ -d "$(dirname "$PEFILE")/ordlookup" ]]; then
    cp -r "$(dirname "$PEFILE")/ordlookup" "$ROOT$PY_LIB/site-packages/"
fi
find "$ROOT$PY_LIB" -name '__pycache__' -type d -prune -exec rm -rf {} +
echo "   python tree: $(du -sh "$ROOT$PY_LIB" | cut -f1)"

# ---- the rest of the filesystem ----
#
# Mount points only. /init creates nothing it can avoid creating, but the
# directories have to exist before the first mount.
mkdir -p "$ROOT"/{dev,proc,sys,tmp,run,mnt/p2,mnt/p3,root,etc}
# A minimal passwd so anything that asks who it is gets an answer rather than
# an error; there is exactly one user in here and it is root.
printf 'root:x:0:0:root:/root:/bin/false\n' > "$ROOT/etc/passwd"
printf 'root:x:0:\n' > "$ROOT/etc/group"

# ukify reads /usr/lib/os-release to fill the .osrel section of everything it
# builds, and refuses to run without it. On a host that file describes the
# host; here it has to describe something, and the honest answer for a wrap
# built on this stick is the stick. Nothing in falcon-boot reads .osrel (the
# loader labels entries from the manifest); sd-boot would show PRETTY_NAME.
cat > "$ROOT/usr/lib/os-release" <<'OSREL'
NAME="falcon-boot blessed media"
ID=falcon-boot
PRETTY_NAME="falcon-boot blessed media"
OSREL

# ---- pack ----
#
# zstd because the initramfs is dominated by the Python tree and an extra
# 30 MB of image is 30 MB of RAM at boot; the kernel this runs on has
# CONFIG_RD_ZSTD (verified against the ISO kernel's own config).
echo "== packing the initramfs"
( cd "$ROOT" && find . -mindepth 1 -printf '%P\n' | LC_ALL=C sort |
    cpio --quiet -o -H newc --owner=+0:+0 ) | zstd -q -19 -T0 -f -o "$INITRD"
RAW=$(du -sb "$ROOT" | cut -f1)
echo "   initramfs: $INITRD ($(stat -c%s "$INITRD") bytes compressed, $RAW raw)"

# ---- the UKI ----
echo "== ukify build"
ukify build \
    --linux="$KERNEL" \
    --initrd="$INITRD" \
    --cmdline="$CMDLINE" \
    --stub="$STUB" \
    --output="$UKI" >/dev/null
echo "   unsigned UKI: $UKI ($(stat -c%s "$UKI") bytes)"

# ---- signature ----
#
# The unsigned image is the deliverable: `mkstick create --uki` signs it with
# the stick's own MOK on the way onto p3, which is the product path and the
# only one where the signing key is the key the machine will trust. The signed
# copy below exists for a stick staged by hand (tools/mk-test-stick.sh
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
