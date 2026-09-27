#!/usr/bin/env bash
# falcon-boot: stations made by the installer, booted (docs/DESIGN.md
# Sections 13.9, 13.11; docs/notes/station-rework.md section 6). Every disk
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
#         and the MOK pair, and free space. `station install --uefi`; the
#         varstore has falcon's entry and Windows's, as the machine would.
#   4kn   GPT on a 4096-byte-sector disk (a loop device with 4096-byte
#         sectors, booted as NVMe with 4096-byte logical blocks, the one QEMU
#         disk SeaBIOS offers at that size; DESIGN Section 3.4): an ESP of
#         300 MiB, a LUKS2 shelf, free space. `station install`, both
#         stations: the station partition (288 MiB, FAT32 of 4096-byte
#         sectors), the FALCON-BIOS partition at LBA 34, everything counted
#         in 4096-byte sectors.
#   mbr4k MBR on a 4096-byte-sector disk: a Windows partition, a LUKS2
#         shelf, free space. `station install --bios`.
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
    install_station gpt "$img" --uefi --shelf 3 --memtest-efi "$STANDIN"
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
    install_station 4kn "$LOOP" --shelf 2 --memtest-efi "$STANDIN"
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

build_all() {
    make --no-print-directory station cli "$STANDIN" "$STANDIN_SIGNED" \
        >/dev/null
    local f
    for f in "$FIXTURE_UKI_SIGNED" "$MOK_KEY" "$MOK_CRT" \
             build/shim/bins/fedora-16.1-shimx64.efi; do
        [[ -f "$f" ]] || { echo "ERROR: $f missing (make test-uki sign)" >&2; exit 1; }
    done
    if [[ "${KEEP:-0}" == 1 && -f "$R/gpt.img" && -f "$R/4kn.img" &&
          -f "$R/mbr4k.img" ]]; then
        return
    fi
    echo "== installing the rig's stations"
    build_gpt
    build_4kn
    build_mbr4k
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

# vars JSON: falcon's Boot0001 on the station disk's ESP, and with WIN a
# "Windows Boot Manager" Boot0009 naming bootmgfw.efi on the same ESP.
# The device path is the firmware's whole path to the disk: OVMF drops a
# boot option whose path matches no device QEMU's bootorder lists.
vars_json() {   # vars_json sata|nvme "START SIZE GUID" WIN
    python3 - "$1" "$2" "$3" "$FALCON_SHIM" "$BOOTMGFW" "$GLOBAL_GUID" \
        "$NVME_SLOT" <<'EOF'
import json, struct, sys, uuid
bus, facts, win, shim, bootmgfw, gguid, slot = sys.argv[1:8]
start, size, guid = facts.split()
if bus == "sata":
    # PciRoot(0)/Pci(0x1f,0x2)/Sata(0,0xFFFF,0): q35's AHCI, port 0.
    pre = struct.pack("<BBHII", 2, 1, 12, 0x0A0341D0, 0)
    pre += struct.pack("<BBHBB", 1, 1, 6, 2, 0x1F)
    pre += struct.pack("<BBHHHH", 3, 0x12, 10, 0, 0xFFFF, 0)
else:
    # PciRoot(0)/Pci(slot,0)/NVMe(0x1,00-00-00-00-00-00-00-00).
    pre = struct.pack("<BBHII", 2, 1, 12, 0x0A0341D0, 0)
    pre += struct.pack("<BBHBB", 1, 1, 6, 0, int(slot))
    pre += struct.pack("<BBHI", 3, 0x17, 16, 1) + bytes(8)
hd = struct.pack("<BBHIQQ", 4, 1, 42, 1, int(start), int(size))
hd += uuid.UUID(guid).bytes_le + bytes([2, 2])
end = bytes([0x7F, 0xFF, 4, 0])
def option(desc, path):
    name = (path + "\0").encode("utf-16-le")
    fp = struct.pack("<BBH", 4, 4, 4 + len(name)) + name
    dp = pre + hd + fp + end
    d = (desc + "\0").encode("utf-16-le")
    return (struct.pack("<IH", 1, len(dp)) + d + dp).hex()
v = [{"name": "Boot0001", "guid": gguid, "attr": 7,
      "data": option("falcon", shim)}]
order = [1]
if win == "1":
    v.append({"name": "Boot0009", "guid": gguid, "attr": 7,
              "data": option("Windows Boot Manager", bootmgfw)})
    order.append(9)
v.append({"name": "BootOrder", "guid": gguid, "attr": 7,
          "data": b"".join(struct.pack("<H", n) for n in order).hex()})
print(json.dumps({"version": 2, "variables": v}))
EOF
}

# boot FIRMWARE IMG BUS FACTS WIN: one disk, the station's. OVMF under
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
    local fw=$1 img=$2 bus=$3 facts=$4 win=$5 code vars dev fwargs=()
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
        vars_json "$bus" "$facts" "$win" > "$R/vars.json"
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
    rm -f "$R/serial.log" "$R/mon.sock"
    timeout "$TIMEOUT" qemu-system-x86_64 "${fwargs[@]}" -m 1024 \
        -enable-kvm -cpu host -display none -net none \
        -drive "if=none,id=d0,format=raw,file=$R/boot.img" -device "$dev" \
        -serial "file:$R/serial.log" \
        -monitor "unix:$R/mon.sock,server,nowait" >"$R/qemu.log" 2>&1 &
    QPID=$!
}

halt() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
    rm -f "$R/boot.img"
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
    wait_for "Booting in 5 s. Any key stops the countdown." 90 ||
        WHY="no countdown"
    [[ -z "$WHY" ]] && { has "Personal OS (encrypted)" || WHY="no Personal OS row"; }
    # The INI's [windows] names the disk, matches the firmware entry, and
    # labels its row.
    [[ -z "$WHY" ]] && { has "Windows (disk gpt:" || WHY="no Windows row"; }
    [[ -z "$WHY" ]] && has "Windows Boot Manager" &&
        WHY="the firmware entry is a row of its own beside the INI's"
    [[ -z "$WHY" ]] && { wait_for "Memory test (memtest86+)" 5 || WHY="no memtest row"; }
    [[ -z "$WHY" ]] && { wait_for "booting: Windows (disk gpt:" 15 ||
        WHY="the default was not Windows"; }
    [[ -z "$WHY" ]] && { has "restarting into it" || WHY="no NXT page"; }
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
    wait_for "Booting in 5 s. Any key stops the countdown." 90 ||
        WHY="no countdown"
    [[ -z "$WHY" ]] && { has "personal.efi" || WHY="no row for the INI's [linux]"; }
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
           4kn-uefi-sb 4kn-bios 4kn-remove mbr4k-bios)

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
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
done
exit "$FAILED"
