#!/usr/bin/env bash
# falcon-boot: stations made by the installer, booted (docs/DESIGN.md
# Sections 12.9, 12.11). Every disk
# here is a machine's disk before falcon: partitions of its own and free
# space, and `mkstick station install` puts the station on it as a user
# does. Nothing is copied onto a disk by hand afterwards: the firmware
# boots the station through a "falcon" Boot0001 naming
# \EFI\falcon\shimx64.efi, the entry `efibootmgr --create` would make on
# the machine (the installer makes none for an image, and says so).
#
# The machines:
#
#   gpt   GPT, 512-byte sectors: an ESP carrying Windows Boot Manager (the
#         rig's stand-in, falcon-efi/tools/payload/standin.c, signed with the
#         test MOK), a Windows partition, a LUKS2 shelf with the fixture UKI
#         and the MOK pair, and free space. `station install --uefi
#         --shim`; the varstore has falcon's entry and Windows's, as the
#         machine would.
#   4kn   GPT on a 4096-byte-sector disk (a loop device with 4096-byte
#         sectors, booted as NVMe with 4096-byte logical blocks, the one QEMU
#         disk SeaBIOS offers at that size; DESIGN Section 3.4): an ESP of
#         300 MiB, a LUKS2 shelf, free space. `station install --shim`, through a
#         link named like a by-id one, both stations: the station partition
#         (288 MiB, FAT32 of 4096-byte sectors), the FALCON-BIOS partition at
#         LBA 34, everything counted in 4096-byte sectors.
#   mbr4k MBR on a 4096-byte-sector disk: a Windows partition, a LUKS2
#         shelf, free space. `station install --bios`.
#   gpt2  two GPT disks: the station's (an ESP with no Windows, a LUKS2
#         shelf, free space) and a Windows disk behind it (its own ESP
#         carrying the stand-in Windows Boot Manager, a Windows partition).
#         `station install --uefi --shim --also` the Windows disk; the varstore
#         has falcon's entry, and in the -nxt cases a "Windows Boot
#         Manager" entry naming the second disk's ESP.
#   mbr   MBR, 512-byte sectors: an ESP of type 0xEF, a LUKS2 shelf, free
#         space. `station install --uefi --shim`; falcon's entry names the ESP by
#         the disk's signature, as a firmware does on an MBR disk.
#   noshim GPT, 512-byte sectors: an ESP, a LUKS2 shelf, free space.
#         `station install --uefi`, without --shim: the loader as
#         \EFI\falcon\falconx64.efi, the jumper as \EFI\BOOT\BOOTX64.EFI,
#         and falcon's entry naming the jumper; with Secure Boot on the
#         test MOK in db is what verifies both, as the machine's own key
#         does on the T430.
#   foreign noshim with another system's \EFI\BOOT\BOOTX64.EFI (the
#         stand-in) on the ESP before the install.
#
# The cases:
#
#   uefi-default, uefi-sb-default
#                 gpt under OVMF, Secure Boot off and on: the list has the
#                 Personal OS, Windows (its firmware entry, which the
#                 installer's [windows] section matches by disk and
#                 labels) and memtest; the INI counts down to Windows, which
#                 boots through BootNext and a reset
#   uefi-pos, uefi-sb-pos
#                 the same, a key stopping the count: the Personal OS row,
#                 the passphrase, and the fixture UKI reporting both
#                 credentials, falcon.ident.cred naming the install
#   4kn-uefi, 4kn-uefi-sb
#                 4kn under OVMF: the installer's default (the shelf's UKI,
#                 a [linux] section, since no row names it before the
#                 prompt) counts down to the prompt, and the UKI reports
#   4kn-bios      4kn under SeaBIOS: stage 2 takes the station partition
#                 (not the ESP, whose record names no stage 3), and the
#                 shelf unlocks to the UKI
#   4kn-remove    4kn: `station status` passes, and `station remove --full`
#                 puts the disk back to the bytes it had before the install,
#                 outside the ESP (mdeltree's deleted entries) and the
#                 shelf (its stamp)
#   mbr4k-bios    mbr4k under SeaBIOS, to the UKI
#   mbr4k-bios-bad  the same with five wrong passphrases: falcon-bios's
#                 waits of 0, 4, 8 and 16 s, then its power-off, QEMU
#                 exiting (DESIGN Section 5.2)
#   second-nxt, second-sb-nxt
#                 gpt2 with Windows's firmware entry: the INI's [windows]
#                 (the second disk) matches the entry, counts down, and
#                 boots it through BootNext and a reset
#   second-esp, second-sb-esp
#                 gpt2 with no entry for Windows: the same [windows] is the
#                 second disk's ESP row, which counts down and loads
#                 bootmgfw.efi from that disk by device path, no reset
#   mbr-uefi, mbr-uefi-sb
#                 mbr under OVMF: the list has the shelf's UKI and memtest,
#                 the installer's default (the UKI, a [linux] section)
#                 counts down to the prompt, and the UKI reports
#   noshim-uefi, noshim-uefi-sb
#                 noshim under OVMF, falcon's entry starting the jumper and
#                 the jumper the loader: the default counts down to the
#                 prompt, and the UKI reports
#   noshim-jump-uefi, noshim-jump-uefi-sb
#                 noshim with no firmware entry for the station: the
#                 firmware boots the disk's \EFI\BOOT\BOOTX64.EFI, the
#                 jumper, which starts falconx64.efi, and the UKI reports
#   noshim-status `station status` on noshim passes: no shim, the loader
#                 and the jumper signed by the cert beside them, and the
#                 ESP holds no shim or MokManager
#   noshim-update `mkstick update` on a copy of noshim signs the loader and
#                 the jumper again, saving the old jumper first
#   noshim-migrate
#                 a copy of noshim made into an install from before the
#                 jumper (the loader as grubx64.efi, the stand-in as
#                 BOOTX64.EFI): `mkstick update` renames the loader, saves
#                 the stand-in and writes the jumper, and with Secure Boot
#                 on the entry naming the jumper boots it to the UKI
#   foreign-jump  foreign: the installer saves the other system's
#                 BOOTX64.EFI beside its backup and puts the jumper there;
#                 `station remove` puts it back byte for byte
#
# The disks are image files; the 4096-byte-sector ones are written through
# loop devices, which needs sudo, as do cryptsetup and the installer's
# LUKS2 token. Nothing touches a real disk or this machine's variables.
#
# Usage: falcon-efi/tools/run-installed-rig.sh [case...]   (default: all)
# Env:   FALCON_TEST_PASS  the shelves' passphrase (default falcon-test)
#        KEEP=1            reuse the installed disks from an earlier run
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. falcon-efi/tools/ovmf.sh

R=build/installed-rig
PASS="${FALCON_TEST_PASS:-falcon-test}"
MKSTICK=build/bin/falcon-boot-mkstick
STANDIN=build/test/standin.efi
STANDIN_SIGNED=build/test/standin-signed.efi
FIXTURE_UKI_SIGNED=build/test/fixture-uki-signed.efi
MOK_KEY=build/mok-test/falcon-test-mok.key
MOK_CRT=build/mok-test/falcon-test-mok.crt
SHIM=build/shim/bins             # the shim cases' --shim; noshim has none
SHELF_GUID=9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF
GLOBAL_GUID=8be4df61-93ca-11d2-aa0d-00e098032b8c
BOOTMGFW='\EFI\Microsoft\Boot\bootmgfw.efi'
FALCON_SHIM='\EFI\falcon\shimx64.efi'
TIMEOUT=240
# The NVMe controller's PCI slot, fixed so falcon's Boot0001 can name it.
NVME_SLOT=5

export MTOOLS_SKIP_CHECK=1
mkdir -p "$R"

LOOP=
MNT=
MAPPER=
QPID=

cleanup() {
    [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null || true
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
    [[ -n "$MAPPER" ]] && sudo cryptsetup close "$MAPPER" 2>/dev/null || true
    [[ -n "$LOOP" ]] && sudo losetup -d "$LOOP" 2>/dev/null || true
    rm -f "$R/pass"
}
trap cleanup EXIT

# ---- disks ----

attach() {      # attach IMG [losetup args...]: sets $LOOP
    local img=$1
    shift
    LOOP=$(sudo losetup --show -f -P "$@" "$img")
    sudo udevadm settle 2>/dev/null || sleep 1
}

detach() {
    sudo losetup -d "$LOOP"
    LOOP=
}

# A LUKS2 shelf on DEV: pbkdf2, ext4, /uki/personal.efi and the MOK pair,
# no falcon token (the installer stamps it).
make_shelf() {
    printf '%s' "$PASS" | sudo cryptsetup luksFormat --type luks2 \
        --pbkdf pbkdf2 --pbkdf-force-iterations 100000 --batch-mode "$1" -
    MAPPER=falcon-installed-rig-$$
    printf '%s' "$PASS" | sudo cryptsetup open "$1" "$MAPPER" -
    sudo mkfs.ext4 -q -L FALCON-UKI "/dev/mapper/$MAPPER"
    MNT=$(mktemp -d)
    sudo mount "/dev/mapper/$MAPPER" "$MNT"
    sudo mkdir "$MNT/uki"
    sudo cp "$FIXTURE_UKI_SIGNED" "$MNT/uki/personal.efi"
    sudo install -m 0600 "$MOK_KEY" "$MNT/falcon-mok.key"
    sudo install -m 0644 "$MOK_CRT" "$MNT/falcon-mok.crt"
    sudo umount "$MNT"
    rmdir "$MNT"
    MNT=
    sudo cryptsetup close "$MAPPER"
    MAPPER=
}

# A partition boot sector naming BOOTMGR, as a Windows partition's does.
bootmgr_sector() {  # bootmgr_sector DEV-OR-IMG BYTE-OFFSET
    printf '\xeb\x52\x90NTFS    ' |
        sudo dd of="$1" bs=1 seek="$2" conv=notrunc status=none
    printf 'BOOTMGR is missing' |
        sudo dd of="$1" bs=1 seek=$(( $2 + 0x180 )) conv=notrunc status=none
    printf '\x55\xaa' |
        sudo dd of="$1" bs=1 seek=$(( $2 + 510 )) conv=notrunc status=none
}

# `mkstick station install`, as root for the shelf's token; its output is
# kept beside the disk.
install_station() {  # install_station NAME TARGET ARGS...
    local name=$1 target=$2
    shift 2
    ( umask 077; printf '%s' "$PASS" > "$R/pass" )
    # shellcheck disable=SC2024  # the log is ours, not root's
    sudo "$MKSTICK" station install "$target" --passphrase-file "$R/pass" \
        --backup "$R/$name.bak" --force "$@" >"$R/$name.install" 2>&1 || {
        rm -f "$R/pass"
        echo "ERROR: station install on $name failed:" >&2
        tail -5 "$R/$name.install" >&2
        exit 1
    }
    rm -f "$R/pass"
    sudo chown "$(id -u):$(id -g)" "$R/$name.bak"
}

# The partition facts a firmware entry's HARDDRIVE node carries, in the
# disk's own sectors: "start size guid".
part_facts() {      # part_facts DEV-OR-IMG N
    sudo sgdisk -i "$2" "$1" | awk '
        /^First sector:/          { s = $3 }
        /^Partition size:/        { n = $3 }
        /^Partition unique GUID:/ { g = $4 }
        END { print s, n, g }'
}

build_gpt() {
    local img=$R/gpt.img

    rm -f "$img"
    truncate -s 256M "$img"
    sgdisk -n 1:2048:+64M -t 1:EF00 -c 1:EFI \
        -n 2:0:+32M -t 2:0700 -c "2:Basic data" \
        -n 3:0:+64M -t 3:"$SHELF_GUID" -c 3:FALCON-UKI "$img" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n SYSTEM "$img" $(( 64 * 1024 )) >/dev/null 2>&1
    mmd -i "$img@@1M" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot
    mcopy -i "$img@@1M" "$STANDIN_SIGNED" ::/EFI/Microsoft/Boot/bootmgfw.efi
    bootmgr_sector "$img" $(( $(part_facts "$img" 2 | cut -d' ' -f1) * 512 ))
    attach "$img"
    make_shelf "${LOOP}p3"
    detach
    install_station gpt "$img" --uefi --shelf 3 --memtest-efi "$STANDIN" --shim "$SHIM"
    part_facts "$img" 1 > "$R/gpt.esp"
}

build_4kn() {
    local img=$R/4kn.img

    rm -f "$img"
    truncate -s 1G "$img"
    attach "$img" --sector-size 4096
    sudo sgdisk -n 1:1MiB:+300MiB -t 1:EF00 -c 1:EFI \
        -n 2:0:+64MiB -t 2:"$SHELF_GUID" -c 2:FALCON-UKI "$LOOP" >/dev/null
    sudo partx -u "$LOOP" 2>/dev/null || true
    sudo udevadm settle 2>/dev/null || sleep 1
    sudo mkfs.fat -F 32 -n SYSTEM "${LOOP}p1" >/dev/null
    make_shelf "${LOOP}p2"
    cp --sparse=always "$img" "$R/4kn.orig.img"
    # Through a link named like a by-id one: the partitions are the
    # kernel's node's (loopNp2), never the link's name with a 2.
    ln -sfn "$LOOP" "$R/by-id-4kn-0:0"
    install_station 4kn "$R/by-id-4kn-0:0" --shelf 2 --memtest-efi "$STANDIN" --shim "$SHIM"
    rm -f "$R/by-id-4kn-0:0"
    part_facts "$LOOP" 1 > "$R/4kn.esp"
    detach
}

build_mbr4k() {
    local img=$R/mbr4k.img

    rm -f "$img"
    truncate -s 1G "$img"
    attach "$img" --sector-size 4096
    printf 'label: dos\nlabel-id: 0x4b4e0001\nstart=256, size=32768, type=7\nsize=16384, type=83\n' |
        sudo sfdisk -q "$LOOP"
    sudo partx -u "$LOOP" 2>/dev/null || true
    sudo udevadm settle 2>/dev/null || sleep 1
    bootmgr_sector "${LOOP}p1" 0
    make_shelf "${LOOP}p2"
    install_station mbr4k "$LOOP" --bios --shelf 2
    detach
}

# The Windows disk of gpt2: GPT with a fixed DiskGUID, its own ESP carrying
# the stand-in as bootmgfw.efi, and a Windows partition.
WIN2_DISK=5EEDD15C-0000-4000-8000-000000000002
build_gpt2() {
    local img=$R/gpt2.img win=$R/win2.img

    rm -f "$img" "$win"
    truncate -s 64M "$win"
    sgdisk -U "$WIN2_DISK" -n 1:2048:+40M -t 1:EF00 -c 1:EFI \
        -n 2:0:0 -t 2:0700 -c "2:Basic data" "$win" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n WINESP "$win" $(( 40 * 1024 )) >/dev/null 2>&1
    mmd -i "$win@@1M" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot
    mcopy -i "$win@@1M" "$STANDIN_SIGNED" ::/EFI/Microsoft/Boot/bootmgfw.efi
    bootmgr_sector "$win" $(( $(part_facts "$win" 2 | cut -d' ' -f1) * 512 ))
    part_facts "$win" 1 > "$R/win2.esp"

    truncate -s 256M "$img"
    sgdisk -n 1:2048:+64M -t 1:EF00 -c 1:EFI \
        -n 2:0:+64M -t 2:"$SHELF_GUID" -c 2:FALCON-UKI "$img" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n SYSTEM "$img" $(( 64 * 1024 )) >/dev/null 2>&1
    attach "$img"
    make_shelf "${LOOP}p2"
    detach
    install_station gpt2 "$img" --uefi --shelf 2 --also "$win" \
        --memtest-efi "$STANDIN" --shim "$SHIM"
    part_facts "$img" 1 > "$R/gpt2.esp"
}

# An MBR disk with an ESP (0xEF) and a shelf; its ESP's facts as the
# firmware's HARDDRIVE node names an MBR partition.
MBR_SIG=4d425201
build_mbr() {
    local img=$R/mbr.img

    rm -f "$img"
    truncate -s 256M "$img"
    printf 'label: dos\nlabel-id: 0x%s\nstart=2048, size=131072, type=ef\nsize=131072, type=83\n' \
        "$MBR_SIG" | sfdisk -q --no-reread --no-tell-kernel "$img" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n SYSTEM "$img" $(( 64 * 1024 )) >/dev/null 2>&1
    attach "$img"
    make_shelf "${LOOP}p2"
    detach
    install_station mbr "$img" --uefi --shelf 2 --memtest-efi "$STANDIN" --shim "$SHIM"
    echo "2048 131072 $(od -An -tx4 -j440 -N4 "$img" | tr -d ' ') 1" \
        > "$R/mbr.esp"
}

build_noshim() {
    local img=$R/noshim.img

    rm -f "$img"
    truncate -s 256M "$img"
    sgdisk -n 1:2048:+64M -t 1:EF00 -c 1:EFI \
        -n 2:0:+64M -t 2:"$SHELF_GUID" -c 2:FALCON-UKI "$img" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n SYSTEM "$img" $(( 64 * 1024 )) >/dev/null 2>&1
    attach "$img"
    make_shelf "${LOOP}p2"
    detach
    install_station noshim "$img" --uefi --shelf 2 \
        --memtest-efi "$STANDIN"
    part_facts "$img" 1 > "$R/noshim.esp"
}

# noshim's disk with another system's \EFI\BOOT\BOOTX64.EFI already on the
# ESP (the stand-in), which the installer must leave as it was.
build_foreign() {
    local img=$R/foreign.img

    rm -f "$img"
    truncate -s 256M "$img"
    sgdisk -n 1:2048:+64M -t 1:EF00 -c 1:EFI \
        -n 2:0:+64M -t 2:"$SHELF_GUID" -c 2:FALCON-UKI "$img" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n SYSTEM "$img" $(( 64 * 1024 )) >/dev/null 2>&1
    mmd -i "$img@@1M" ::/EFI ::/EFI/BOOT
    mcopy -i "$img@@1M" "$STANDIN_SIGNED" ::/EFI/BOOT/BOOTX64.EFI
    attach "$img"
    make_shelf "${LOOP}p2"
    detach
    install_station foreign "$img" --uefi --shelf 2
}

build_all() {
    make --no-print-directory station cli "$STANDIN" "$STANDIN_SIGNED" \
        >/dev/null
    local f
    for f in "$FIXTURE_UKI_SIGNED" "$MOK_KEY" "$MOK_CRT" \
             build/shim/bins/fedora-16.1-shimx64.efi; do
        [[ -f "$f" ]] || { echo "ERROR: $f missing (make test-uki sign)" >&2; exit 1; }
    done
    if [[ "${KEEP:-0}" == 1 && -f "$R/gpt.img" && -f "$R/4kn.img" &&
          -f "$R/mbr4k.img" && -f "$R/gpt2.img" && -f "$R/mbr.img" &&
          -f "$R/noshim.img" && -f "$R/foreign.img" ]]; then
        return
    fi
    echo "== installing the rig's stations"
    build_gpt
    build_4kn
    build_mbr4k
    build_gpt2
    build_mbr
    build_noshim
    build_foreign
}

# ---- the machine ----

log_text() {
    tr -d '\r' 2>/dev/null < "$1" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}

mon() { printf '%s\n' "$*" | socat - "UNIX-CONNECT:$R/mon.sock" >/dev/null 2>&1 || true; }

wait_for() {    # wait_for PATTERN SECONDS
    local i
    for (( i = 0; i < $2 * 10; i++ )); do
        log_text "$R/serial.log" | grep -aqF -- "$1" && return 0
        kill -0 "$QPID" 2>/dev/null || { log_text "$R/serial.log" |
            grep -aqF -- "$1"; return; }
        sleep 0.1
    done
    return 1
}

has() { log_text "$R/serial.log" | grep -aqF -- "$1"; }

# wait_count PATTERN N SECONDS: until PATTERN has appeared N times. The
# unlock's lines are the same after every wrong passphrase (DESIGN Section
# 5.2 says nothing of the wait), so progress is counted, not matched.
wait_count() {
    local i
    for (( i = 0; i < $3 * 10; i++ )); do
        [[ $(log_text "$R/serial.log" | grep -aoF -- "$1" | wc -l) -ge $2 ]] && return 0
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

send_keys() {
    local s=$1 i c
    for (( i = 0; i < ${#s}; i++ )); do
        c=${s:i:1}
        case $c in
        [a-z0-9]) mon "sendkey $c" ;;
        -)        mon "sendkey minus" ;;
        *) echo "ERROR: no sendkey for '$c'" >&2; exit 2 ;;
        esac
        sleep 0.05
    done
    mon "sendkey ret"
}

# vars JSON: falcon's Boot0001 on the station disk's ESP, and a "Windows
# Boot Manager" Boot0009 naming bootmgfw.efi: with WIN 1 on the same ESP,
# with WIN 2 on the second disk's ESP (WINFACTS, SATA port 1). FACTS are
# "START SIZE GUID" for a GPT partition or "START SIZE SIG N" for an MBR
# one, in the disk's own sectors. The device path is the firmware's whole
# path to the disk: OVMF drops a boot option whose path matches no device
# QEMU's bootorder lists.
vars_json() {   # vars_json sata|nvme FACTS WIN [WINFACTS]; ENTRY: falcon's file
    python3 - "$1" "$2" "$3" "${ENTRY:-$FALCON_SHIM}" "$BOOTMGFW" "$GLOBAL_GUID" \
        "$NVME_SLOT" "${4:-}" <<'EOF'
import json, struct, sys, uuid
bus, facts, win, shim, bootmgfw, gguid, slot, winfacts = sys.argv[1:9]
def prefix(port):
    pre = struct.pack("<BBHII", 2, 1, 12, 0x0A0341D0, 0)
    if bus == "sata":
        # PciRoot(0)/Pci(0x1f,0x2)/Sata(port,0xFFFF,0): q35's AHCI.
        pre += struct.pack("<BBHBB", 1, 1, 6, 2, 0x1F)
        pre += struct.pack("<BBHHHH", 3, 0x12, 10, port, 0xFFFF, 0)
    else:
        # PciRoot(0)/Pci(slot,0)/NVMe(0x1,00-00-00-00-00-00-00-00).
        pre += struct.pack("<BBHBB", 1, 1, 6, 0, int(slot))
        pre += struct.pack("<BBHI", 3, 0x17, 16, 1) + bytes(8)
    return pre
def harddrive(f):
    f = f.split()
    if len(f) == 3:
        hd = struct.pack("<BBHIQQ", 4, 1, 42, 1, int(f[0]), int(f[1]))
        return hd + uuid.UUID(f[2]).bytes_le + bytes([2, 2])
    # MBR: the partition's number, and the disk signature in the node's
    # first four signature bytes (MBRType 1, SignatureType 1).
    hd = struct.pack("<BBHIQQ", 4, 1, 42, int(f[3]), int(f[0]), int(f[1]))
    return hd + struct.pack("<I", int(f[2], 16)) + bytes(12) + bytes([1, 1])
end = bytes([0x7F, 0xFF, 4, 0])
def option(desc, pre, hd, path):
    name = (path + "\0").encode("utf-16-le")
    fp = struct.pack("<BBH", 4, 4, 4 + len(name)) + name
    dp = pre + hd + fp + end
    d = (desc + "\0").encode("utf-16-le")
    return (struct.pack("<IH", 1, len(dp)) + d + dp).hex()
v = []
order = []
# ENTRY=none: the firmware has no entry for the station (an NVRAM reset),
# and boots the disk's removable-media path.
if shim != "none":
    v.append({"name": "Boot0001", "guid": gguid, "attr": 7,
              "data": option("falcon", prefix(0), harddrive(facts), shim)})
    order.append(1)
if win in ("1", "2"):
    where = (prefix(0), harddrive(facts)) if win == "1" else \
        (prefix(1), harddrive(winfacts))
    v.append({"name": "Boot0009", "guid": gguid, "attr": 7,
              "data": option("Windows Boot Manager", *where, bootmgfw)})
    order.append(9)
if order:
    v.append({"name": "BootOrder", "guid": gguid, "attr": 7,
              "data": b"".join(struct.pack("<H", n) for n in order).hex()})
print(json.dumps({"version": 2, "variables": v}))
EOF
}

# boot FIRMWARE IMG BUS FACTS WIN [WINFACTS]: the station's disk, and
# SECOND (a disk image, a copy of it on SATA port 1) when it is set. OVMF under
# KVM on a loaded host now and then stops before its boot manager says
# anything (run-station-rig.sh has the same); one that has not reached
# BdsDxe in a minute is started again once.
boot() {
    boot_once "$@"
    if [[ $1 != bios ]] && ! wait_for "BdsDxe: " 60; then
        halt
        echo "note: $1: the firmware stopped before its boot manager; started again" >&2
        boot_once "$@"
    fi
}

boot_once() {
    local fw=$1 img=$2 bus=$3 facts=$4 win=$5 winfacts=${6:-} code vars dev
    local fwargs=() second=()
    if [[ $bus == nvme ]]; then
        dev="nvme,drive=d0,serial=falcon4k,addr=$NVME_SLOT,logical_block_size=4096,physical_block_size=4096,bootindex=0"
    else
        dev="ide-hd,drive=d0,bus=ide.0,bootindex=0"
    fi
    case $fw in
    uefi|uefi-sb)
        if [[ $fw == uefi ]]; then
            code=$(ovmf_find CODE plain)
            vars=$(ovmf_find VARS plain)
        else
            code=$(ovmf_find CODE secboot)
            vars=build/mok-test/OVMF_VARS.secboot.mok.fd
        fi
        cp "$vars" "$R/vars.fd"
        chmod u+w "$R/vars.fd"
        vars_json "$bus" "$facts" "$win" "$winfacts" > "$R/vars.json"
        virt-fw-vars --inplace "$R/vars.fd" --set-json "$R/vars.json" \
            >/dev/null 2>&1
        if [[ $fw == uefi-sb ]]; then
            # The stand-in Windows is signed with the test MOK; its cert
            # in db stands in for Microsoft's.
            virt-fw-vars --inplace "$R/vars.fd" \
                --add-db 5eedd15c-0000-4000-8000-00000000d00b "$MOK_CRT" \
                >/dev/null 2>&1
        fi
        fwargs=(-machine q35
                -drive "if=pflash,format=raw,readonly=on,file=$code"
                -drive "if=pflash,format=raw,file=$R/vars.fd")
        ;;
    bios) fwargs=(-machine pc) ;;
    esac
    # Each boot writes a copy, so the installed disk stays as installed.
    cp --sparse=always "$img" "$R/boot.img"
    if [[ -n "${SECOND:-}" ]]; then
        cp --sparse=always "$SECOND" "$R/boot2.img"
        second=(-drive "if=none,id=d1,format=raw,file=$R/boot2.img"
                -device "ide-hd,drive=d1,bus=ide.1,bootindex=1")
    fi
    rm -f "$R/serial.log" "$R/mon.sock"
    timeout "$TIMEOUT" qemu-system-x86_64 "${fwargs[@]}" -m 1024 \
        -enable-kvm -cpu host -display none -net none \
        -drive "if=none,id=d0,format=raw,file=$R/boot.img" -device "$dev" \
        "${second[@]}" \
        -serial "file:$R/serial.log" \
        -monitor "unix:$R/mon.sock,server,nowait" >"$R/qemu.log" 2>&1 &
    QPID=$!
}

halt() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
    rm -f "$R/boot.img" "$R/boot2.img"
}

FAILED=0
result() {      # result CASE WHY DETAIL
    if [[ -z "$2" ]]; then
        printf 'PASS  %-16s %s\n' "$1" "$3"
    else
        printf 'FAIL  %-16s %s\n' "$1" "$2"
        FAILED=1
    fi
    log_text "$R/serial.log" > "$R/$1.log" 2>/dev/null || true
}

# The install ID of a disk's boot records (a disk with both stations has
# two, carrying one ID).
install_id() {  # install_id DEV-OR-IMG
    sudo "$MKSTICK" ident "$1" | awk -F '\t' '$2 == "boot" { print $3; exit }'
}

# From the passphrase prompt on: the passphrase and the UKI's report.
# Sets WHY on a failure.
unlock() {      # unlock ID SECONDS
    wait_for "passphrase for the encrypted partition" "$2" ||
        { WHY="no passphrase prompt"; return; }
    sleep 0.5
    send_keys "$PASS"
    wait_for "FALCON-UKI-DONE" 120 || { WHY="the UKI never reported"; return; }
    has "FALCON-CRED-OK" || { WHY="the passphrase credential is wrong"; return; }
    has "FALCON-CRED-HEAD falcon.ident.cred [falcon 1 boot $1" ||
        WHY="falcon.ident.cred missing or not install $1"
}

# ---- the cases ----

case_uefi_default() {   # case_uefi_default CASE FIRMWARE
    local c=$1 fw=$2
    WHY=
    boot "$fw" "$R/gpt.img" sata "$(cat "$R/gpt.esp")" 1
    wait_for "Booting in 5 s" 90 ||
        WHY="no countdown"
    # OVMF's terminal gets the countdown line before the rows; by the
    # count's second line every row has been drawn.
    [[ -z "$WHY" ]] && { wait_for "Booting in 4 s" 5 || WHY="the count stopped"; }
    [[ -z "$WHY" ]] && { wait_for "Personal OS (encrypted)" 5 || WHY="no Personal OS row"; }
    # The INI's [windows] names the disk, matches the firmware entry, and
    # labels its row.
    [[ -z "$WHY" ]] && { has "Windows Boot Manager (disk gpt:" || WHY="no Windows row"; }
    # The entry's own label, "Windows Boot Manager" and nothing after it
    # but the row's padding, would be a second row.
    [[ -z "$WHY" ]] &&
        log_text "$R/serial.log" | grep -aqE 'Windows Boot Manager( {2,}|$)' &&
        WHY="the firmware entry is a row of its own beside the INI's"
    [[ -z "$WHY" ]] && { wait_for "Memory test (memtest86+)" 5 || WHY="no memtest row"; }
    [[ -z "$WHY" ]] && { wait_for "booting: Windows Boot Manager (disk gpt:" 15 ||
        WHY="the default was not Windows"; }
    [[ -z "$WHY" ]] && { wait_for "restarting into it" 10 || WHY="no NXT page"; }
    [[ -z "$WHY" ]] && { wait_for "FALCON-STANDIN $BOOTMGFW" 90 ||
        WHY="the firmware did not start Windows after the reset"; }
    halt
    result "$c" "$WHY" "the installer's INI counted down to Windows; BootNext started $BOOTMGFW"
}

case_uefi_pos() {       # case_uefi_pos CASE FIRMWARE
    local c=$1 fw=$2 id
    WHY=
    id=$(install_id "$R/gpt.img")
    boot "$fw" "$R/gpt.img" sata "$(cat "$R/gpt.esp")" 1
    wait_for "Booting in 5 s" 90 || WHY="no countdown"
    if [[ -z "$WHY" ]]; then
        # Up stops the count and moves from the default (Windows) to the
        # Personal OS, the first row.
        mon "sendkey up"
        sleep 0.5
        mon "sendkey ret"
        unlock "$id" 30
    fi
    [[ -z "$WHY" ]] && has "FALCON-STANDIN" && WHY="Windows started after a key"
    halt
    result "$c" "$WHY" "a key stopped the count; the shelf of install ${id:0:8}... unlocked to the UKI"
}

case_4kn_uefi() {       # case_4kn_uefi CASE FIRMWARE
    local c=$1 fw=$2 id
    WHY=
    id=$(cat "$R/4kn.id")
    boot "$fw" "$R/4kn.img" nvme "$(cat "$R/4kn.esp")" 0
    wait_for "Booting in 5 s" 90 ||
        WHY="no countdown"
    # OVMF's terminal gets the countdown line before the rows; by the
    # count's second line every row has been drawn.
    [[ -z "$WHY" ]] && { wait_for "Booting in 4 s" 5 || WHY="the count stopped"; }
    [[ -z "$WHY" ]] && { wait_for "personal.efi" 5 || WHY="no row for the INI's [linux]"; }
    [[ -z "$WHY" ]] && { wait_for "booting: personal.efi" 15 ||
        WHY="the default was not the shelf's UKI"; }
    [[ -z "$WHY" ]] && unlock "$id" 30
    halt
    result "$c" "$WHY" "4096-byte sectors: the default counted down to the prompt; UKI got both credentials"
}

case_bios() {           # case_bios CASE IMG PART
    local c=$1 img=$2 part=$3 id
    WHY=
    id=$(cat "${img%.img}.id")
    boot bios "$img" nvme - 0
    wait_for "boot partition p$part at LBA" 60 ||
        WHY="stage 2 did not take partition $part"
    [[ -z "$WHY" ]] && { wait_for "install $id" 30 ||
        WHY="stage 3 did not report install $id"; }
    # The installer's STATION.INI counts down to its default, which on the
    # MBR disk is the Windows placeholder (no boot code, and a 4Kn disk's
    # partition chain is refused by design). Up stops the count and moves
    # to the Personal OS, the first row, as case_uefi_pos does.
    [[ -z "$WHY" ]] && { wait_for "Booting in 5 s" 60 || WHY="no countdown"; }
    if [[ -z "$WHY" ]]; then
        mon "sendkey up"
        sleep 0.5
        mon "sendkey ret"
        unlock "$id" 60
    fi
    halt
    result "$c" "$WHY" "4096-byte sectors: stage 2 took p$part, a key stopped the count, the shelf unlocked to the UKI"
}

# QEMU exits when the guest powers off.
wait_exit() {   # wait_exit SECONDS
    local i
    for (( i = 0; i < $1 * 10; i++ )); do
        kill -0 "$QPID" 2>/dev/null || return 0
        sleep 0.1
    done
    return 1
}

case_bios_bad() {       # case_bios_bad CASE IMG
    local c=$1 img=$2 k d t0
    WHY=
    boot bios "$img" nvme - 0
    wait_for "Booting in 5 s" 90 || WHY="no countdown"
    if [[ -z "$WHY" ]]; then
        mon "sendkey up"
        sleep 0.5
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 60 || WHY="no passphrase prompt"
    fi
    for k in 1 2 3 4; do
        [[ -n "$WHY" ]] && break
        d=$(( k == 1 ? 0 : 2 << (k - 1) ))
        sleep 0.5
        t0=$SECONDS
        send_keys "wrong$k"
        wait_count "wrong passphrase." "$k" $(( d + 60 )) || { WHY="no refusal after try $k"; break; }
        (( SECONDS - t0 >= d - 1 )) || { WHY="no $d s wait after try $k"; break; }
        wait_count "passphrase for the encrypted partition" $(( k + 1 )) 30 ||
            WHY="no prompt after try $k"
    done
    if [[ -z "$WHY" ]]; then
        sleep 0.5
        send_keys wrong5
        wait_count "wrong passphrase." 5 60 || WHY="no refusal after try 5"
    fi
    [[ -z "$WHY" ]] && { wait_exit 30 || WHY="the machine was still on"; }
    [[ -z "$WHY" ]] && has "FALCON-UKI" && WHY="a UKI started"
    halt
    result "$c" "$WHY" "BIOS: five wrong, waits of 0, 4, 8 and 16 s, then powered off"
}

# gpt2: Windows on the second disk, through its firmware entry (nxt) or
# its ESP (esp).
case_second() {         # case_second CASE FIRMWARE nxt|esp
    local c=$1 fw=$2 how=$3 win=0
    WHY=
    [[ $how == nxt ]] && win=2
    SECOND=$R/win2.img boot "$fw" "$R/gpt2.img" sata "$(cat "$R/gpt2.esp")" \
        "$win" "$(cat "$R/win2.esp")"
    wait_for "Booting in 5 s" 90 ||
        WHY="no countdown"
    # OVMF's terminal gets the countdown line before the rows; by the
    # count's second line every row has been drawn.
    [[ -z "$WHY" ]] && { wait_for "Booting in 4 s" 5 || WHY="the count stopped"; }
    [[ -z "$WHY" ]] && { wait_for "Personal OS (encrypted)" 5 || WHY="no Personal OS row"; }
    [[ -z "$WHY" ]] && { wait_for "booting: Windows" 15 ||
        WHY="the default was not Windows"; }
    if [[ -z "$WHY" && $how == nxt ]]; then
        wait_for "restarting into it" 10 || WHY="no NXT page"
        [[ -z "$WHY" ]] && { wait_for "FALCON-STANDIN $BOOTMGFW" 90 ||
            WHY="the firmware did not start Windows after the reset"; }
    elif [[ -z "$WHY" ]]; then
        wait_for "FALCON-STANDIN" 30 || WHY="bootmgfw.efi did not start"
        [[ -z "$WHY" ]] && has "restarting into it" &&
            WHY="the ESP row went through a reset"
        [[ -z "$WHY" ]] && { has "BitLocker will ask for the recovery key" ||
            WHY="no ESP page"; }
    fi
    halt
    if [[ $how == nxt ]]; then
        result "$c" "$WHY" "the INI's [windows] on the second disk matched its entry; BootNext started $BOOTMGFW"
    else
        result "$c" "$WHY" "the INI's [windows] was the second disk's ESP; bootmgfw.efi loaded from it, no reset"
    fi
}

# mbr: the UEFI station on an MBR disk's ESP.
case_mbr_uefi() {       # case_mbr_uefi CASE FIRMWARE
    local c=$1 fw=$2 id
    WHY=
    id=$(install_id "$R/mbr.img")
    boot "$fw" "$R/mbr.img" sata "$(cat "$R/mbr.esp")" 0
    wait_for "Booting in 5 s" 90 ||
        WHY="no countdown"
    # OVMF's terminal gets the countdown line before the rows; by the
    # count's second line every row has been drawn.
    [[ -z "$WHY" ]] && { wait_for "Booting in 4 s" 5 || WHY="the count stopped"; }
    [[ -z "$WHY" ]] && { wait_for "personal.efi" 5 || WHY="no row for the INI's [linux]"; }
    [[ -z "$WHY" ]] && { wait_for "Memory test (memtest86+)" 5 || WHY="no memtest row"; }
    [[ -z "$WHY" ]] && { wait_for "booting: personal.efi" 15 ||
        WHY="the default was not the shelf's UKI"; }
    [[ -z "$WHY" ]] && unlock "$id" 30
    halt
    result "$c" "$WHY" "an ESP on MBR (mbr:$MBR_SIG): the default counted down to the prompt; UKI got both credentials"
}

# noshim: the `falcon` entry names the jumper, which starts the loader; no
# shim in front of either.
case_noshim() {         # case_noshim CASE FIRMWARE
    local c=$1 fw=$2 id
    WHY=
    id=$(install_id "$R/noshim.img")
    ENTRY='\EFI\BOOT\BOOTX64.EFI' boot "$fw" "$R/noshim.img" sata \
        "$(cat "$R/noshim.esp")" 0
    wait_for "Booting in 5 s" 90 ||
        WHY="no countdown"
    [[ -z "$WHY" ]] && { wait_for "booting: personal.efi" 15 ||
        WHY="the default was not the shelf's UKI"; }
    [[ -z "$WHY" ]] && unlock "$id" 30
    halt
    result "$c" "$WHY" "the falcon entry started the jumper, the jumper falconx64.efi; the default counted down to the prompt; UKI got both credentials"
}

# noshim with no firmware entry for the station: the firmware boots the
# disk's \EFI\BOOT\BOOTX64.EFI, the jumper the installer put there, and the
# jumper starts \EFI\falcon\falconx64.efi.
case_noshim_jump() {    # case_noshim_jump CASE FIRMWARE
    local c=$1 fw=$2 id n
    WHY=
    id=$(install_id "$R/noshim.img")
    grep -q 'BOOTX64.EFI, the jumper to falconx64.efi' "$R/noshim.install" ||
        WHY="the installer did not write the jumper"
    if [[ -z "$WHY" ]]; then
        ENTRY=none boot "$fw" "$R/noshim.img" sata \
            "$(cat "$R/noshim.esp")" 0
        # The jumper's one line, when it has one, is the reason.
        wait_for "Booting in 5 s" 90 ||
            WHY="no countdown $(log_text "$R/serial.log" |
                grep -ao 'falcon-esp-jump: .*' | head -1)"
        # Counted, not grep -q: the line is near the top of a long log, and
        # a grep that stops reading there fails the pipe under pipefail.
        n=$(log_text "$R/serial.log" |
            grep -acF 'starting Boot0001 "UEFI QEMU HARDDISK' || true)
        [[ -z "$WHY" && "$n" == 0 ]] &&
            WHY="the firmware did not boot the disk's removable-media path"
        [[ -z "$WHY" ]] && { wait_for "booting: personal.efi" 15 ||
            WHY="the default was not the shelf's UKI"; }
        [[ -z "$WHY" ]] && unlock "$id" 30
        halt
    fi
    result "$c" "$WHY" "no firmware entry: BOOTX64.EFI, the jumper, started falconx64.efi; the default counted down to the prompt; UKI got both credentials"
}

case_noshim_status() {
    local c=$1
    WHY=
    : > "$R/serial.log"
    # shellcheck disable=SC2024
    sudo "$MKSTICK" station status "$R/noshim.img" --no-scan \
        >"$R/noshim.status" 2>&1 || WHY="station status failed"
    [[ -z "$WHY" ]] && { grep -q 'no shim: the jumper starts falconx64.efi' "$R/noshim.status" ||
        WHY="status did not say there is no shim"; }
    [[ -z "$WHY" ]] && { grep -q '^  OK .*falconx64.efi is signed by falcon-boot.cer' "$R/noshim.status" ||
        WHY="status did not check the loader's signature"; }
    [[ -z "$WHY" ]] && { grep -q '^  OK .*BOOTX64.EFI is the jumper, signed by falcon-boot.cer' "$R/noshim.status" ||
        WHY="status did not check the jumper"; }
    [[ -z "$WHY" ]] && mdir -b -i "$R/noshim.img@@1M" ::/EFI/falcon 2>/dev/null |
        grep -qiE 'shimx64|mmx64' && WHY="the ESP has a shim or MokManager"
    result "$c" "$WHY" "status: no shim, the loader and the jumper signed by the cert beside them"
}

# `mkstick update` on a copy of noshim: the loader and the jumper are both
# signed again, the jumper's old copy saved first.
case_noshim_update() {
    local c=$1 out=$R/noshim-update
    WHY=
    : > "$R/serial.log"
    rm -rf "$out"
    mkdir -p "$out"
    cp --sparse=always "$R/noshim.img" "$out/disk.img"
    # shellcheck disable=SC2024
    sudo "$MKSTICK" update "$out/disk.img" --key "$MOK_KEY" --cert "$MOK_CRT" \
        --backup-dir "$out" --yes >"$out/update.log" 2>&1 ||
        WHY="update failed: $(tail -1 "$out/update.log")"
    [[ -z "$WHY" ]] && { grep -q 'write: .EFI.BOOT.BOOTX64.EFI, the jumper, signed' "$out/update.log" ||
        WHY="update did not plan the jumper"; }
    [[ -z "$WHY" ]] && { grep -q 'UEFI: /EFI/BOOT/BOOTX64.EFI signed, written and read back; the one it replaced' "$out/update.log" ||
        WHY="update did not write the jumper"; }
    [[ -z "$WHY" ]] && { ls "$out"/disk.img-esp-jump-*.efi >/dev/null 2>&1 ||
        WHY="the old jumper was not saved"; }
    if [[ -z "$WHY" ]]; then
        mcopy -n -i "$out/disk.img@@1M" ::/EFI/BOOT/BOOTX64.EFI "$out/jump.efi"
        sbverify --cert "$MOK_CRT" "$out/jump.efi" >/dev/null 2>&1 ||
            WHY="the jumper on the disk does not verify"
    fi
    rm -rf "$out"
    result "$c" "$WHY" "update: falconx64.efi and the jumper signed again, the old jumper saved"
}

# foreign: another system's BOOTX64.EFI before the install. The installer
# saves it beside the backup and puts the jumper there; `station remove`
# puts it back byte for byte.
case_foreign_jump() {
    local c=$1 out=$R/foreign-remove
    WHY=
    : > "$R/serial.log"
    rm -rf "$out"
    mkdir -p "$out"
    grep -q "BOOTX64.EFI was another system's; saved to $R/foreign.bak.BOOTX64.EFI" \
        "$R/foreign.install" ||
        WHY="the installer did not say where it saved BOOTX64.EFI"
    [[ -z "$WHY" ]] && { cmp -s "$R/foreign.bak.BOOTX64.EFI" "$STANDIN_SIGNED" ||
        WHY="the saved BOOTX64.EFI is not the other system's"; }
    if [[ -z "$WHY" ]]; then
        mcopy -n -i "$R/foreign.img@@1M" ::/EFI/BOOT/BOOTX64.EFI "$out/jump.efi"
        sbverify --cert "$MOK_CRT" "$out/jump.efi" >/dev/null 2>&1 ||
            WHY="BOOTX64.EFI after the install is not the signed jumper"
    fi
    if [[ -z "$WHY" ]]; then
        cp --sparse=always "$R/foreign.img" "$out/disk.img"
        # shellcheck disable=SC2024
        sudo "$MKSTICK" station remove "$out/disk.img" \
            --backup "$R/foreign.bak" >"$out/remove.log" 2>&1 ||
            WHY="station remove failed: $(tail -1 "$out/remove.log")"
        [[ -z "$WHY" ]] && { grep -q 'restored .EFI.BOOT.BOOTX64.EFI from' "$out/remove.log" ||
            WHY="remove did not say it restored BOOTX64.EFI"; }
        if [[ -z "$WHY" ]]; then
            mcopy -n -i "$out/disk.img@@1M" ::/EFI/BOOT/BOOTX64.EFI "$out/after.efi"
            cmp -s "$out/after.efi" "$STANDIN_SIGNED" ||
                WHY="BOOTX64.EFI after remove is not the other system's"
        fi
    fi
    rm -rf "$out"
    result "$c" "$WHY" "another system's BOOTX64.EFI: saved by install, replaced by the jumper, put back by remove"
}

# A station without shim as installed before the jumper (the loader as
# \EFI\falcon\grubx64.efi, the entry naming it) with another system's
# BOOTX64.EFI, made from a copy of noshim; `mkstick update` migrates it,
# and the entry naming the jumper boots it.
case_noshim_migrate() { # case_noshim_migrate CASE FIRMWARE
    local c=$1 fw=$2 out=$R/noshim-migrate id
    WHY=
    : > "$R/serial.log"
    rm -rf "$out"
    mkdir -p "$out"
    id=$(install_id "$R/noshim.img")
    cp --sparse=always "$R/noshim.img" "$out/disk.img"
    mren -i "$out/disk.img@@1M" ::/EFI/falcon/falconx64.efi ::/EFI/falcon/grubx64.efi
    mcopy -o -i "$out/disk.img@@1M" "$STANDIN_SIGNED" ::/EFI/BOOT/BOOTX64.EFI
    # shellcheck disable=SC2024
    sudo "$MKSTICK" update "$out/disk.img" --key "$MOK_KEY" --cert "$MOK_CRT" \
        --backup-dir "$out" --yes >"$out/update.log" 2>&1 ||
        WHY="update failed: $(tail -1 "$out/update.log")"
    [[ -z "$WHY" ]] && { grep -q 'write: partition 1 .EFI.falcon.falconx64.efi in place of grubx64.efi' "$out/update.log" ||
        WHY="update did not plan the rename"; }
    [[ -z "$WHY" ]] && { grep -q "BOOTX64.EFI was another system's; saved to $out/disk.img-bootx64-" "$out/update.log" ||
        WHY="update did not say where it saved BOOTX64.EFI"; }
    [[ -z "$WHY" ]] && { grep -q 'entry: falcon naming .EFI.BOOT.BOOTX64.EFI, first in BootOrder' "$out/update.log" ||
        WHY="update did not plan the entry"; }
    [[ -z "$WHY" ]] && { cmp -s "$out"/disk.img-bootx64-*.efi "$STANDIN_SIGNED" ||
        WHY="the saved BOOTX64.EFI is not the other system's"; }
    [[ -z "$WHY" ]] && mdir -b -i "$out/disk.img@@1M" ::/EFI/falcon 2>/dev/null |
        grep -qi 'grubx64' && WHY="grubx64.efi is still on the ESP"
    if [[ -z "$WHY" ]]; then
        mcopy -n -i "$out/disk.img@@1M" ::/EFI/BOOT/BOOTX64.EFI "$out/jump.efi"
        sbverify --cert "$MOK_CRT" "$out/jump.efi" >/dev/null 2>&1 ||
            WHY="BOOTX64.EFI after the update is not the signed jumper"
    fi
    if [[ -z "$WHY" ]]; then
        ENTRY='\EFI\BOOT\BOOTX64.EFI' boot "$fw" "$out/disk.img" sata \
            "$(cat "$R/noshim.esp")" 0
        wait_for "Booting in 5 s" 90 || WHY="no countdown after the migration"
        [[ -z "$WHY" ]] && { wait_for "booting: personal.efi" 15 ||
            WHY="the default was not the shelf's UKI"; }
        [[ -z "$WHY" ]] && unlock "$id" 30
        halt
    fi
    rm -rf "$out"
    result "$c" "$WHY" "update migrated grubx64.efi to falconx64.efi behind the jumper, saved the other BOOTX64.EFI; the entry booted it to the UKI"
}

case_4kn_remove() {
    local c=$1 out
    WHY=
    : > "$R/serial.log"
    cp --sparse=always "$R/4kn.img" "$R/remove.img"
    attach "$R/remove.img" --sector-size 4096
    # shellcheck disable=SC2024
    if ! sudo "$MKSTICK" station status "$LOOP" >"$R/4kn.status" 2>&1; then
        WHY="station status failed"
    fi
    if [[ -z "$WHY" ]]; then
        out=$(grep -c '^  OK ' "$R/4kn.status" || true)
        grep -q 'sector(s)' "$R/4kn.status" || WHY="status did not read stage 1"
    fi
    if [[ -z "$WHY" ]]; then
        # shellcheck disable=SC2024
        sudo "$MKSTICK" station remove "$LOOP" --backup "$R/4kn.bak" --full \
            >"$R/4kn.remove" 2>&1 || WHY="station remove --full failed"
    fi
    detach
    # The ESP keeps the deleted directory entries mdeltree leaves, and the
    # shelf its stamp (1 MiB to 365 MiB); every other byte is the original.
    if [[ -z "$WHY" ]] &&
       ! { cmp -s <(head -c 1048576 "$R/remove.img") \
                  <(head -c 1048576 "$R/4kn.orig.img") &&
           cmp -s <(tail -c +$(( 365 * 1048576 + 1 )) "$R/remove.img") \
                  <(tail -c +$(( 365 * 1048576 + 1 )) "$R/4kn.orig.img"); }; then
        WHY="the disk after remove --full differs from before the install"
    fi
    rm -f "$R/remove.img"
    result "$c" "$WHY" "status: ${out:-0} checks OK; remove --full gave back the original bytes outside the ESP and shelf"
}

# ---- run ----

build_all
for n in 4kn mbr4k; do
    [[ -f "$R/$n.id" && "${KEEP:-0}" == 1 ]] && continue
    attach "$R/$n.img" --sector-size 4096
    install_id "$LOOP" > "$R/$n.id"
    detach
done

CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(uefi-default uefi-sb-default uefi-pos uefi-sb-pos 4kn-uefi
           4kn-uefi-sb 4kn-bios 4kn-remove mbr4k-bios second-nxt
           second-sb-nxt second-esp second-sb-esp mbr-uefi mbr-uefi-sb
           noshim-uefi noshim-uefi-sb noshim-jump-uefi noshim-jump-uefi-sb
           noshim-status noshim-update noshim-migrate foreign-jump
           mbr4k-bios-bad)

for c in "${CASES[@]}"; do
    case $c in
    uefi-default)    case_uefi_default "$c" uefi ;;
    uefi-sb-default) case_uefi_default "$c" uefi-sb ;;
    uefi-pos)        case_uefi_pos "$c" uefi ;;
    uefi-sb-pos)     case_uefi_pos "$c" uefi-sb ;;
    4kn-uefi)        case_4kn_uefi "$c" uefi ;;
    4kn-uefi-sb)     case_4kn_uefi "$c" uefi-sb ;;
    4kn-bios)        case_bios "$c" "$R/4kn.img" 4 ;;
    4kn-remove)      case_4kn_remove "$c" ;;
    mbr4k-bios)      case_bios "$c" "$R/mbr4k.img" 3 ;;
    mbr4k-bios-bad)  case_bios_bad "$c" "$R/mbr4k.img" ;;
    second-nxt)      case_second "$c" uefi nxt ;;
    second-sb-nxt)   case_second "$c" uefi-sb nxt ;;
    second-esp)      case_second "$c" uefi esp ;;
    second-sb-esp)   case_second "$c" uefi-sb esp ;;
    mbr-uefi)        case_mbr_uefi "$c" uefi ;;
    mbr-uefi-sb)     case_mbr_uefi "$c" uefi-sb ;;
    noshim-uefi)     case_noshim "$c" uefi ;;
    noshim-uefi-sb)  case_noshim "$c" uefi-sb ;;
    noshim-jump-uefi)    case_noshim_jump "$c" uefi ;;
    noshim-jump-uefi-sb) case_noshim_jump "$c" uefi-sb ;;
    noshim-status)   case_noshim_status "$c" ;;
    noshim-update)   case_noshim_update "$c" ;;
    noshim-migrate)  case_noshim_migrate "$c" uefi-sb ;;
    foreign-jump)    case_foreign_jump "$c" ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
done
exit "$FAILED"
