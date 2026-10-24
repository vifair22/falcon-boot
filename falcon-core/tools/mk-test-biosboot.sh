#!/usr/bin/env bash
# falcon-boot: build the ISOs falcon-core/tests/biosboot_test.c reads
# (falcon-core/src/media/biosboot.h: can falcon-bios boot this, and does it
# need a 64-bit processor).
#
# Each is the smallest medium with the shape in question. The kernels are
# not kernels: the rule reads a kernel's head only, so a head is what each
# one is (the boot sector signature, "HdrS" at 0x202, the protocol version
# at 0x206 and xloadflags at 0x236), and a BSD kernel is an ELF header.
#
#   build/test-bios-efionly.iso    a UEFI El Torito image only, no boot
#                                  configuration: boots NO ({EFI ONLY})
#   build/test-bios-arm64.iso      an ARM64 medium: \EFI\BOOT\BOOTAA64.EFI
#                                  in its tree and in a UEFI El Torito
#                                  image, nothing else: boots NO, and only
#                                  ARM64's UEFI can ({ARM64 ONLY})
#   build/test-bios-data.iso       a data disc: no boot record, no
#                                  configuration, no UEFI loader; neither
#                                  loader can start it, so boots is UNKNOWN
#   build/test-bios-efikernel.iso  the same plus a grub.cfg whose kernel is
#                                  a 64-bit bzImage: boots YES (the direct
#                                  route starts it), needs64 YES
#   build/test-bios-64.iso         isolinux, two entries, both kernels
#                                  64-bit: needs64 YES
#   build/test-bios-multi.iso      isolinux, a 64-bit and a 32-bit kernel
#                                  (a multi-arch medium): needs64 NO
#   build/test-bios-32.iso         isolinux, a 32-bit kernel: needs64 NO
#   build/test-bios-old.iso        isolinux, a 64-bit kernel and one whose
#                                  header predates xloadflags: needs64
#                                  UNKNOWN
#   build/test-bios-memtest.iso    isolinux, a 64-bit kernel and a memory
#                                  tester that is no bzImage: the tester is
#                                  not asked, needs64 YES
#   build/test-bios-missing.iso    isolinux, a 64-bit kernel and an entry
#                                  whose kernel is not on the medium:
#                                  needs64 UNKNOWN
#   build/test-bios-hdd.iso        a hard-disk emulation BIOS image only, no
#                                  configuration: boots UNKNOWN
#   build/test-bios-bsd64.iso      FreeBSD-shaped, /boot/kernel/kernel an
#                                  x86-64 ELF: needs64 YES
#   build/test-bios-bsd32.iso      the same with an i386 ELF: needs64 NO
#   build/test-bios-win-efi.iso    Windows-shaped with a UEFI El Torito image
#                                  only: boots NO
#
# Usage: falcon-core/tools/mk-test-biosboot.sh
# Needs xorriso, mkfs.fat, mtools and sfdisk. No sudo.

set -euo pipefail

cd "$(dirname "$0")/../.."

for t in xorriso mkfs.fat mmd mcopy sfdisk; do
    command -v "$t" >/dev/null || { echo "ERROR: $t not found" >&2; exit 1; }
done

mkdir -p build
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

put() {         # put FILE OFFSET HEX...: bytes written in place
    local f=$1 off=$2
    shift 2
    printf "$(printf '\\x%s' "$@")" |
        dd of="$f" bs=1 seek=$((off)) conv=notrunc status=none
}

# A bzImage head: the boot sector signature, the setup header magic, the
# protocol version (low, high) and xloadflags' low byte.
bzimage() {     # bzimage FILE VER_LO VER_HI XLF
    head -c 8192 /dev/zero > "$1"
    put "$1" 0x1fe 55 aa
    put "$1" 0x202 48 64 72 53          # "HdrS"
    put "$1" 0x206 "$2" "$3"
    put "$1" 0x236 "$4" 00
}

# An ELF header: identification, then e_machine at 18.
elf() {         # elf FILE CLASS MACHINE_LO
    head -c 4096 /dev/zero > "$1"
    put "$1" 0 7f 45 4c 46 "$2" 01 01
    put "$1" 18 "$3" 00
}

efi_image() {   # efi_image IMG [NAME]: a FAT boot image holding BOOTX64.EFI
    rm -f "$1"
    mkfs.fat -C "$1" 1440 >/dev/null
    mmd -i "$1" ::/EFI ::/EFI/BOOT
    printf 'not a loader\n' > "$W/loader"
    mcopy -i "$1" "$W/loader" "::/EFI/BOOT/${2:-BOOTX64.EFI}"
}

# isolinux with one LABEL per kernel given, each at /boot/<name>.
linux_tree() {  # linux_tree DIR NAME:FILE...
    local d=$1 k name
    shift
    mkdir -p "$d/isolinux" "$d/boot"
    head -c 2048 /dev/zero > "$d/isolinux/isolinux.bin"
    printf 'DEFAULT %s\n' "${1%%:*}" > "$d/isolinux/isolinux.cfg"
    for k in "$@"; do
        name=${k%%:*}
        printf 'LABEL %s\n  KERNEL /boot/%s\n  APPEND boot=live\n' \
            "$name" "$name" >> "$d/isolinux/isolinux.cfg"
        if [[ -n "${k#*:}" ]]; then
            cp "${k#*:}" "$d/boot/$name"
        fi
    done
}

XO=(xorriso -as mkisofs -quiet -r -J)
BIOS_LX=(-b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot
         -boot-load-size 4)

bzimage "$W/k64" 0f 02 01
bzimage "$W/k32" 0f 02 00
bzimage "$W/kold" 02 02 00
head -c 8192 /dev/urandom > "$W/memtest"

echo "== build/test-bios-efionly.iso"
mkdir -p "$W/t1/efi"
efi_image "$W/t1/efi/efiboot.img"
"${XO[@]}" -V BIOS_EFIONLY -e efi/efiboot.img -no-emul-boot \
    -o build/test-bios-efionly.iso "$W/t1"

echo "== build/test-bios-arm64.iso"
mkdir -p "$W/t13/efi" "$W/t13/EFI/BOOT"
efi_image "$W/t13/efi/efiboot.img" BOOTAA64.EFI
printf 'not a loader\n' > "$W/t13/EFI/BOOT/BOOTAA64.EFI"
"${XO[@]}" -V BIOS_ARM64 -e efi/efiboot.img -no-emul-boot \
    -o build/test-bios-arm64.iso "$W/t13"

echo "== build/test-bios-data.iso"
mkdir -p "$W/t0"
printf 'just files\n' > "$W/t0/README.TXT"
"${XO[@]}" -V BIOS_DATA -o build/test-bios-data.iso "$W/t0"

echo "== build/test-bios-efikernel.iso"
mkdir -p "$W/t2/efi" "$W/t2/boot/grub"
efi_image "$W/t2/efi/efiboot.img"
cp "$W/k64" "$W/t2/boot/vmlinuz"
cat > "$W/t2/boot/grub/grub.cfg" <<'CFG'
menuentry "live" {
    linux /boot/vmlinuz boot=live
}
CFG
"${XO[@]}" -V BIOS_EFIKERNEL -e efi/efiboot.img -no-emul-boot \
    -o build/test-bios-efikernel.iso "$W/t2"

echo "== build/test-bios-64.iso"
linux_tree "$W/t3" "vmlinuz:$W/k64" "vmlinuz-rescue:$W/k64"
"${XO[@]}" -V BIOS_64 "${BIOS_LX[@]}" -o build/test-bios-64.iso "$W/t3"

echo "== build/test-bios-multi.iso"
linux_tree "$W/t4" "vmlinuz-amd64:$W/k64" "vmlinuz-i386:$W/k32"
"${XO[@]}" -V BIOS_MULTI "${BIOS_LX[@]}" -o build/test-bios-multi.iso "$W/t4"

echo "== build/test-bios-32.iso"
linux_tree "$W/t5" "vmlinuz:$W/k32"
"${XO[@]}" -V BIOS_32 "${BIOS_LX[@]}" -o build/test-bios-32.iso "$W/t5"

echo "== build/test-bios-old.iso"
linux_tree "$W/t6" "vmlinuz:$W/k64" "vmlinuz-old:$W/kold"
"${XO[@]}" -V BIOS_OLD "${BIOS_LX[@]}" -o build/test-bios-old.iso "$W/t6"

echo "== build/test-bios-memtest.iso"
linux_tree "$W/t7" "vmlinuz:$W/k64" "memtest:$W/memtest"
"${XO[@]}" -V BIOS_MEMTEST "${BIOS_LX[@]}" \
    -o build/test-bios-memtest.iso "$W/t7"

echo "== build/test-bios-missing.iso"
linux_tree "$W/t8" "vmlinuz:$W/k64" "vmlinuz-gone:"
"${XO[@]}" -V BIOS_MISSING "${BIOS_LX[@]}" \
    -o build/test-bios-missing.iso "$W/t8"

echo "== build/test-bios-hdd.iso"
mkdir -p "$W/t9/boot"
truncate -s 2M "$W/t9/boot/hd.img"
printf 'start=2048, type=0c, bootable\n' |
    sfdisk --quiet --label dos "$W/t9/boot/hd.img" >/dev/null
put "$W/t9/boot/hd.img" 0x1fe 55 aa
"${XO[@]}" -V BIOS_HDD -b boot/hd.img -c boot/boot.cat -hard-disk-boot \
    -o build/test-bios-hdd.iso "$W/t9"

bsd_tree() {    # bsd_tree DIR KERNEL
    mkdir -p "$1/boot/kernel" "$1/boot/defaults"
    head -c 2048 /dev/zero > "$1/boot/cdboot"
    cp "$2" "$1/boot/kernel/kernel"
    head -c 1024 /dev/zero | tr '\0' '#' > "$1/boot/defaults/loader.conf"
}

elf "$W/elf64" 02 3e
elf "$W/elf32" 01 03

echo "== build/test-bios-bsd64.iso"
bsd_tree "$W/t10" "$W/elf64"
"${XO[@]}" -V BIOS_BSD64 -b boot/cdboot -c boot/boot.cat -no-emul-boot \
    -boot-load-size 4 -o build/test-bios-bsd64.iso "$W/t10"

echo "== build/test-bios-bsd32.iso"
bsd_tree "$W/t11" "$W/elf32"
"${XO[@]}" -V BIOS_BSD32 -b boot/cdboot -c boot/boot.cat -no-emul-boot \
    -boot-load-size 4 -o build/test-bios-bsd32.iso "$W/t11"

echo "== build/test-bios-win-efi.iso"
mkdir -p "$W/t12/boot" "$W/t12/sources" "$W/t12/efi/microsoft/boot"
printf 'bootmgr\n' > "$W/t12/bootmgr"
printf 'wim\n' > "$W/t12/sources/install.wim"
efi_image "$W/t12/efi/microsoft/boot/efisys.bin"
"${XO[@]}" -V BIOS_WIN_EFI -e efi/microsoft/boot/efisys.bin -no-emul-boot \
    -o build/test-bios-win-efi.iso "$W/t12"
