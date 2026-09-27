#!/usr/bin/env bash
# falcon-cli: `mkstick station install|status|remove` end to end, on image
# files (docs/DESIGN.md Section 13.9). No root: the installer formats with
# mkfs.fat --offset, writes files with mtools and stamps a shelf with
# debugfs or cryptsetup, all of which take an image file as it is.
#
# Three machines:
#
#   mbr    the A21m's shape: one NTFS partition (an NTLDR boot sector) from
#          LBA 63, shrunk to leave 80 MiB free at the end, disk signature 0;
#          a second disk with a Windows 98 FAT32 partition and its own MBR
#          code, signature 0 too. BIOS install, status, remove, and remove
#          --full back to the original bytes; a LUKS2 shelf image stamped
#          with the station's install ID.
#   gpt    an ESP with Windows Boot Manager on it, a Windows partition (a
#          BOOTMGR boot sector), free space at the end, and a plaintext
#          shelf image. BIOS and UEFI together.
#   the refusals: a disk with no free space, one with four primaries, a
#          size under the floor, a second install.
#
# Usage: falcon-cli/tools/test-station-install.sh   (after make, make bios,
# make station; the UEFI half needs build/shim/bins and build/mok-test)

set -euo pipefail

cd "$(dirname "$0")/../.."
MK=build/bin/falcon-boot-mkstick
W=build/station-install
MOK=build/mok-test
export MTOOLS_SKIP_CHECK=1

for f in "$MK" build/bin/falcon-bios-stage1.bin build/bin/falcon-bios-stage2.bin \
         build/bin/falcon-bios-station-stage3.bin build/bin/falcon-boot-station.efi; do
    [ -f "$f" ] || { echo "missing $f: make, make bios, make station" >&2; exit 2; }
done

rm -rf "$W"
mkdir -p "$W"

pass=0
fail=0
ok()  { echo "PASS  $*"; pass=$((pass + 1)); }
bad() { echo "FAIL  $*"; fail=$((fail + 1)); }
check() { local d=$1; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi; }
# expect_fail <what> <pattern> <command...>: fails, and says the pattern.
expect_fail() {
    local d=$1 pat=$2 out
    shift 2
    if out=$("$@" 2>&1); then
        bad "$d (it succeeded)"
    elif grep -q -- "$pat" <<<"$out"; then
        ok "$d"
    else
        bad "$d: $out"
    fi
}

# A partition boot sector naming a loader: jump, OEM name, the loader's
# string where boot code keeps its messages, 0x55AA.
bootsec() {
    local img=$1 lba=$2 oem=$3 text=$4
    printf '\xeb\x52\x90%s' "$oem" |
        dd of="$img" bs=1 seek=$((lba * 512)) conv=notrunc status=none
    printf '%s' "$text" |
        dd of="$img" bs=1 seek=$((lba * 512 + 0x180)) conv=notrunc status=none
    printf '\x55\xaa' |
        dd of="$img" bs=1 seek=$((lba * 512 + 510)) conv=notrunc status=none
}
mbrcode() { printf '\xfa\x33\xc0\x8e\xd0\xbc\x00\x7c' | dd of="$1" bs=1 conv=notrunc status=none; }
sig() { od -An -tx4 -j440 -N4 "$1" | tr -d ' '; }
same_range() { # img orig first-byte count
    cmp -s <(dd if="$1" bs=1M iflag=skip_bytes,count_bytes skip="$3" count="$4" status=none) \
           <(dd if="$2" bs=1M iflag=skip_bytes,count_bytes skip="$3" count="$4" status=none)
}

# ---- the MBR machine ----
M=$W/mbr.img
D2=$W/disk2.img
LS=$W/luks-shelf.img
truncate -s 1G "$M"
total=$((1024 * 2048))
printf 'label: dos\nlabel-id: 0x0\nunit: sectors\nstart=63, size=%d, type=7, bootable\n' \
    $((total - 80 * 2048 - 63)) | sfdisk -q "$M"
mbrcode "$M"
bootsec "$M" 63 "NTFS    " "A disk read error occurred.NTLDR is missing"
truncate -s 64M "$D2"
printf 'label: dos\nlabel-id: 0x0\nunit: sectors\nstart=2048, type=c, bootable\n' |
    sfdisk -q "$D2"
mbrcode "$D2"
bootsec "$D2" 2048 "MSWIN4.1" "IO      SYSMSDOS   SYSWINBOOT SYS"
truncate -s 32M "$LS"
printf pw | cryptsetup luksFormat --type luks2 --pbkdf pbkdf2 \
    --pbkdf-force-iterations 1000 --batch-mode "$LS" - >/dev/null
printf '{"type":"falcon","keyslots":[],"version":1,"role":"shelf","install_id":"00112233445566778899aabbccddeeff"}' |
    cryptsetup token import "$LS"
printf 'memtest86+ stand-in\n' >"$W/memtest.bin"
cp --sparse=always "$M" "$W/mbr.orig"
cp --sparse=always "$D2" "$W/disk2.orig"

"$MK" station install "$M" --bios --also "$D2" --dry-run >"$W/dry.log" 2>&1
check "mbr: --dry-run writes nothing" cmp -s "$M" "$W/mbr.orig"
check "mbr: --dry-run names the NTLDR partition" grep -q "Windows (NTLDR)" "$W/dry.log"

"$MK" station install "$M" --bios --also "$D2" --shelf "$LS" \
    --memtest "$W/memtest.bin" --backup "$W/mbr.bak" >"$W/install.log" 2>&1 ||
    { cat "$W/install.log"; bad "mbr: install"; }
ok "mbr: install"
p2=$(sfdisk -d "$M" | grep 'img2 :' || true)
check "mbr: a type 0x1c primary in the free space" grep -q "type=1c" <<<"$p2"
start=$(sed -n 's/.*start= *\([0-9]*\).*/\1/p' <<<"$p2")
check "mbr: its start is on a MiB boundary" test $((start % 2048)) -eq 0
check "mbr: it ends inside the disk" test $((start + 131072)) -le "$total"
check "mbr: the NTFS partition is untouched" \
    same_range "$M" "$W/mbr.orig" $((63 * 512)) 512
check "mbr: the target has a signature now" test "$(sig "$M")" != 00000000
check "mbr: so does the second disk" test "$(sig "$D2")" != 00000000
id=$("$MK" ident "$M" | awk '$2 == "boot" { print $3 }')
check "mbr: partition 2 carries a boot record" test -n "$id"
check "mbr: the LUKS2 shelf carries the station's install ID" \
    grep -q "\"install_id\":\"$id\"" <(cryptsetup token export --token-id 0 "$LS" 2>/dev/null ||
                                         cryptsetup token export --token-id 1 "$LS")
check "mbr: the shelf has one falcon token" \
    test "$(for t in 0 1 2 3; do cryptsetup token export --token-id $t "$LS" 2>/dev/null; echo; done | grep -c falcon)" -eq 1
off=$((start * 512))
check "mbr: STATION.INI in the station partition" mtype -i "$M@@$off" ::/STATION.INI
check "mbr: MEMTEST.BIN in the station partition" mtype -i "$M@@$off" ::/MEMTEST.BIN
mtype -i "$M@@$off" ::/STATION.INI >"$W/station.ini"
check "mbr: the INI's default is the NTLDR Windows" grep -q "^default = windows$" "$W/station.ini"
check "mbr: the INI names the second disk's Windows 98" grep -q "^\[windows-2\]" "$W/station.ini"
check "mbr: the INI names the second disk's MBR" grep -q "^\[disk\]" "$W/station.ini"
check "mbr: the INI names the disk by its new signature" \
    grep -qi "disk = mbr:$(sig "$M")" "$W/station.ini"
"$MK" station status "$M" --also "$D2" >"$W/status.log" 2>&1 && ok "mbr: status passes" ||
    { cat "$W/status.log"; bad "mbr: status"; }
check "mbr: status shows the default row" grep -q "\[default\]" "$W/status.log"
check "mbr: status finds stage 2" grep -q "OK   stage 2 is at LBA 1" "$W/status.log"
check "mbr: status finds MEMTEST.BIN" grep -q "MEMTEST.BIN present" "$W/status.log"
expect_fail "mbr: a second install is refused" "already carries falcon install" \
    "$MK" station install "$M" --bios --dry-run

"$MK" station remove --backup "$W/mbr.bak" >"$W/remove.log" 2>&1 && ok "mbr: remove" ||
    { cat "$W/remove.log"; bad "mbr: remove"; }
check "mbr: remove puts back LBA 0's boot code" same_range "$M" "$W/mbr.orig" 0 440
check "mbr: remove puts back the boot area" same_range "$M" "$W/mbr.orig" 512 $((62 * 512))
check "mbr: remove leaves the station partition" grep -q 'img2 :' <(sfdisk -d "$M")
"$MK" station remove --backup "$W/mbr.bak" --full >"$W/remove-full.log" 2>&1 &&
    ok "mbr: remove --full" || { cat "$W/remove-full.log"; bad "mbr: remove --full"; }
check "mbr: remove --full is the original disk, byte for byte" cmp -s "$M" "$W/mbr.orig"
check "mbr: and the second disk's" cmp -s "$D2" "$W/disk2.orig"

# ---- station-legacy: jump's drivers and /uki in the station partition ----
mkdir -p "$W/mods"
for m in ATA EHCI UHCI OHCI PCC; do
    printf '%s driver\n' "$m" | gzip >"$W/mods/$m.MOD.gz"
done
printf 'DevLink recovery stand-in\n' >"$W/devlink.efi"
cp --sparse=always "$W/mbr.orig" "$M"
"$MK" station install "$M" --bios --legacy \
    --stage3 build/bin/falcon-bios-station-stage3.bin --mods "$W/mods" \
    --uki "$W/devlink.efi" --backup "$W/legacy.bak" >"$W/legacy.log" 2>&1 &&
    ok "legacy: install" || { cat "$W/legacy.log"; bad "legacy: install"; }
start=$(sfdisk -d "$M" | sed -n 's/.*img2 : start= *\([0-9]*\).*/\1/p')
for m in ATA EHCI UHCI OHCI PCC; do
    check "legacy: $m.MOD, as the floppy carries it" \
        cmp -s <(mtype -i "$M@@$((start * 512))" "::/$m.MOD") "$W/mods/$m.MOD.gz"
done
check "legacy: /uki/devlink.efi" mtype -i "$M@@$((start * 512))" ::/uki/devlink.efi
"$MK" station remove --backup "$W/legacy.bak" --full >/dev/null 2>&1
check "legacy: remove --full is the original disk" cmp -s "$M" "$W/mbr.orig"

# ---- the refusals ----
F=$W/full.img
truncate -s 256M "$F"
printf 'label: dos\nunit: sectors\nstart=63, type=7, bootable\n' | sfdisk -q "$F"
expect_fail "refused: no free space, and it says how much it needs" "it needs 64.0 MiB" \
    "$MK" station install "$F" --bios --dry-run
S=$W/slots.img
truncate -s 1G "$S"
printf 'label: dos\nunit: sectors\nstart=2048, size=100000, type=7\nsize=100000, type=83\nsize=100000, type=83\nsize=100000, type=82\n' |
    sfdisk -q "$S"
expect_fail "refused: four primaries taken" "four primary slots" \
    "$MK" station install "$S" --bios --dry-run
expect_fail "refused: --size under the floor" "at least 40 MiB" \
    "$MK" station install "$M" --bios --size 30 --dry-run
expect_fail "refused: --uki without --legacy" "station-legacy" \
    "$MK" station install "$M" --bios --uki "$W/memtest.bin" --dry-run
truncate -s 1M "$W/blank.img"
expect_fail "refused: a disk with no table" "no partition table" \
    "$MK" station install "$W/blank.img" --dry-run

# ---- the GPT machine ----
if [ ! -d build/shim/bins ] || [ ! -f "$MOK/falcon-test-mok.key" ]; then
    echo "SKIP  gpt: no build/shim/bins or $MOK (copy them from a built tree)"
else
    G=$W/gpt.img
    truncate -s 1G "$G"
    sgdisk -o -n 1:2048:+100M -t 1:EF00 -n 2:0:+700M -t 2:0700 \
        -n 3:0:+64M -t 3:9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF "$G" >/dev/null
    mkfs.fat -F 32 -s 1 --offset 2048 --mbr=n "$G" 102400 >/dev/null 2>&1
    head -c 8192 /dev/urandom >"$W/bootmgfw.efi"
    mmd -i "$G@@1048576" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot
    mcopy -i "$G@@1048576" "$W/bootmgfw.efi" ::/EFI/Microsoft/Boot/bootmgfw.efi
    first() { sgdisk -i "$1" "$G" | sed -n 's/^First sector: \([0-9]*\).*/\1/p'; }
    p2s=$(first 2)
    p3s=$(first 3)
    bootsec "$G" "$p2s" "NTFS    " "A disk read error occurred.BOOTMGR is missing"
    mkdir -p "$W/shelf-root/uki"
    cp build/bin/falcon-boot-station.efi "$W/shelf-root/uki/arch.efi"
    mkfs.ext4 -q -F -L FALCON-UKI -E offset=$((p3s * 512)) -d "$W/shelf-root" "$G" 64M
    cp --sparse=always "$G" "$W/gpt.orig"

    "$MK" station install "$G" --shelf 3 --key "$MOK/falcon-test-mok.key" \
        --cert "$MOK/falcon-test-mok.crt" --memtest "$W/memtest.bin" \
        --memtest-efi build/bin/falcon-boot-station.efi --backup "$W/gpt.bak" \
        >"$W/gpt-install.log" 2>&1 && ok "gpt: install (BIOS and UEFI)" ||
        { cat "$W/gpt-install.log"; bad "gpt: install"; }
    sgdisk -p "$G" >"$W/gpt-table.txt"
    check "gpt: the FALCON-BIOS partition at LBA 34" grep -qE "^ +[0-9]+ +34 .*EF02.*FALCON-BIOS" "$W/gpt-table.txt"
    check "gpt: the station partition" grep -q "FALCON-STATION" "$W/gpt-table.txt"
    gid=$("$MK" ident "$G" | awk '$2 == "boot" { print $3 }' | sort -u)
    check "gpt: the ESP and the station partition carry one install ID" \
        test "$(wc -l <<<"$gid")" -eq 1 -a "$("$MK" ident "$G" | awk '$2 == "boot"' | wc -l)" -eq 2
    check "gpt: the plaintext shelf (partition 3) carries it" \
        grep -q "falcon 1 shelf $gid" <(debugfs -R "cat /.falcon" "$G?offset=$((p3s * 512))" 2>/dev/null)
    for f in shimx64.efi mmx64.efi grubx64.efi falcon-boot.cer STATION.INI memtest.efi; do
        check "gpt: \\EFI\\falcon\\$f" mtype -i "$G@@1048576" "::/EFI/falcon/$f"
    done
    mcopy -o -i "$G@@1048576" ::/EFI/falcon/grubx64.efi "$W/grubx64.efi"
    check "gpt: grubx64.efi is signed with the MOK" \
        sbverify --cert "$MOK/falcon-test-mok.crt" "$W/grubx64.efi"
    mtype -i "$G@@1048576" ::/EFI/falcon/STATION.INI >"$W/uefi.ini"
    check "gpt: the UEFI INI's default is Windows on the ESP" grep -q "^default = windows$" "$W/uefi.ini"
    check "gpt: through the firmware's entry" grep -q "^method = firmware$" "$W/uefi.ini"
    "$MK" station status "$G" >"$W/gpt-status.log" 2>&1 && ok "gpt: status passes" ||
        { cat "$W/gpt-status.log"; bad "gpt: status"; }
    check "gpt: status lists the shelf's UKI" grep -q "uki-arch" "$W/gpt-status.log"
    check "gpt: status reads both INIs" test "$(grep -c "parses" "$W/gpt-status.log")" -eq 2

    "$MK" station remove --backup "$W/gpt.bak" >"$W/gpt-remove.log" 2>&1 && ok "gpt: remove" ||
        { cat "$W/gpt-remove.log"; bad "gpt: remove"; }
    check "gpt: remove deletes \\EFI\\falcon" bash -c "! mdir -i '$G@@1048576' ::/EFI/falcon"
    check "gpt: remove keeps Windows Boot Manager" mtype -i "$G@@1048576" ::/EFI/Microsoft/Boot/bootmgfw.efi
    check "gpt: remove drops the FALCON-BIOS partition" bash -c "! sgdisk -p '$G' | grep -q FALCON-BIOS"
    check "gpt: remove leaves the station partition" grep -q FALCON-STATION <(sgdisk -p "$G")
    "$MK" station remove --backup "$W/gpt.bak" --full --force >"$W/gpt-remove-full.log" 2>&1 &&
        ok "gpt: remove --full" || { cat "$W/gpt-remove-full.log"; bad "gpt: remove --full"; }
    # The ESP keeps the deleted directory entries mdeltree leaves, and the
    # shelf its stamp; every other byte is the original.
    same_disk() {
        cmp -s <(head -c 1048576 "$G") <(head -c 1048576 "$W/gpt.orig") &&
            same_range "$G" "$W/gpt.orig" $((p2s * 512)) $(((p3s - p2s) * 512)) &&
            cmp -s <(tail -c +$(((p3s + 131072) * 512 + 1)) "$G") \
                   <(tail -c +$(((p3s + 131072) * 512 + 1)) "$W/gpt.orig")
    }
    check "gpt: remove --full is the original disk outside the ESP and the shelf" same_disk
fi

echo
echo "station install: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
