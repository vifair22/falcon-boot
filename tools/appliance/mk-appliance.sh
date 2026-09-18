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
#   sbsign       signs that wrap, and appends to a bare payload
#   sbverify     counts the signatures a bare payload carries before and after
#                (DESIGN Section 7.2: blessing one must ADD ours and keep the
#                vendor's, and the count is one of the three checks that says
#                so). Only the payload path runs it, and it refuses up front
#                without it rather than part way through
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
# kernel modules Arch ships outside its vmlinuz (dm-crypt, exfat, loop and
# vfat, plus what modules.dep says they need). There is no shell, no network
# tooling, and no package manager.
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

# Both consoles are named because both are real places a user can be: tty0 is
# the screen of the machine the stick is plugged into, ttyS1 is the port the
# rig types into (ttyS0 carries the firmware and loader log, and is write-only
# there). The appliance opens every console it can and reads from whichever
# one answers (tools/appliance/init.c, the console section), so the ORDER of
# these no longer decides whether its prompts can be answered - which it did
# until 2026-09-13, when a laptop with no ttyS1 made /dev/console a dead
# device and every prompt read EOF.
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

# src/disk/part.c identifies p2 and p3 by the falcon type GUIDs, and
# src/bless/manifest.c decides what "already blessed" means. Both are the
# loader's own modules, EFI-free by construction, linked here so the appliance
# cannot disagree with the menu about either question.
#
# The rest is how the survey tells Windows media from everything else:
# src/cli/sign/isoprobe.c is the bless tool's own ISO reader (two helper
# listings, unioned; no mount and no loop device) and src/iso/family.c is
# the registry it fingerprints against, so the queue an image lands in here is
# the queue `falcon-boot-sign` will agree with when it is handed the file.
# util.c and the two crypto units come along because isoprobe calls into them,
# not as anything this program uses directly.
# src/cli/sign/payload.c comes in for two answers the survey needs and must
# not have its own version of: which file names are bare payloads, and whether
# one already carries this stick's certificate (DESIGN Section 7.2). The
# signing itself still happens in the tool, in its own process.
INIT_SRCS=(
    tools/appliance/init.c
    tools/appliance/route.c
    src/disk/part.c
    src/bless/manifest.c
    src/iso/family.c
    src/cli/sign/isoprobe.c
    src/cli/sign/payload.c
    src/cli/sign/util.c
    src/crypto/sha256.c
    src/crypto/crypto.c
)

# The programs `sign` execs, plus the interpreter ukify is written in. Order
# does not matter; the closure walk below takes them as a set.
BINARIES=(
    /usr/bin/cryptsetup
    /usr/bin/sbsign
    /usr/bin/sbverify
    /usr/bin/bsdtar
    /usr/bin/7z
    /usr/bin/wimlib-imagex
)

# Wanted by name; their dependencies are resolved from the kernel's own
# modules.dep below. dm-crypt alone pulls in five modules on this kernel
# (encrypted-keys, trusted, asn1_encoder, tee), which is exactly the kind of
# list that should not be maintained by hand.
#
# The appliance's hardware needs are exactly three: reach the stick, draw on a
# screen, and read a keyboard. The rig can only ever test the middle one, since
# QEMU hands the stick over as emulated SATA and the keyboard as PS/2, and both
# of those drivers are built into the kernel. So the first and the third are
# carried here on the strength of what a real machine looks like, not on the
# strength of a green test run.
#
# Reach the stick: the rig's emulated SATA hid the gap until the appliance met
# actual hardware (2026-09-13) and found no block device to survey. A falcon
# stick is a USB stick by definition, so the bus it arrives on is not optional.
# Both drivers are listed because which one claims a given bridge is the
# bridge's choice, not ours.
#
# Read a keyboard: USB (usbhid, hid-generic) and PS/2 (i8042, atkbd) are built
# in, but i2c-hid is not, and a great many modern laptops drive their built-in
# keyboard over it. Without these the appliance would boot, print its prompts
# perfectly, and ignore every keystroke - which looks exactly like the console
# bug above and would send the next person chasing it again. i2c-hid-of is
# device-tree and cannot apply on x86; hid-multitouch is a touchpad and nothing
# here needs a pointer.
#
# Write into a .img payload: blessing one signs the loader inside the image's
# own filesystem, and src/cli/sign/payload.c does that through the kernel
# rather than through mtools - a loop device configured for the partition's
# byte range, mounted vfat. Neither driver is built in (CONFIG_BLK_DEV_LOOP=m,
# CONFIG_VFAT_FS=m, read out of this kernel's own IKCONFIG), and vfat's
# dependency on fat is resolved below rather than listed.
#
# What is deliberately NOT listed is the character sets that mount needs. A
# vfat mount with no options loads the codepage and the I/O charset the kernel
# was configured with, and a missing one fails the mount with a message about
# a charset rather than about the image; here they are CONFIG_FAT_DEFAULT_
# CODEPAGE=437 and CONFIG_FAT_DEFAULT_IOCHARSET="ascii", and both nls_cp437
# and nls_ascii are BUILT IN on this kernel (they appear in modules.builtin,
# not in modules.dep). Naming either would be a build failure by the rule
# below, which is the resolver doing its job.
#
# Names are the ones modules.dep uses, hyphens and all, because the resolver
# below looks them up literally rather than going through modprobe's aliasing.
# Only list what is actually a module: the host controller (xhci-pci, xhci-hcd) and the SCSI disk layer
# (sd_mod) are built into Arch's kernel and naming them here is a build
# failure, correctly, since the same failure is what a typo deserves.
MODULES=(dm-crypt exfat loop vfat usb-storage uas i2c-hid i2c-hid-acpi)

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

for t in bsdtar cpio gcc readelf ukify zstd sudo; do
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

# ukify's shebang names its interpreter, and the appliance copies that binary
# in (see BINARIES below), so what is wanted is the real executable. A shebang
# may name it through env - Arch ships "#!/usr/bin/env python3" - and env is a
# launcher, not an interpreter: copying it into the appliance would put a
# program there that looks up a PATH the appliance does not have.
UKIFY=$(command -v ukify) || { echo "ERROR: ukify not on PATH" >&2; exit 1; }
PYTHON=$(head -1 "$UKIFY" | sed 's|^#!||; s|^[[:space:]]*||')
case "$PYTHON" in
    */env\ *) PYTHON=$(command -v "${PYTHON#* }") ;;
esac
PYTHON=$(readlink -f "$PYTHON" 2>/dev/null)
[[ -n "$PYTHON" && -x "$PYTHON" ]] || {
    echo "ERROR: could not resolve ukify's interpreter from $UKIFY" >&2
    exit 1
}
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

# The ELF interpreter a program asks the kernel for (PT_INTERP), or nothing
# for a file that is not a dynamic ELF. Tolerant on purpose: it is also asked
# about scripts, and under pipefail readelf refusing one would end the build.
interp() {
    { readelf -l "$1" 2>/dev/null || true; } |
        sed -n 's/.*Requesting program interpreter: \(\/[^]]*\)\].*/\1/p'
}

# What a one-line exec wrapper runs: a script of the form
#   #!/bin/sh
#   exec /abs/path "$@"
# prints /abs/path. Anything else prints nothing. Arch's 7zip package ships
# /usr/bin/7z as exactly this (exec /usr/lib/7zip/7z), and there is no shell
# in the image to run it, so the program a wrapper execs is staged AT the
# wrapper's path instead, with the shared objects from its own directory
# beside it: 7-Zip finds 7z.so next to argv[0], not next to the resolved
# executable, so a symlink runs but loads no formats and reports every ISO as
# "Cannot open the file as archive" (measured 2026-09-17). The Gentoo host
# this was first built on ships the real program at /usr/bin/7z, which is why
# a plain copy used to be enough.
unwrap() {
    [[ "$(head -c2 "$1")" == '#!' ]] || return 0
    sed -n 's/^exec \(\/[^ "]*\) "\$@"$/\1/p' "$1" | head -n1
}

LIBS=()
for b in "${BINARIES[@]}" "$PYTHON"; do
    t=$(unwrap "$b")
    if [[ -n "$t" ]]; then
        mkdir -p "$(dirname "$ROOT$b")"
        cp -L "$t" "$ROOT$b"
        mapfile -t -O "${#LIBS[@]}" LIBS < <(closure "$t")
        # The wrapped program's own directory is its private library
        # directory, dlopened at run time where ldd cannot see it, so its
        # shared objects come too, beside the staged program, with their
        # closure.
        for so in "$(dirname "$t")"/*.so; do
            [[ -e "$so" ]] || continue
            cp -L "$so" "$(dirname "$ROOT$b")/"
            mapfile -t -O "${#LIBS[@]}" LIBS < <(closure "$so")
        done
        continue
    fi
    if [[ "$(head -c2 "$b")" == '#!' && "$b" != "$PYTHON" ]]; then
        echo "ERROR: $b is a script this builder cannot unwrap, and the" \
             "image has no shell to run it" >&2
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
# ENOENT: bsdtar, 7z and cryptsetup all "missing" while plainly present. The
# Gentoo host this was first built on has a real /lib64, which is why the
# walk above was enough there.
for b in "${BINARIES[@]}" "$PYTHON"; do
    i=$(interp "$ROOT$b")
    if [[ -n "$i" && ! -e "$ROOT$i" ]]; then
        copy_to_root "$i"
    fi
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

# ukify, with its shebang pointed at the interpreter resolved above. The sign
# tool execs ukify directly, so the kernel reads that line, and Arch ships
# "#!/usr/bin/env python3": an image without env (there is none, see the PYTHON
# note) fails that exec with ENOENT. Gentoo's names python outright, which is
# why a straight copy worked until the stick was rebuilt on Arch.
{ printf '#!%s\n' "$PYTHON"; tail -n +2 "$UKIFY"; } > "$ROOT/usr/bin/ukify"
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
# built on this stick is the stick.
#
# falcon-boot reads .osrel to label menu entries (src/pos/osrel.c), and so does
# sd-boot, so this string is user-visible rather than decorative.
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
# Every program the image can exec must find its interpreter IN the image:
# the ELF loader it names, or the #! line of a script. Checked over /init and
# the bin directories as staged rather than over the BINARIES list, so a
# program that arrives some other way is held to it too, and through symlinks
# resolved inside the image. Libraries and the Python tree's own scripts are
# not exec'd and are not checked. This is the check that would have stopped
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
# --os-release is passed EXPLICITLY. Without it ukify falls back to the build
# host's /usr/lib/os-release, and the appliance then carries whatever distro
# built it: the first one built on the Gentoo workstation announced itself in
# the loader menu as "Gentoo Linux", which is both wrong and the least useful
# thing that entry could say. What it is does not depend on where it was made.
echo "== ukify build"
APPLIANCE_OSREL=$(mktemp)
cat > "$APPLIANCE_OSREL" <<'OSREL'
NAME="falcon-boot signing appliance"
ID=falcon-boot-appliance
PRETTY_NAME="falcon-boot signing appliance"
OSREL
ukify build \
    --linux="$KERNEL" \
    --initrd="$INITRD" \
    --cmdline="$CMDLINE" \
    --stub="$STUB" \
    --os-release="@$APPLIANCE_OSREL" \
    --output="$UKI" >/dev/null
rm -f "$APPLIANCE_OSREL"
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
