#!/usr/bin/env bash
# falcon-boot: the core step's matrix cells (docs/notes/station-rework.md
# section 6), each booted headless and judged from serial like
# falcon-bios/tools/run-jump-rig.sh. UEFI is OVMF (ovmf.sh finds it), BIOS
# is QEMU's SeaBIOS.
#
#   uefi-stick     the rig stick (mk-test-stick.sh) as the only disk under
#                  OVMF, Secure Boot off: the menu lists its library, the
#                  personal OS unlocks, and the fixture UKI reports the
#                  passphrase credential and falcon.ident.cred carrying the
#                  stick's install ID
#   uefi-stick-sb  the same under Secure Boot, the test MOK enrolled
#   bios-stick     the same image under SeaBIOS: stage 2 reports the boot
#                  partition, stage 3 the install, the library and the
#                  shelf, and the personal OS unlocks as above
#   two-sticks-uefi, two-sticks-bios
#                  a second stick of another install ID, another
#                  passphrase and other ISO names attached before the booted
#                  one: only the booted stick's library is listed, and its
#                  shelf is the one that unlocks
#   mbr-disk-uefi, mbr-disk-bios
#                  an MBR data disk beside the stick (FAT32 and exFAT
#                  primaries, a FAT16 logical partition, each holding an
#                  .iso, no records): nothing from it is listed
#   foreign-uefi, foreign-bios
#                  a GPT disk carrying another install's records (a boot
#                  record, a library with foreign.iso, a shelf with the
#                  same passphrase and two UKIs of its own): ignored; the
#                  chooser offers the stick's UKIs, not its
#   4kn-uefi       a stick `mkstick create` wrote onto a 4096-byte-sector
#                  loop device (and `mkstick bios` put the stages on),
#                  booted from an NVMe disk with 4096-byte logical blocks:
#                  the menu lists its library and the personal OS unlocks
#   4kn-bios       the same stick under SeaBIOS, whose NVMe driver is the
#                  one that offers a 4096-byte-sector disk through INT 13h
#                  (DESIGN Section 3.4)
#   4kn-bios-marker
#                  the same, with `mkstick next-boot --sector :personal`
#                  written to the stick first: the loader takes the whole
#                  4096-byte marker sector, clears it and goes straight to
#                  the personal OS without the menu, and `next-boot --print`
#                  afterwards finds nothing queued
#   station-uefi, station-bios
#                  the station disk (mk-test-station.sh): reaches its
#                  passphrase prompt, from the shelf of its install, through
#                  the Personal OS row of the menu either opens at
#                  (run-station-rig.sh under each firmware has the rest)
#   station-stick-uefi, station-stick-bios
#                  the station disk with a STATION.INI (default the
#                  shelf's personal.efi, a [linux] section), booted twice:
#                  beside a blank disk, then beside stick-a (same
#                  passphrase, its own library and UKIs). The first
#                  screen's rows are the same both times, the countdown
#                  boots the same default, one prompt unlocks the
#                  station's shelf (no chooser: the stick's shelf has
#                  more UKIs than the station's one), and nothing of the
#                  stick's library is listed
#   bios-only-uefi, bios-only-uefi-sb
#                  the rig stick under OVMF, Secure Boot off and on: a
#                  BIOS-only ISO on its library (isolinux, a kernel with no
#                  EFI stub) is listed with {BIOS ONLY}, one whose kernel has
#                  a stub is not, and choosing the first shows its page and
#                  returns to the menu
#   efi-only-bios  the rig stick under SeaBIOS: an ISO with only a UEFI El
#                  Torito image and no kernel is listed with {EFI ONLY}, the
#                  BIOS-only one and the 64-bit-only one are not (this
#                  processor has long mode), and choosing the first shows its
#                  page and returns to the menu
#   64-only-bios32 the same stick under SeaBIOS on a Pentium III (no long
#                  mode): an ISO whose every kernel is 64-bit is listed with
#                  {64-BIT ONLY}, a multi-arch one (a 64-bit and a 32-bit
#                  kernel) is not, and choosing the first shows its page and
#                  returns to the menu
#
# Every image is built from image files; loop devices on them need sudo, as
# mk-test-stick.sh does. Nothing touches a real disk.
#
# Usage: falcon-efi/tools/run-core-rig.sh [case...]   (default: all)
# Env:   FALCON_TEST_PASS  the sticks' passphrase (default falcon-test)
#        KEEP=1            reuse the images from an earlier run
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. falcon-efi/tools/ovmf.sh

R=build/core-rig
PASS="${FALCON_TEST_PASS:-falcon-test}"
PASS_B=falcon-other
MKSTICK=build/bin/falcon-boot-mkstick
TIMEOUT=180

# Fixed install IDs, so a log names the same install every run.
ID_A=c0de0000000000000000000000000a0a
ID_B=c0de0000000000000000000000000b0b
ID_F=c0de0000000000000000000000000f0f

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

# ---- images ----

mount_part() {  # mount_part DEV [ext4]: mounts at $MNT, owned by us
    MNT=$(mktemp -d)
    if [[ "${2:-}" == ext4 ]]; then
        sudo mount "$1" "$MNT"
        sudo chown "$(id -u):$(id -g)" "$MNT"
    else
        sudo mount -o uid="$(id -u)",gid="$(id -g)" "$1" "$MNT"
    fi
}

umount_part() {
    sync
    sudo umount "$MNT"
    rmdir "$MNT"
    MNT=
}

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

# The BIOS-only case's two media, named to sort first on the root screen,
# right under the Personal OS row.
BIOS_ONLY_ISO=a-bios-only.iso
STUB_ISO=a-efi-stub.iso
# The BIOS tag cases' three (media/biosboot.h), named to sort after them so
# the BIOS-only case's rows keep their places.
EFI_ONLY_ISO=b-efi-only.iso
X64_ISO=b-64-only.iso
MULTI_ISO=b-multi.iso

build_sticks() {
    STICK_ID=$ID_A TEST_STICK_IMG=$R/stick-a.img \
        STICK_ISO_FILES="build/test-uefi-biosonly.iso:$BIOS_ONLY_ISO build/test-uefi-stub.iso:$STUB_ISO build/test-bios-efionly.iso:$EFI_ONLY_ISO build/test-bios-64.iso:$X64_ISO build/test-bios-multi.iso:$MULTI_ISO" \
        falcon-efi/tools/mk-test-stick.sh --no-labels >"$R/stick-a.build" 2>&1
    STICK_ID=$ID_B TEST_STICK_IMG=$R/stick-b.img FALCON_TEST_PASS=$PASS_B \
        STICK_ISOS="second-stick.iso" \
        falcon-efi/tools/mk-test-stick.sh --no-labels >"$R/stick-b.build" 2>&1
}

# MBR: p1 FAT32 (0x0C), p2 exFAT (0x07), p3 extended holding p5 FAT16
# (0x06). Each carries an .iso a loader would list if it read the volume,
# and none carries a record.
build_mbr_disk() {
    local img=$R/mbr-disk.img

    rm -f "$img"
    truncate -s 160M "$img"
    # sfdisk numbers an unnamed line after the extended one as a fifth
    # primary; the names make p5 the logical partition.
    sfdisk -q "$img" <<EOF
label: dos
label-id: 0x5eed0001
${img}1 : start=2048, size=64M, type=c
${img}2 : size=48M, type=7
${img}3 : size=40M, type=5
${img}5 : size=32M, type=6
EOF
    attach "$img"
    sudo mkfs.fat -F 32 -n MBRFAT "${LOOP}p1" >/dev/null
    sudo mkfs.exfat -n MBREXFAT "${LOOP}p2" >/dev/null
    sudo mkfs.fat -F 16 -n MBRLOG "${LOOP}p5" >/dev/null
    local n
    for n in 1:mbr-fat.iso 2:mbr-exfat.iso 5:mbr-logical.iso; do
        mount_part "${LOOP}p${n%%:*}"
        dd if=/dev/zero of="$MNT/${n#*:}" bs=1M count=1 status=none
        umount_part
    done
    detach
}

# GPT, another install's records on every partition: p1 an ESP-typed FAT32
# with a boot record, p2 a library with foreign.iso, p3 a LUKS2 shelf with
# the rig's passphrase and two UKIs of its own.
build_foreign_disk() {
    local img=$R/foreign.img

    rm -f "$img"
    truncate -s 200M "$img"
    sgdisk -n 1:1MiB:+40MiB -t 1:ef00 -c 1:OTHER-ESP \
        -n 2:0:+48MiB -t 2:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 -c 2:FALCON-ISO \
        -n 3:0:+64MiB -t 3:9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF -c 3:FALCON-UKI \
        "$img" >/dev/null
    attach "$img"
    sudo mkfs.fat -F 32 -R 64 -n OTHERESP "${LOOP}p1" >/dev/null
    sudo mkfs.exfat -n FALCON-ISO "${LOOP}p2" >/dev/null
    mount_part "${LOOP}p2"
    dd if=/dev/zero of="$MNT/foreign.iso" bs=1M count=1 status=none
    printf 'falcon 1 library %s\n' "$ID_F" > "$MNT/.falcon"
    umount_part
    printf '%s' "$PASS" | sudo cryptsetup luksFormat --type luks2 \
        --pbkdf pbkdf2 --pbkdf-force-iterations 100000 --batch-mode \
        "${LOOP}p3" -
    printf '{"type":"falcon","keyslots":[],"version":1,"role":"shelf","install_id":"%s"}' \
        "$ID_F" | sudo cryptsetup token import "${LOOP}p3"
    MAPPER=falcon-core-rig-$$
    printf '%s' "$PASS" | sudo cryptsetup open "${LOOP}p3" "$MAPPER" -
    sudo mkfs.ext4 -q "/dev/mapper/$MAPPER"
    mount_part "/dev/mapper/$MAPPER" ext4
    mkdir "$MNT/uki"
    cp build/test/fixture-uki.efi "$MNT/uki/foreign-a.efi"
    cp build/test/fixture-uki.efi "$MNT/uki/foreign-b.efi"
    umount_part
    sudo cryptsetup close "$MAPPER"
    MAPPER=
    detach
    "$MKSTICK" --quiet ident "$img" --write-boot 1 --id "$ID_F" >/dev/null
}

# `mkstick create` onto a loop device with 4096-byte logical sectors, the
# only way to make a 4Kn disk from a file; then an .iso on its library.
build_4kn() {
    local img=$R/stick-4kn.img p2

    rm -f "$img"
    truncate -s 1G "$img"
    ( umask 077; printf '%s' "$PASS" > "$R/pass" )
    attach "$img" --sector-size 4096
    # shellcheck disable=SC2024  # the log is ours, not root's
    sudo "$MKSTICK" --quiet create "$LOOP" --force \
        --p1-size 300M --iso-size 128M --p3-size 128M --p4-size rest \
        --passphrase-file "$R/pass" --shim build/shim/bins \
        --loader build/bin/falcon-boot.efi --uki build/test/fixture-uki.efi \
        --cn "falcon-boot 4Kn rig" >"$R/stick-4kn.build" 2>&1
    rm -f "$R/pass"
    p2=$(sudo "$MKSTICK" ident "$LOOP" | awk -F '\t' '$2 == "library" { print $1 }')
    [[ -n "$p2" ]] || { echo "ERROR: the 4Kn stick has no library" >&2; exit 1; }
    mount_part "${LOOP}p$p2"
    dd if=/dev/zero of="$MNT/fourk.iso" bs=1M count=1 status=none
    umount_part
    # The BIOS stages, counted in 4096-byte sectors like everything else
    # on this disk (DESIGN Section 3.3).
    # shellcheck disable=SC2024
    sudo "$MKSTICK" --quiet bios --reserve --no-backup \
        --stage1 build/bin/falcon-bios-stage1.bin \
        --stage2 build/bin/falcon-bios-stage2.bin \
        --stage3 build/bin/falcon-bios-stage3.bin "$LOOP" >>"$R/stick-4kn.build" 2>&1
    ID_4K=$(sudo "$MKSTICK" ident "$LOOP" | awk -F '\t' '$2 == "boot" { print $3 }')
    detach
    echo "$ID_4K" > "$R/stick-4kn.id"
}

build_all() {
    make --no-print-directory all bios station cli payload test-iso \
        test-uefiboot test-biosboot >/dev/null
    # The fixture UKI and the test MOK are built once (make test-uki,
    # make sign) and reused; this rig never rebuilds them.
    for f in build/test/fixture-uki.efi build/mok-test/falcon-test-mok.key \
             build/shim/bins/fedora-16.1-shimx64.efi; do
        [[ -f "$f" ]] || { echo "ERROR: $f missing (make test-uki sign)" >&2; exit 1; }
    done
    make --no-print-directory sign >/dev/null
    if [[ "${KEEP:-0}" == 1 && -f "$R/stick-a.img" ]]; then
        return
    fi
    echo "== building the rig's disks"
    build_sticks
    build_mbr_disk
    build_foreign_disk
    build_4kn
    make --no-print-directory test-station >"$R/station.build" 2>&1
}

# ---- the machine ----

log_text() {    # log_text LOG: serial with CRs and ANSI sequences gone
    tr -d '\r' 2>/dev/null < "$1" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}

mon() { printf '%s\n' "$*" | socat - "UNIX-CONNECT:$R/mon.sock" >/dev/null 2>&1 || true; }

wait_for() {    # wait_for PATTERN SECONDS: 0 once the log has it
    local i
    for (( i = 0; i < $2 * 10; i++ )); do
        log_text "$R/serial.log" | grep -aqF -- "$1" && return 0
        kill -0 "$QPID" 2>/dev/null || { log_text "$R/serial.log" |
            grep -aqF -- "$1"; return; }
        sleep 0.1
    done
    return 1
}

send_keys() {   # send_keys TEXT: one sendkey per character, then Enter
    local s=$1 i c
    for (( i = 0; i < ${#s}; i++ )); do
        c=${s:i:1}
        case $c in
        [a-z0-9]) mon "sendkey $c" ;;
        -)        mon "sendkey minus" ;;
        .)        mon "sendkey dot" ;;
        *) echo "ERROR: no sendkey for '$c'" >&2; exit 2 ;;
        esac
        sleep 0.05
    done
    mon "sendkey ret"
}

# boot FIRMWARE DISK... : a disk is IMG[:boot][:4k]; the one marked boot is
# given bootindex 0, the others 1, 2, ... in the order given. OVMF connects
# only the disks QEMU's boot order lists, so a disk with no bootindex is
# one the UEFI loader never sees.
boot() {
    local fw=$1 d img opts n=0 idx=1 args=() fwargs=() vars code cpu=host
    shift
    for d in "$@"; do
        img=${d%%:*}
        opts=":${d#*:}:"
        [[ "$d" == *:* ]] || opts=::
        args+=(-drive "if=none,id=d$n,format=raw,file=$img")
        local dev
        if [[ "$opts" == *:4k:* ]]; then
            # NVMe: the one QEMU disk with 4096-byte logical blocks that
            # SeaBIOS offers through INT 13h (virtio-blk, SCSI, USB and
            # IDE do not; DESIGN Section 3.4), and OVMF reads it too.
            dev="nvme,drive=d$n,serial=falcon4k$n,logical_block_size=4096,physical_block_size=4096"
        elif [[ "$fw" == bios || "$fw" == bios32 ]]; then
            dev="ide-hd,drive=d$n,bus=ide.$(( n / 2 )),unit=$(( n % 2 ))"
        else
            dev="ide-hd,drive=d$n,bus=ide.$n"
        fi
        if [[ "$opts" == *:boot:* ]]; then
            dev+=",bootindex=0"
        else
            dev+=",bootindex=$idx"
            idx=$(( idx + 1 ))
        fi
        args+=(-device "$dev")
        n=$(( n + 1 ))
    done
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
        fwargs=(-machine q35
                -drive "if=pflash,format=raw,readonly=on,file=$code"
                -drive "if=pflash,format=raw,file=$R/vars.fd")
        ;;
    bios) fwargs=(-machine pc) ;;
    # The coverage harness's bios32 column: a Pentium III, no long mode.
    bios32) fwargs=(-machine pc); cpu=pentium3 ;;
    esac
    rm -f "$R/serial.log" "$R/mon.sock"
    timeout "$TIMEOUT" qemu-system-x86_64 "${fwargs[@]}" -m 1024 \
        -enable-kvm -cpu "$cpu" -display none -net none "${args[@]}" \
        -serial "file:$R/serial.log" \
        -monitor "unix:$R/mon.sock,server,nowait" >"$R/qemu.log" 2>&1 &
    QPID=$!
}

halt() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
}

# The personal OS, from the menu: the passphrase, the chooser's row for
# personal.efi when there is a chooser, then the fixture UKI's report.
# Sets POS_WHY on a failure.
pos() {
    POS_WHY=
    wait_for "Personal OS (encrypted)" 90 || { POS_WHY="no Personal OS row"; return 1; }
    sleep 1
    mon "sendkey ret"
    pos_unlock 30
}

# From the passphrase prompt on: the passphrase, the chooser's row, the
# UKI's report. pos_unlock SECONDS waits that long for the prompt.
pos_unlock() {
    local rows idx i
    POS_WHY=
    wait_for "passphrase for the encrypted partition" "$1" ||
        { POS_WHY="no passphrase prompt"; return 1; }
    sleep 0.5
    send_keys "$PASS"
    for (( i = 0; i < 300; i++ )); do
        log_text "$R/serial.log" | grep -aq 'personal\.efi\|FALCON-UKI-INIT' && break
        sleep 0.1
    done
    sleep 1
    if ! log_text "$R/serial.log" | grep -aq FALCON-UKI-INIT; then
        rows=$(log_text "$R/serial.log" | grep -aoE '[A-Za-z ]+\([a-z0-9-]+\.efi\)' |
               sed 's/^ *//' | awk '!seen[$0]++')
        idx=$(printf '%s\n' "$rows" | grep -n 'personal\.efi' | head -1 | cut -d: -f1)
        [[ -n "$idx" ]] || { POS_WHY="the chooser has no personal.efi"; return 1; }
        for (( i = 1; i < idx; i++ )); do
            mon "sendkey down"
            sleep 0.1
        done
        mon "sendkey ret"
    fi
    wait_for "FALCON-UKI-DONE" 120 || { POS_WHY="the UKI never reported"; return 1; }
    return 0
}

has() { log_text "$R/serial.log" | grep -aqF -- "$1"; }

FAILED=0
result() {      # result CASE OK DETAIL
    if [[ $2 -eq 1 ]]; then
        printf 'PASS  %-15s %s\n' "$1" "$3"
    else
        printf 'FAIL  %-15s %s\n' "$1" "$3"
        FAILED=1
    fi
    log_text "$R/serial.log" > "$R/$1.log" 2>/dev/null || true
}

# The judgements every stick case shares: the library listed, the personal
# OS through to both credentials, the ident credential naming this install,
# nothing named in ABSENT anywhere in the log. SEEN, "|"-separated, is what
# falcon-bios's serial must also say (that the other disk was read); the
# UEFI loader says nothing of the disks it passes over.
stick_case() {  # stick_case CASE FIRMWARE ID ISO ABSENT SEEN DISK...
    local c=$1 fw=$2 id=$3 iso=$4 absent=$5 seen=$6 why=
    shift 6
    boot "$fw" "$@"
    wait_for "$iso" 90 || why="the menu never listed $iso"
    if [[ -z "$why" ]] && ! pos; then why=$POS_WHY; fi
    if [[ -z "$why" ]]; then
        has "FALCON-CRED-FILE cryptsetup.passphrase.cred" ||
            why="no passphrase credential"
        has "FALCON-CRED-OK" || why="${why:-the passphrase credential is wrong}"
        has "FALCON-CRED-HEAD falcon.ident.cred [falcon 1 boot $id" ||
            why="${why:-falcon.ident.cred missing or not install $id}"
    fi
    if [[ -z "$why" && -n "$absent" ]]; then
        local a
        for a in $absent; do
            has "$a" && why="${why:-$a was taken from another disk}"
        done
    fi
    if [[ -z "$why" && "$fw" == bios ]]; then
        has "boot partition p1 at LBA" || why="stage 2 did not report the boot partition"
        has "install $id" || why="${why:-stage 3 did not report install $id}"
        local says one
        IFS='|' read -r -a says <<< "$seen"
        for one in "${says[@]}"; do
            has "$one" || why="${why:-serial never said \"$one\"}"
        done
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$c" 1 "listed $iso; UKI got both credentials, install ${id:0:8}..."
    else
        result "$c" 0 "$why"
    fi
}

# The 4Kn stick's marker, set and printed through a 4096-byte-sector loop
# device, then taken by falcon-bios off the NVMe disk.
case_4kn_marker() { # case_4kn_marker CASE
    local why= out
    attach "$R/stick-4kn.img" --sector-size 4096
    sudo "$MKSTICK" --quiet next-boot --sector --disk "$LOOP" :personal
    out=$(sudo "$MKSTICK" next-boot --print --sector --disk "$LOOP")
    detach
    grep -qx 'sector_size=4096' <<< "$out" || why="--print did not read a 4096-byte sector"
    grep -qx 'value=:personal' <<< "$out" || why="${why:---print does not show :personal queued}"
    grep -qx 'window=yes' <<< "$out" || why="${why:---print says the loader would not honour it}"
    if [[ -n "$why" ]]; then
        result "$1" 0 "$why"
        return
    fi
    boot bios "$R/stick-4kn.img:boot:4k"
    wait_for "next-boot marker: cleared; names :personal" 90 ||
        why="falcon-bios did not take the marker"
    if [[ -z "$why" ]] && ! pos_unlock 30; then why=$POS_WHY; fi
    if [[ -z "$why" ]]; then
        has "enter boot | c chain" && why="the menu was drawn"
    fi
    halt
    if [[ -z "$why" ]]; then
        attach "$R/stick-4kn.img" --sector-size 4096
        out=$(sudo "$MKSTICK" next-boot --print --sector --disk "$LOOP")
        detach
        grep -qx 'queued=no' <<< "$out" || why="the marker sector was not cleared"
    fi
    if [[ -z "$why" ]]; then
        result "$1" 1 "4096-byte marker taken and cleared; the personal OS booted from it"
    else
        result "$1" 0 "$why"
    fi
}

case_station() { # case_station CASE FIRMWARE
    local id why=
    id=$("$MKSTICK" ident build/test-station.img |
         awk -F '\t' '$2 == "boot" { print $3; exit }')
    boot "$2" build/test-station.img:boot
    # Either station opens at its menu (DESIGN Section 13.3), the shelf's
    # row first; Enter on it is the prompt.
    wait_for "Personal OS (encrypted)" 90 || why="no Personal OS row"
    sleep 1
    [[ -z "$why" ]] && mon "sendkey ret"
    [[ -n "$why" ]] || wait_for "passphrase for the encrypted partition" 90 ||
        why="no passphrase prompt"
    if [[ -z "$why" && "$2" == bios ]]; then
        has "install $id" || why="stage 3 did not report install $id"
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "the prompt for the shelf of install ${id:0:8}..."
    else
        result "$1" 0 "$why"
    fi
}

# The first list's rows, as the serial carries them between the key hint
# and the countdown's second line, less its first: the same text for the
# same rows. (UEFI draws the countdown before the rows, BIOS after.)
first_rows() {
    log_text "$R/serial.log" | tr -s ' \n' ' ' |
        sed -n 's/.*| esc back\(.*\)Booting in 4 s.*/\1/p' |
        sed 's/Booting in [0-9] s.*//; s/Booting in 5 s\. Any key stops the countdown\.//' |
        tr -s ' '
}

# The station disk with a STATION.INI on the ESP and in the station
# partition: the shelf's UKI the default, a long enough count.
station_ini_disk() {
    local img=$R/station-ini.img start ini=$R/STATION.INI
    cp --sparse=always build/test-station.img "$img"
    printf '[general]\ndefault = linux\ntimeout = 5\n\n[linux]\nuki = personal.efi\n' \
        > "$ini"
    mcopy -o -i "$img@@1M" "$ini" ::/EFI/falcon/STATION.INI
    start=$(sgdisk -p "$img" | awk '$7 == "FALCON-STATION" { print $2 }')
    [[ -n "$start" ]] || { echo "ERROR: test-station.img has no FALCON-STATION" >&2; exit 1; }
    mcopy -o -i "$img@@$(( start * 512 ))" "$ini" ::/STATION.INI
    rm -f "$R/blank.img"
    truncate -s 256M "$R/blank.img"
}

case_station_stick() { # case_station_stick CASE FIRMWARE
    local c=$1 fw=$2 id why= alone with prompts
    id=$("$MKSTICK" ident build/test-station.img |
         awk -F '\t' '$2 == "boot" { print $3; exit }')
    station_ini_disk
    boot "$fw" "$R/station-ini.img:boot" "$R/blank.img"
    wait_for "Booting in 4 s" 90 || why="no countdown beside a blank disk"
    alone=$(first_rows)
    log_text "$R/serial.log" > "$R/$c-alone.log"
    halt
    [[ -z "$why" && -z "$alone" ]] && why="no rows read beside a blank disk"
    if [[ -z "$why" ]]; then
        boot "$fw" "$R/station-ini.img:boot" "$R/stick-a.img"
        wait_for "Booting in 4 s" 90 || why="no countdown beside the stick"
        with=$(first_rows)
        [[ -z "$why" && "$with" != "$alone" ]] &&
            why="the rows differ: alone [$alone], with the stick [$with]"
        [[ -z "$why" ]] && { wait_for "booting: personal.efi" 15 ||
            why="the default was not the shelf's personal.efi"; }
        [[ -z "$why" ]] && { wait_for "passphrase for the encrypted partition" 30 ||
            why="no passphrase prompt"; }
        if [[ -z "$why" ]]; then
            sleep 0.5
            send_keys "$PASS"
            wait_for "FALCON-UKI-DONE" 120 || why="the UKI never reported"
        fi
        if [[ -z "$why" ]]; then
            has "FALCON-CRED-OK" || why="the passphrase credential is wrong"
            has "FALCON-CRED-HEAD falcon.ident.cred [falcon 1 boot $id" ||
                why="${why:-falcon.ident.cred missing or not install $id}"
            has "unsigned.efi" && why="${why:-a chooser listed the UKIs on the stick}"
            has "$A_ISOS" && why="${why:-the library on the stick was listed}"
            prompts=$(log_text "$R/serial.log" |
                grep -aoF "passphrase for the encrypted partition" | wc -l)
            (( prompts == 1 )) || why="${why:-$prompts passphrase prompts}"
        fi
        if [[ -z "$why" && "$fw" == bios ]]; then
            has "install $id" || why="stage 3 did not report install $id"
        fi
        halt
    fi
    if [[ -z "$why" ]]; then
        result "$c" 1 "rows and default as without the stick; one prompt, the station's shelf, install ${id:0:8}..."
    else
        result "$c" 0 "$why"
    fi
}

# The row's tag, its page, and the menu back after a key. The rows are
# found by what OVMF's terminal drew: each row's text lands on the serial
# with the tag columns after the label.
case_bios_only() { # case_bios_only CASE FIRMWARE
    local why= n
    boot "$2" "$R/stick-a.img:boot"
    wait_for "$STUB_ISO" 90 || why="the menu never listed $STUB_ISO"
    sleep 1
    if [[ -z "$why" ]]; then
        log_text "$R/serial.log" | grep -aqE "$BIOS_ONLY_ISO +\[no efi\] +\{BIOS ONLY\}" ||
            why="$BIOS_ONLY_ISO is not drawn with {BIOS ONLY}"
    fi
    if [[ -z "$why" ]]; then
        log_text "$R/serial.log" | grep -aqE "$STUB_ISO +\[" ||
            why="$STUB_ISO is drawn with no tag column"
    fi
    if [[ -z "$why" ]]; then
        log_text "$R/serial.log" | grep -aoE "$STUB_ISO +\[[^]]*\]( +\{[A-Z ]+\})?" |
            grep -aqF "{BIOS ONLY}" && why="$STUB_ISO is drawn with {BIOS ONLY}"
    fi
    if [[ -z "$why" ]]; then
        # Personal OS is the first row; the BIOS-only image the second.
        mon "sendkey down"
        sleep 0.3
        mon "sendkey ret"
        wait_for "this image boots only on a BIOS machine" 30 ||
            why="choosing $BIOS_ONLY_ISO did not show its page"
    fi
    if [[ -z "$why" ]]; then
        has "booting: $BIOS_ONLY_ISO" && why="the page started a route"
        wait_for "press any key to return to the menu" 10 ||
            why="${why:-the page has no way back}"
    fi
    if [[ -z "$why" ]]; then
        n=$(log_text "$R/serial.log" | grep -aoF "c chain | d direct" | wc -l)
        mon "sendkey spc"
        local i
        for (( i = 0; i < 100; i++ )); do
            (( $(log_text "$R/serial.log" | grep -aoF "c chain | d direct" |
                 wc -l) > n )) && break
            sleep 0.1
        done
        (( i < 100 )) || why="the menu did not come back"
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "$BIOS_ONLY_ISO tagged and refused with its page; $STUB_ISO untagged"
    else
        result "$1" 0 "$why"
    fi
}

# The serial as text with every run of spaces one space, so a row reads as
# its label, then its tags.
squeezed() { log_text "$R/serial.log" | tr -s ' '; }

# The row a name is on, the Personal OS being row 0: the root screen's
# images in the order the serial first drew them.
row_of() {      # row_of NAME
    local i
    i=$(log_text "$R/serial.log" | grep -aoE '[A-Za-z0-9_.-]+\.iso' |
        awk '!seen[$0]++' | grep -nxF -- "$1" | head -1 | cut -d: -f1)
    [[ -n "$i" ]] && echo "$i"
}

# falcon-bios's tags (DESIGN Section 7.8): TAGGED is drawn with TAG,
# every name in PLAIN without one, and choosing TAGGED shows PAGE and
# returns to the menu. The rows are read off the serial mirror, where each
# row's text lands with its tag column after the label.
case_bios_tags() { # case_bios_tags CASE FIRMWARE TAGGED TAG PAGE PLAIN...
    local c=$1 fw=$2 tagged=$3 tag=$4 page=$5 why= n row one i
    shift 5
    boot "$fw" "$R/stick-a.img:boot"
    wait_for "systemrescue-fake.iso" 90 || why="the menu never listed its last row"
    sleep 1
    if [[ -z "$why" ]]; then
        squeezed | grep -aqF -- "$tagged $tag" ||
            why="$tagged is not drawn with $tag"
    fi
    for one in "$@"; do
        [[ -n "$why" ]] && break
        if squeezed | grep -aoE -- "$one [^ ]*" | grep -aqF "{"; then
            why="$one is drawn with a tag"
        fi
    done
    if [[ -z "$why" ]]; then
        row=$(row_of "$tagged") || true
        [[ -n "$row" ]] || why="no row for $tagged"
    fi
    if [[ -z "$why" ]]; then
        for (( i = 0; i < row; i++ )); do
            mon "sendkey down"
            sleep 0.2
        done
        sleep 0.3
        mon "sendkey ret"
        wait_for "$page" 30 || why="choosing $tagged did not show its page"
    fi
    if [[ -z "$why" ]]; then
        has "booting: $tagged" && why="the page started a route"
        wait_for "press any key to return to the menu" 10 ||
            why="${why:-the page has no way back}"
    fi
    if [[ -z "$why" ]]; then
        n=$(log_text "$R/serial.log" | grep -aoF "c chain | d direct" | wc -l)
        mon "sendkey spc"
        for (( i = 0; i < 100; i++ )); do
            (( $(log_text "$R/serial.log" | grep -aoF "c chain | d direct" |
                 wc -l) > n )) && break
            sleep 0.1
        done
        (( i < 100 )) || why="the menu did not come back"
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$c" 1 "$tagged tagged $tag and refused with its page; $* untagged"
    else
        result "$c" 0 "$why"
    fi
}

build_all

A_ISOS="archlinux-fake.iso"
CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(uefi-stick uefi-stick-sb bios-stick two-sticks-uefi two-sticks-bios
           mbr-disk-uefi mbr-disk-bios foreign-uefi foreign-bios 4kn-uefi
           4kn-bios 4kn-bios-marker station-uefi station-bios
           station-stick-uefi station-stick-bios bios-only-uefi
           bios-only-uefi-sb efi-only-bios 64-only-bios32)

for c in "${CASES[@]}"; do
    case $c in
    uefi-stick)    stick_case "$c" uefi "$ID_A" "$A_ISOS" "" "" "$R/stick-a.img:boot" ;;
    uefi-stick-sb) stick_case "$c" uefi-sb "$ID_A" "$A_ISOS" "" "" "$R/stick-a.img:boot" ;;
    bios-stick)    stick_case "$c" bios "$ID_A" "$A_ISOS" "" "" "$R/stick-a.img:boot" ;;
    two-sticks-uefi|two-sticks-bios)
        stick_case "$c" "${c#two-sticks-}" "$ID_A" "$A_ISOS" "second-stick.iso" \
            "[library, another install]|[shelf, another install]" \
            "$R/stick-b.img" "$R/stick-a.img:boot" ;;
    mbr-disk-uefi|mbr-disk-bios)
        stick_case "$c" "${c#mbr-disk-}" "$ID_A" "$A_ISOS" \
            "mbr-fat.iso mbr-exfat.iso mbr-logical.iso" \
            "MBR with 4 partition(s)|p5  LBA 233472..299007" \
            "$R/mbr-disk.img" "$R/stick-a.img:boot" ;;
    foreign-uefi|foreign-bios)
        stick_case "$c" "${c#foreign-}" "$ID_A" "$A_ISOS" \
            "foreign.iso foreign-a.efi foreign-b.efi" \
            "[library, another install]|[shelf, another install]" \
            "$R/foreign.img" "$R/stick-a.img:boot" ;;
    4kn-uefi)
        stick_case "$c" uefi "$(cat "$R/stick-4kn.id")" fourk.iso "" "" \
            "$R/stick-4kn.img:boot:4k" ;;
    4kn-bios)
        stick_case "$c" bios "$(cat "$R/stick-4kn.id")" fourk.iso "" \
            "sector size 4096" "$R/stick-4kn.img:boot:4k" ;;
    4kn-bios-marker) case_4kn_marker "$c" ;;
    station-uefi)  case_station "$c" uefi ;;
    station-bios)  case_station "$c" bios ;;
    station-stick-uefi) case_station_stick "$c" uefi ;;
    station-stick-bios) case_station_stick "$c" bios ;;
    bios-only-uefi)    case_bios_only "$c" uefi ;;
    bios-only-uefi-sb) case_bios_only "$c" uefi-sb ;;
    efi-only-bios)
        case_bios_tags "$c" bios "$EFI_ONLY_ISO" "{EFI ONLY}" \
            "this image boots only on a UEFI machine" \
            "$BIOS_ONLY_ISO" "$STUB_ISO" "$X64_ISO" "$MULTI_ISO" ;;
    64-only-bios32)
        case_bios_tags "$c" bios32 "$X64_ISO" "{64-BIT ONLY}" \
            "this image needs a 64-bit processor" \
            "$MULTI_ISO" "$BIOS_ONLY_ISO" "$STUB_ISO" ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
done
exit "$FAILED"
