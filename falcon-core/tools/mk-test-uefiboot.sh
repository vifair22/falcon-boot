#!/usr/bin/env bash
# falcon-boot: build the ISOs falcon-core/tests/uefiboot_test.c reads
# (falcon-core/src/media/uefiboot.h: can a UEFI machine of this
# architecture boot this).
#
# Seven media, each the smallest one that has the shape in question. The
# kernels are not kernels: the rule reads a kernel's head only, so a head is
# what each one is (a setup header's "HdrS" at 0x202, and for the stub kernels
# a PE header at 0x400 naming its processor).
#
#   build/test-uefi-biosonly.iso  isolinux, BIOS El Torito only, a bzImage
#                                 with no PE header: NO
#   build/test-uefi-stub.iso      the same, the kernel an x86-64 EFI stub:
#                                 YES (the direct route boots it)
#   build/test-uefi-efi.iso       the first medium plus a UEFI El Torito
#                                 image (FAT, \EFI\BOOT\BOOTX64.EFI): YES
#   build/test-uefi-win-bios.iso  Windows-shaped (/sources/install.wim,
#                                 /bootmgr), BIOS El Torito only: NO
#   build/test-uefi-win-efi.iso   the same plus a 0xEF entry whose image
#                                 holds only BOOTIA32.EFI, 8.1 and 10 x86's
#                                 shape: YES, it is a 32-bit UEFI medium
#   build/test-uefi-aa64.iso      the first medium plus a UEFI El Torito
#                                 image holding BOOTAA64.EFI: YES on
#                                 AArch64, OTHER on x86-64
#   build/test-uefi-aa64-stub.iso isolinux-shaped, the kernel an ARM64 EFI
#                                 stub: YES on AArch64, OTHER on x86-64
#
# Usage: falcon-core/tools/mk-test-uefiboot.sh
# Needs xorriso, mkfs.fat and mtools. No sudo.

set -euo pipefail

cd "$(dirname "$0")/../.."

for t in xorriso mkfs.fat mmd mcopy; do
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

# A bzImage head: the boot sector signature and the setup header magic.
bzimage() {     # bzimage FILE
    head -c 8192 /dev/zero > "$1"
    put "$1" 0x1fe 55 aa
    put "$1" 0x202 48 64 72 53          # "HdrS"
}

# The same with an EFI stub: "MZ", e_lfanew at 0x3c, and a PE signature and
# COFF header at 0x400 with an optional header of the given magic.
stub() {        # stub FILE MACHINE_LO MACHINE_HI MAGIC_LO MAGIC_HI
    bzimage "$1"
    put "$1" 0 4d 5a
    put "$1" 0x3c 00 04 00 00
    put "$1" 0x400 50 45 00 00 "$2" "$3" 00 00
    put "$1" 0x414 f0 00                # SizeOfOptionalHeader
    put "$1" 0x418 "$4" "$5"
}

efi_image() {   # efi_image IMG LOADER: a FAT boot image holding one loader
    rm -f "$1"
    mkfs.fat -C "$1" 1440 >/dev/null
    mmd -i "$1" ::/EFI ::/EFI/BOOT
    printf 'not a loader\n' > "$W/loader"
    mcopy -i "$1" "$W/loader" "::/EFI/BOOT/$2"
}

linux_tree() {  # linux_tree DIR KERNEL
    mkdir -p "$1/isolinux" "$1/boot"
    head -c 2048 /dev/zero > "$1/isolinux/isolinux.bin"
    cat > "$1/isolinux/isolinux.cfg" <<'CFG'
DEFAULT live
LABEL live
  KERNEL /boot/vmlinuz
  APPEND initrd=/boot/initrd.img boot=live
CFG
    cp "$2" "$1/boot/vmlinuz"
    head -c 3001 /dev/urandom > "$1/boot/initrd.img"
}

win_tree() {    # win_tree DIR
    mkdir -p "$1/boot" "$1/sources"
    head -c 2048 /dev/zero > "$1/boot/etfsboot.com"
    printf 'bootmgr\n' > "$1/bootmgr"
    printf 'wim\n' > "$1/sources/install.wim"
}

XO=(xorriso -as mkisofs -quiet -r -J)
BIOS_LX=(-b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot
         -boot-load-size 4)
BIOS_WIN=(-b boot/etfsboot.com -c boot/boot.cat -no-emul-boot
          -boot-load-size 8)

bzimage "$W/k-plain"
stub "$W/k-stub" 64 86 0b 02
stub "$W/k-stub-aa64" 64 aa 0b 02

echo "== build/test-uefi-biosonly.iso"
linux_tree "$W/t1" "$W/k-plain"
"${XO[@]}" -V UEFI_BIOSONLY "${BIOS_LX[@]}" \
    -o build/test-uefi-biosonly.iso "$W/t1"

echo "== build/test-uefi-stub.iso"
linux_tree "$W/t2" "$W/k-stub"
"${XO[@]}" -V UEFI_STUB "${BIOS_LX[@]}" -o build/test-uefi-stub.iso "$W/t2"

echo "== build/test-uefi-efi.iso"
linux_tree "$W/t3" "$W/k-plain"
efi_image "$W/t3/isolinux/efiboot.img" BOOTX64.EFI
"${XO[@]}" -V UEFI_EFI "${BIOS_LX[@]}" \
    -eltorito-alt-boot -e isolinux/efiboot.img -no-emul-boot \
    -o build/test-uefi-efi.iso "$W/t3"

echo "== build/test-uefi-win-bios.iso"
win_tree "$W/t4"
"${XO[@]}" -V UEFI_WIN_BIOS "${BIOS_WIN[@]}" \
    -o build/test-uefi-win-bios.iso "$W/t4"

echo "== build/test-uefi-win-efi.iso"
win_tree "$W/t5"
mkdir -p "$W/t5/efi/microsoft/boot"
efi_image "$W/t5/efi/microsoft/boot/efisys.bin" BOOTIA32.EFI
"${XO[@]}" -V UEFI_WIN_EFI "${BIOS_WIN[@]}" \
    -eltorito-alt-boot -e efi/microsoft/boot/efisys.bin -no-emul-boot \
    -o build/test-uefi-win-efi.iso "$W/t5"

echo "== build/test-uefi-aa64.iso"
linux_tree "$W/t6" "$W/k-plain"
efi_image "$W/t6/isolinux/efiboot.img" BOOTAA64.EFI
"${XO[@]}" -V UEFI_AA64 "${BIOS_LX[@]}" \
    -eltorito-alt-boot -e isolinux/efiboot.img -no-emul-boot \
    -o build/test-uefi-aa64.iso "$W/t6"

echo "== build/test-uefi-aa64-stub.iso"
linux_tree "$W/t7" "$W/k-stub-aa64"
"${XO[@]}" -V UEFI_AA64_STUB "${BIOS_LX[@]}" \
    -o build/test-uefi-aa64-stub.iso "$W/t7"
