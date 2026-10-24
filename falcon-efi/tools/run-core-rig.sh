#!/usr/bin/env bash
# falcon-boot: the core step's matrix cells (docs/DESIGN.md Section 10),
# each booted headless and judged from serial like
# falcon-bios/tools/run-jump-rig.sh. UEFI is OVMF (ovmf.sh finds it), BIOS
# is QEMU's SeaBIOS.
#
#   chain-uefi-aa64
#                  the rig stick under AAVMF: a-efi-stub.iso, whose kernel
#                  is an x86-64 EFI stub, is tagged {X64 ONLY}, and c on
#                  payload-test.iso runs its BOOTAA64.EFI
#   uefi-stick-aa64, uefi-stick-aa64-sb
#                  the same under AAVMF on qemu-system-aarch64's virt
#                  machine, Secure Boot off and on: the AArch64 loader
#                  through shimaa64, and the arm64 fixture UKI the shelf
#                  offers it (personal-aa64.efi). Emulated on an x86-64
#                  host, so every wait is AA64_SCALE (6) times as long
#   uefi-stick     the rig stick (mk-test-stick.sh) as the only disk under
#                  OVMF, Secure Boot off: the menu lists its library, the
#                  personal OS unlocks (p3 is argon2id at the stick's cost),
#                  and the fixture UKI reports the passphrase credential and
#                  falcon.ident.cred carrying the stick's install ID. Every
#                  unlocking case's line gives the time from "unlocking..."
#                  to the next screen, read off the serial log
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
#   kdf-memory-uefi, kdf-memory-bios
#                  a stick whose p3 argon2id slot asks for 512 MiB, in a
#                  384 MiB machine: the passphrase is refused with the page
#                  that says how much the slot needs and how much is free
#                  (DESIGN Section 5)
#   relaunch-uefi, relaunch-uefi-sb
#                  stick-a with build/test/relaunch.efi blessed on its
#                  library: the payload, started from the menu, starts a
#                  second falcon-boot off p1 while the first one's
#                  credential filesystem is still published, and the
#                  second one's personal OS still gets both credentials
#                  (DESIGN Section 5.1). Under Secure Boot the payload's
#                  LoadImage is shim's (DESIGN Section 7.6)
#   probe-keys-uefi, probe-keys-bios
#                  stick-a on a disk QEMU throttles to SLOW_BPS: Esc at the
#                  root, pressed while rows are still pending, acts within
#                  KEY_MS (the probe yields at its reads, DESIGN Section 7.8)
#   update-stick-uefi-sb, update-stick-bios
#                  stick-a aged: its p1 loader replaced by the unsigned
#                  stand-in and stage 3's first sector zeroed. `mkstick
#                  update` with the passphrase signs the new loader with
#                  p3's MOK (read into memory, never a file) and rewrites
#                  the stages; stage 3 and the loader read back, the old
#                  ones are in the backup directory, and the stick boots
#                  its UKI under Secure Boot, and under SeaBIOS
#   update-station-uefi-sb, update-station-bios
#                  the station disk aged the same way, both halves: the
#                  ESP's loader and the station partition's stage 3.
#                  `mkstick update --key --cert` (its shelf holds no key),
#                  then the station boots its UKI under each firmware
#   update-refuse  `mkstick update` on a blank disk: refused, nothing
#                  written
#   hibernated-uefi, hibernated-bios
#                  the station disk as a hibernated OS leaves it: its shelf
#                  mounted read-write on this host, personal.efi copied to
#                  resumed.efi and deleted, a commit, and the disk copied
#                  with the shelf still mounted, so its superblock says the
#                  journal needs recovery and resumed.efi is only in the
#                  journal. The passphrase boots resumed.efi (a loader that
#                  ignored the journal would find personal.efi, one before
#                  this did not boot at all), and falcon-bios says on serial
#                  that it read the shelf through its journal
#   64-only-bios32 the same stick under SeaBIOS on an i686 (no long
#                  mode): an ISO whose every kernel is 64-bit is listed with
#                  {64-BIT ONLY}, a multi-arch one (a 64-bit and a 32-bit
#                  kernel) is not, and choosing the first shows its page and
#                  returns to the menu
#   validation-off-uefi
#                  a stick with a blessed ISO (mk-test-blessed-stick.sh), a
#                  second copy of the payload ISO and an ISO whose tree
#                  carries an unsigned BOOTX64.EFI, under OVMF with Secure
#                  Boot on and shim's MokSBState set (DESIGN Section 7.4;
#                  the varstore mk-test-mok.sh writes beside the enforcing
#                  one): the banner says validation is disabled, the
#                  Personal OS row and the blessed row carry
#                  {VALIDATION OFF}, the tree ISO is [unsigned] with no
#                  brace where validation on would say {NOT BLESSED}, and
#                  the payload copy and the stale record's ISO carry none
#   validation-off-pos-uefi
#                  the same machine: Enter on the personal OS warns before
#                  the read, hands over, and the stub reports the firmware
#                  refusing its kernel (DESIGN Section 7.4); the page with
#                  the advice is judged when the image comes back, which
#                  the fixture UKI does not under OVMF (case_voff_pos)
#   validation-off-iso-uefi
#                  the same machine: the unsigned payload ISO boots
#   validation-off-station-uefi
#                  the station disk under the same firmware: the banner,
#                  {VALIDATION OFF} on its Personal OS row, and Enter on it
#                  is the prompt
#   timing-uefi    stick-a under OVMF with 3 s spent at the menu and 2 s at
#                  the prompt, then the personal OS: the boot-timing
#                  variables (DESIGN Section 5.5) as the booted kernel reads
#                  them from efivarfs equal what the loader logged on
#                  serial, are volatile and in systemd-boot's formats,
#                  0 < init < exec, 5 s <= wait < exec - init, and the
#                  loader's TSC rate is within 1% of the kernel's for the
#                  same counter (falcon-efi/tools/bli-judge.sh). The
#                  rate comes however the host's CPUID allows
#   lanes-uefi     the 512 MiB stick under OVMF on four processors: the
#                  personal OS unlocks with p3's four lanes on all four (MP
#                  services, falcon-efi/src/mp), as the loader says on serial
#   lanes-smt-uefi the same on four cores of two threads each: still four,
#                  the second threads left out
#   lanes-alone-uefi
#                  the same stick on one processor: one, and the time the
#                  others are compared with
#   lanes-cheap-uefi
#                  stick-a, at the stick's own cost, on four processors:
#                  one, since the workers would cost more than they save
#   lanes-bios, lanes-smt-bios, lanes-alone-bios, lanes-cheap-bios
#                  the same four under SeaBIOS, falcon-bios waking the
#                  other processors itself (falcon-bios/src/mp)
#   timing-stall-uefi
#                  the same on a processor whose CPUID stops at leaf 0xD,
#                  as the T430's i5-3320M does: the rate comes from the
#                  Stall calibration, and is held to the same 1%
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
. falcon-efi/tools/bli-judge.sh

R=build/core-rig
PASS="${FALCON_TEST_PASS:-falcon-test}"
PASS_B=falcon-other
MKSTICK=build/bin/falcon-boot-mkstick
SIGN_TOOL=build/bin/falcon-boot-sign
TIMEOUT=180
# The machine's memory, in MiB; the refusal cases boot a smaller one.
MEM=1024
# A throttled disk's bytes a second (the probe-keys cases), and how long a
# key may take to act on a menu whose probe is reading from it.
SLOW_BPS=${SLOW_BPS:-65536}
KEY_MS=${KEY_MS:-500}

# Fixed install IDs, so a log names the same install every run.
ID_A=c0de0000000000000000000000000a0a
ID_B=c0de0000000000000000000000000b0b
ID_F=c0de0000000000000000000000000f0f
ID_K=c0de0000000000000000000000000c0c

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
# The validation-off stick's second copy of the payload ISO, under a name no
# bless record claims, so one row boots the ordinary way beside the stale
# one; and an ISO with an unsigned \EFI\BOOT\BOOTX64.EFI in its tree, the
# row the predictions mark {NOT BLESSED} with validation on. The payload ISO
# cannot be that row: its loader is inside its El Torito image only, and the
# survey draws it [no efi].
UNSIGNED_ISO=unsigned-test.iso
TREE_ISO=unsigned-tree.iso

build_sticks() {
    STICK_ID=$ID_A TEST_STICK_IMG=$R/stick-a.img \
        STICK_ISO_FILES="build/test-uefi-biosonly.iso:$BIOS_ONLY_ISO build/test-uefi-stub.iso:$STUB_ISO build/test-bios-efionly.iso:$EFI_ONLY_ISO build/test-bios-64.iso:$X64_ISO build/test-bios-multi.iso:$MULTI_ISO" \
        falcon-efi/tools/mk-test-stick.sh --no-labels >"$R/stick-a.build" 2>&1
    STICK_ID=$ID_B TEST_STICK_IMG=$R/stick-b.img FALCON_TEST_PASS=$PASS_B \
        STICK_ISOS="second-stick.iso" \
        falcon-efi/tools/mk-test-stick.sh --no-labels >"$R/stick-b.build" 2>&1
    STICK_ID=$ID_K TEST_STICK_IMG=$R/stick-kdf.img \
        P3_KDF_MEMORY_KIB=$((512 * 1024)) STICK_ISOS="kdf-stick.iso" \
        falcon-efi/tools/mk-test-stick.sh --no-labels >"$R/stick-kdf.build" 2>&1
    # The validation-off cases' stick: a second copy of the payload ISO under
    # a name no record claims, the tree ISO (the unsigned payload as
    # \EFI\BOOT\BOOTX64.EFI, no El Torito entry: the survey's first stage,
    # nothing the firmware boots), then the blessed ISO and the stale record
    # for payload-test.iso that mk-test-blessed-stick.sh adds.
    rm -rf "$R/tree-iso"
    mkdir -p "$R/tree-iso/EFI/BOOT"
    cp build/test/payload.efi "$R/tree-iso/EFI/BOOT/BOOTX64.EFI"
    xorriso -as mkisofs -quiet -r -J -V UNSIGNED_TREE -o "$R/$TREE_ISO" \
        "$R/tree-iso" >"$R/stick-voff.build" 2>&1
    STICK_ID=$ID_A TEST_STICK_IMG=$R/stick-voff.img \
        STICK_ISO_FILES="build/test-payload.iso:$UNSIGNED_ISO $R/$TREE_ISO:$TREE_ISO" \
        falcon-efi/tools/mk-test-stick.sh --no-labels >>"$R/stick-voff.build" 2>&1
    STICK_IMG=$R/stick-voff.img falcon-efi/tools/mk-test-blessed-stick.sh \
        >>"$R/stick-voff.build" 2>&1
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
             build/mok-test/OVMF_VARS.secboot.moksb.fd \
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
    # SECONDS as KVM takes them; boot scales them for an emulated machine.
    for (( i = 0; i < $2 * 10 * ${SCALE:-1}; i++ )); do
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
    local fw=$1 d img opts n=0 idx=1 args=() fwargs=() vars code
    local cpu=${BOOT_CPU:-host} qemu=qemu-system-x86_64 accel=(-enable-kvm)
    local aa64=0
    shift
    # AArch64 runs on the virt machine under AAVMF, emulated on an x86-64
    # host (TCG), so every wait in the case is scaled for it.
    SCALE=1
    if [[ $fw == uefi-aa64* ]]; then
        aa64=1
        qemu=qemu-system-aarch64
        if [[ $(uname -m) == aarch64 && -w /dev/kvm ]]; then
            cpu=host
        else
            accel=()
            cpu=max
            SCALE=${AA64_SCALE:-6}
        fi
    fi
    for d in "$@"; do
        img=${d%%:*}
        opts=":${d#*:}:"
        [[ "$d" == *:* ]] || opts=::
        if [[ "$opts" == *:slow:* ]]; then
            args+=(-drive "if=none,id=d$n,format=raw,file=$img,throttling.bps-total=$SLOW_BPS")
        else
            args+=(-drive "if=none,id=d$n,format=raw,file=$img")
        fi
        local dev
        if [[ "$opts" == *:4k:* ]]; then
            # NVMe: the one QEMU disk with 4096-byte logical blocks that
            # SeaBIOS offers through INT 13h (virtio-blk, SCSI, USB and
            # IDE do not; DESIGN Section 3.4), and OVMF reads it too.
            dev="nvme,drive=d$n,serial=falcon4k$n,logical_block_size=4096,physical_block_size=4096"
        elif [[ "$fw" == bios || "$fw" == bios32 ]]; then
            dev="ide-hd,drive=d$n,bus=ide.$(( n / 2 )),unit=$(( n % 2 ))"
        elif [[ $aa64 == 1 ]]; then
            # virt has no IDE; AAVMF reads virtio-blk.
            dev="virtio-blk-pci,drive=d$n"
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
    uefi|uefi-sb|uefi-moksb)
        if [[ $fw == uefi ]]; then
            code=$(ovmf_find CODE plain)
            vars=$(ovmf_find VARS plain)
        elif [[ $fw == uefi-sb ]]; then
            code=$(ovmf_find CODE secboot)
            vars=build/mok-test/OVMF_VARS.secboot.mok.fd
        else
            # The same MOK plus MokSBState: Secure Boot on, shim validating
            # nothing (DESIGN Section 7.4). Copied afresh like the others,
            # so no boot inherits the enforcing varstore.
            code=$(ovmf_find CODE secboot)
            vars=build/mok-test/OVMF_VARS.secboot.moksb.fd
        fi
        cp "$vars" "$R/vars.fd"
        chmod u+w "$R/vars.fd"
        fwargs=(-machine q35
                -drive "if=pflash,format=raw,readonly=on,file=$code"
                -drive "if=pflash,format=raw,file=$R/vars.fd")
        ;;
    uefi-aa64|uefi-aa64-sb)
        if [[ $fw == uefi-aa64 ]]; then
            code=$(aavmf_find CODE plain)
            vars=$(aavmf_find VARS plain)
        else
            code=$(aavmf_find CODE secboot)
            vars=build/mok-test/AAVMF_VARS.secboot.mok.fd
        fi
        cp "$vars" "$R/vars.fd"
        chmod u+w "$R/vars.fd"
        fwargs=(-machine virt
                -drive "if=pflash,format=$(fw_format "$code"),readonly=on,file=$code"
                -drive "if=pflash,format=$(fw_format "$vars"),file=$R/vars.fd"
                -device qemu-xhci -device usb-kbd -device ramfb)
        ;;
    bios) fwargs=(-machine pc) ;;
    # The coverage harness's bios32 column: an i686, no long mode.
    bios32) fwargs=(-machine pc); cpu=pentium3 ;;
    esac
    rm -f "$R/serial.log" "$R/mon.sock"
    timeout "$(( TIMEOUT * SCALE ))" "$qemu" "${fwargs[@]}" -m "$MEM" \
        -smp "${BOOT_SMP:-1}" \
        "${accel[@]}" -cpu "$cpu" -display none -net none "${args[@]}" \
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
    local i
    POS_WHY=
    wait_for "passphrase for the encrypted partition" "$1" ||
        { POS_WHY="no passphrase prompt"; return 1; }
    sleep 0.5
    send_keys "$PASS"
    UNLOCK_S=
    local t0=
    wait_for "unlocking..." 10 && t0=$(date +%s.%N)
    for (( i = 0; i < 3000 * ${SCALE:-1}; i++ )); do
        log_text "$R/serial.log" |
            grep -aq 'personal[-a-z0-9]*\.efi\|FALCON-UKI-INIT' && break
        sleep 0.01
    done
    [[ -n "$t0" ]] && UNLOCK_S=$(awk -v a="$t0" -v b="$(date +%s.%N)" \
        'BEGIN { printf "%.2f", b - a }')
    sleep 1
    # One image for this machine is read straight away; several open the
    # chooser first.
    if ! log_text "$R/serial.log" |
            grep -aq 'FALCON-UKI-INIT\|reading personal[-a-z0-9]*\.efi'; then
        choose_personal || return 1
    fi
    wait_for "FALCON-UKI-DONE" 120 || { POS_WHY="the UKI never reported"; return 1; }
    return 0
}

# The chooser's row for personal.efi, on a shelf with more than one UKI: the
# rows as the serial drew them, down to it, Enter. Sets POS_WHY when there
# is no such row.
choose_personal() {
    local rows idx i
    rows=$(log_text "$R/serial.log" | grep -aoE '[A-Za-z ]+\([a-z0-9-]+\.efi\)' |
           sed 's/^ *//' | awk '!seen[$0]++')
    idx=$(printf '%s\n' "$rows" | grep -n 'personal\.efi' | head -1 | cut -d: -f1)
    [[ -n "$idx" ]] || { POS_WHY="the chooser has no personal.efi"; return 1; }
    for (( i = 1; i < idx; i++ )); do
        mon "sendkey down"
        sleep 0.1
    done
    mon "sendkey ret"
}

# grep reads to the end rather than -q: under pipefail, a grep that quits at
# its first match leaves log_text writing into a closed pipe, and the SIGPIPE
# fails the check on a log that has the line.
has() { log_text "$R/serial.log" | grep -aF -- "$1" >/dev/null; }

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
        result "$c" 1 "listed $iso; UKI got both credentials, install ${id:0:8}...; unlock ${UNLOCK_S:-?} s"
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
        has "enter boot   c chain" && why="the menu was drawn"
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

# The boot-timing variables through the personal OS: 3 s at the menu and
# 2 s at the prompt before the passphrase, so at least 5 s of the wait is
# the rig's. CPU is QEMU's -cpu; VIA is how the loader must have had the
# rate.
case_timing() { # case_timing CASE CPU VIA
    local c=$1 why=
    BOOT_CPU=$2 boot uefi "$R/stick-a.img:boot"
    wait_for "Personal OS (encrypted)" 90 || why="no Personal OS row"
    if [[ -z "$why" ]]; then
        sleep 3
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 30 ||
            why="no passphrase prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 2
        pos_unlock 1 || why=$POS_WHY
    fi
    halt
    if [[ -z "$why" ]]; then
        log_text "$R/serial.log" > "$R/$c.txt"
        bli_judge "$R/$c.txt" 5000000 "$3"
        why=$BLI_WHY
    fi
    if [[ -z "$why" ]]; then
        result "$c" 1 "$BLI_DETAIL"
    else
        result "$c" 0 "$why"
    fi
}

# argon2id's lanes on the other processors (falcon-efi/src/mp under OVMF,
# falcon-bios/src/mp under SeaBIOS): STICK under FIRMWARE with SMP QEMU's
# -smp, unlocked, and the loader's serial line
# naming WANT processors, the boot one and the workers that joined. A wrong
# passphrase goes first, so the timed attempt is the second: the first
# write to a page of a fresh guest is the host's page fault, taken one
# vCPU at a time, which a machine of its own never pays.
case_lanes() { # case_lanes CASE FIRMWARE STICK SMP WANT
    local c=$1 fw=$2 stick=$3 why= got
    shift 2
    MEM=2048 BOOT_SMP=$2 boot "$fw" "$stick:boot"
    wait_for "Personal OS (encrypted)" 90 || why="no Personal OS row"
    if [[ -z "$why" ]]; then
        sleep 1
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 30 ||
            why="no passphrase prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys "not-$PASS"
        wait_for "wrong passphrase." 60 || why="the wrong passphrase was not refused"
    fi
    [[ -z "$why" ]] && { pos_unlock 30 || why=$POS_WHY; }
    halt
    local line ms
    line=$(log_text "$R/serial.log" | grep -ao 'argon2id ran on [0-9]* processors in [0-9]* ms' | sed -n 2p)
    got=$(sed -n 's/argon2id ran on \([0-9]*\) .*/\1/p' <<< "$line")
    ms=$(sed -n 's/.* in \([0-9]*\) ms/\1/p' <<< "$line")
    [[ -z "$why" && "$got" != "$3" ]] &&
        why="lanes ran on ${got:-no} processors, not $3"
    # Every processor handed back: the kernel brought all of them up.
    local cpus want_cpus
    cpus=$(log_text "$R/serial.log" | sed -n 's/.*FALCON-UKI-CPUS \([^ ]*\).*/\1/p' | head -n 1)
    want_cpus=0-$(( ${2%%,*} - 1 ))
    [[ ${2%%,*} == 1 ]] && want_cpus=0
    [[ -z "$why" && "$cpus" != "$want_cpus" ]] &&
        why="the kernel brought up CPUs ${cpus:-unknown}, not $want_cpus"
    if [[ -z "$why" ]]; then
        result "$c" 1 "unlocked with lanes on $got processors (-smp $2); keyslots $ms ms, unlock $UNLOCK_S s; kernel CPUs $cpus"
    else
        result "$c" 0 "$why"
    fi
}

# The refusal page: a slot asking for more memory than the machine has.
case_kdf_memory() { # case_kdf_memory CASE FIRMWARE
    local why=
    MEM=384
    boot "$2" "$R/stick-kdf.img:boot"
    MEM=1024
    wait_for "kdf-stick.iso" 90 || why="the menu never listed kdf-stick.iso"
    if [[ -z "$why" ]]; then
        wait_for "Personal OS (encrypted)" 30 || why="no Personal OS row"
    fi
    if [[ -z "$why" ]]; then
        sleep 1
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 30 ||
            why="no passphrase prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys "$PASS"
        wait_for "press any key to return to the menu" 30 ||
            why="no failure page"
    fi
    if [[ -z "$why" ]]; then
        has "failed while unlocking the encrypted partition" ||
            why="the page does not name the unlock"
        log_text "$R/serial.log" | grep -aqE \
            "this keyslot needs 512 MiB of memory; this machine has [0-9]+ MiB free\." ||
            why="${why:-the page does not say the memory}"
        has "FALCON-UKI-INIT" && why="${why:-a UKI started}"
    fi
    local said
    said=$(log_text "$R/serial.log" | grep -aoE "this machine has [0-9]+ MiB free" | head -1)
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "refused: needs 512 MiB; ${said#this machine }"
    else
        result "$1" 0 "$why"
    fi
}

# ---- mkstick update ----

# First LBA of partition N of an image.
part_start() { sgdisk -i "$2" "$1" | awk '/^First sector:/ { print $3 }'; }

# The image aged: the loader at LOADER (an mtools path) replaced by the
# unsigned stand-in, and stage 3's first sector in partition S3PART zeroed.
age() {         # age IMG ESP-PART LOADER S3PART
    local img=$1 esp s3
    esp=$(part_start "$img" "$2")
    s3=$(part_start "$img" "$4")
    mcopy -o -i "$img@@$(( esp * 512 ))" build/test/standin.efi "::$3"
    dd if=/dev/zero of="$img" bs=512 seek=$(( s3 + 32 )) count=1 \
        conv=notrunc status=none
}

# What update left: stage 3's magic, the loader not the stand-in and signed
# by the certificate, a backup of each. Empty when all is well.
updated() {     # updated IMG ESP-PART LOADER S3PART
    local img=$1 esp s3 got
    esp=$(part_start "$img" "$2")
    s3=$(part_start "$img" "$4")
    got=$(dd if="$img" bs=512 skip=$(( s3 + 32 )) count=1 status=none |
          head -c 4)
    [[ "$got" == BOSZ ]] || { echo "stage 3 was not rewritten"; return; }
    rm -f "$R/upd-loader.efi"
    mcopy -n -i "$img@@$(( esp * 512 ))" "::$3" "$R/upd-loader.efi"
    cmp -s "$R/upd-loader.efi" build/test/standin.efi &&
        { echo "the loader is still the stand-in"; return; }
    sbverify --cert build/mok-test/falcon-test-mok.crt "$R/upd-loader.efi" \
        >/dev/null 2>&1 || { echo "the loader is not signed by the MOK"; return; }
    ls "$R"/upd-backup/*-bios-*.bin >/dev/null 2>&1 ||
        { echo "no backup of the BIOS sectors"; return; }
    ls "$R"/upd-backup/*-loader-*.efi >/dev/null 2>&1 ||
        { echo "no backup of the old loader"; return; }
}

# The rig's sticks and station are made signed with the test MOK and with
# it off the shelf; a stick `mkstick create` makes, or a station the
# installer made, has it there, which is the key update reads. The aged
# copy is given it.
key_onto() { # key_onto IMG SHELF-PART
    LOOP=$(sudo losetup -f --show -P "$1")
    MAPPER=falcon-core-rig-upd
    printf '%s' "$PASS" | sudo cryptsetup open "${LOOP}p$2" "$MAPPER" -
    MNT=$(mktemp -d)
    sudo mount "/dev/mapper/$MAPPER" "$MNT"
    sudo install -m 0600 build/mok-test/falcon-test-mok.key "$MNT/falcon-mok.key"
    sudo install -m 0644 build/mok-test/falcon-test-mok.crt "$MNT/falcon-mok.crt"
    sudo umount "$MNT"
    rmdir "$MNT"
    MNT=
    sudo cryptsetup close "$MAPPER"
    MAPPER=
    sudo losetup -d "$LOOP"
    LOOP=
}

case_update_stick() { # case_update_stick CASE FIRMWARE
    local img=$R/stick-upd.img why=
    cp --sparse=always "$R/stick-a.img" "$img"
    key_onto "$img" 3
    age "$img" 1 /EFI/BOOT/grubx64.efi 1
    rm -rf "$R/upd-backup"
    mkdir -p "$R/upd-backup"
    ( umask 077; printf '%s' "$PASS" > "$R/pass" )
    "$MKSTICK" update "$img" --from build/bin --passphrase-file "$R/pass" \
        --backup-dir "$R/upd-backup" --yes >"$R/$1-update.log" 2>&1 ||
        why="mkstick update failed: $(tail -1 "$R/$1-update.log")"
    rm -f "$R/pass"
    [[ -z "$why" ]] && why=$(updated "$img" 1 /EFI/BOOT/grubx64.efi 1)
    if [[ -z "$why" ]]; then
        boot "$2" "$img:boot"
        pos || why=$POS_WHY
        halt
    fi
    if [[ -z "$why" ]]; then
        result "$1" 1 "aged stick updated from p3's MOK; the UKI booted"
    else
        result "$1" 0 "$why"
    fi
}

case_update_station() { # case_update_station CASE FIRMWARE
    local img=$R/station-upd.img why= stn
    cp --sparse=always build/test-station.img "$img"
    stn=$(sgdisk -p "$img" | awk '$7 == "FALCON-STATION" { print $1 }')
    [[ -n "$stn" ]] || { result "$1" 0 "the station disk has no FALCON-STATION"; return; }
    key_onto "$img" 2
    ( umask 077; printf '%s' "$PASS" > "$R/pass" )
    age "$img" 1 /EFI/falcon/grubx64.efi "$stn"
    rm -rf "$R/upd-backup"
    mkdir -p "$R/upd-backup"
    "$MKSTICK" update "$img" --from build/bin --passphrase-file "$R/pass" \
        --backup-dir "$R/upd-backup" --yes >"$R/$1-update.log" 2>&1 ||
        why="mkstick update failed: $(tail -1 "$R/$1-update.log")"
    [[ -z "$why" ]] && why=$(updated "$img" 1 /EFI/falcon/grubx64.efi "$stn")
    if [[ -z "$why" ]]; then
        # A station's judge is case_station's: its menu, then the shelf's
        # prompt, which the new loader (signed, under Secure Boot) reached.
        boot "$2" "$img:boot"
        wait_for "Personal OS (encrypted)" 90 || why="no Personal OS row"
        sleep 1
        [[ -z "$why" ]] && mon "sendkey ret"
        [[ -n "$why" ]] || wait_for "passphrase for the encrypted partition" 90 ||
            why="no passphrase prompt"
        halt
    fi
    if [[ -z "$why" ]]; then
        result "$1" 1 "aged station updated from its shelf's MOK, both halves; the prompt came up"
    else
        result "$1" 0 "$why"
    fi
}

case_update_refuse() { # case_update_refuse CASE
    local why= before after
    rm -f "$R/blank-upd.img"
    truncate -s 64M "$R/blank-upd.img"
    sgdisk -n 1:2048:0 -t 1:0700 "$R/blank-upd.img" >/dev/null
    before=$(sha256sum < "$R/blank-upd.img")
    if "$MKSTICK" update "$R/blank-upd.img" --yes >"$R/$1.log" 2>&1; then
        why="update did not refuse a disk with no falcon on it"
    fi
    after=$(sha256sum < "$R/blank-upd.img")
    [[ "$before" == "$after" ]] || why="${why:-the refused disk was written}"
    grep -q "no falcon boot record" "$R/$1.log" ||
        why="${why:-the refusal does not say why}"
    if [[ -z "$why" ]]; then
        result "$1" 1 "refused, and the disk is as it was"
    else
        result "$1" 0 "$why"
    fi
}

# The stick on a disk throttled to SLOW_BPS, so looking inside one entry
# takes seconds; Esc at the root, pressed while rows are still pending, has
# to take effect within KEY_MS. The probe yields at its reads (DESIGN
# Section 7.8), so a key waits for one read, not for the entry.
# The rows of the newest root menu on serial, in order, Personal OS first.
menu_row() {    # menu_row NAME: its index, 0 for the first row
    log_text "$R/serial.log" |
        awk '/^menu: ISO library/ { n = 0; delete r; on = 1; next }
             on && /^ *$/ { next }
             on && /^(> |  )/ { r[n++] = $0; next }
             on { on = 0 }
             END { for (i = 0; i < n; i++) print r[i] }' |
        grep -nF -- "$1" | head -1 | awk -F: '{ print $1 - 1 }'
}

menus() { log_text "$R/serial.log" | grep -ac '^menu: ISO library'; }

case_relaunch() { # case_relaunch CASE FIRMWARE
    local img=$R/stick-relaunch.img why= row i
    cp --sparse=always "$R/stick-a.img" "$img"
    attach "$img"
    mount_part "${LOOP}p2"
    cp build/test/relaunch.efi "$MNT/relaunch.efi"
    # Signed with the MOK, as a blessed .efi is: such an image is served off
    # the credential filesystem, and that filesystem staying published is
    # the point.
    "$SIGN_TOOL" "$MNT/relaunch.efi" --key build/mok-test/falcon-test-mok.key \
        --cert build/mok-test/falcon-test-mok.crt >/dev/null
    umount_part
    detach
    boot "$2" "$img:boot"
    wait_for "relaunch.efi" 90 || why="the menu never listed relaunch.efi"
    if [[ -z "$why" ]]; then
        sleep 1
        row=$(menu_row "relaunch.efi") || true
        [[ -n "$row" ]] || why="no row for relaunch.efi"
    fi
    if [[ -z "$why" ]]; then
        for (( i = 0; i < row; i++ )); do
            mon "sendkey down"
            sleep 0.2
        done
        sleep 0.3
        mon "sendkey ret"
        # has, not wait_for: the second loader's first screen follows the
        # marker at once, and wait_for's grep -q under pipefail fails on a
        # match with that much log after it (see has).
        for (( i = 0; i < 600; i++ )); do
            has "FALCON-RELAUNCH-LOADED" && break
            sleep 0.1
        done
        has "FALCON-RELAUNCH-LOADED" ||
            why="the payload did not load a second falcon-boot"
        has "naming the image on the synthetic filesystem" ||
            why="${why:-relaunch.efi was not served off the credential filesystem}"
    fi
    if [[ -z "$why" ]]; then
        for (( i = 0; i < 900; i++ )); do
            (( $(menus) >= 2 )) && break
            sleep 0.1
        done
        (( $(menus) >= 2 )) || why="the second falcon-boot never showed its menu"
    fi
    if [[ -z "$why" ]]; then
        sleep 1
        mon "sendkey ret"
        pos_unlock 30 || why=$POS_WHY
    fi
    if [[ -z "$why" ]]; then
        has "credential filesystem could not be published" &&
            why="the second loader could not publish its credential filesystem"
        has "FALCON-CRED-FILE cryptsetup.passphrase.cred" ||
            why="${why:-no passphrase credential}"
        has "FALCON-CRED-OK" || why="${why:-the passphrase credential is wrong}"
    fi
    halt
    rm -f "$img"
    if [[ -z "$why" ]]; then
        result "$1" 1 "a falcon-boot started from relaunch.efi handed the UKI both credentials"
    else
        result "$1" 0 "$why"
    fi
}

case_probe_keys() { # case_probe_keys CASE FIRMWARE
    local why= t0 t1 ms=-
    boot "$2" "$R/stick-a.img:boot:slow"
    wait_for "enter boot   c chain" 170 || why="no menu"
    [[ -z "$why" ]] && { has "..." || why="no row was pending"; }
    if [[ -z "$why" ]]; then
        sleep 2
        t0=$(date +%s%N)
        mon "sendkey esc"
        if wait_for "falcon-boot: returning to firmware." 120; then
            t1=$(date +%s%N)
            ms=$(( (t1 - t0) / 1000000 ))
            (( ms <= KEY_MS )) || why="Esc took $ms ms to act (limit $KEY_MS)"
        else
            why="Esc never acted"
        fi
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "Esc acted in $ms ms with the probe reading at $SLOW_BPS B/s"
    else
        result "$1" 0 "$why"
    fi
}

# The station disk's copy, taken with its shelf mounted on this host as a
# hibernated OS would have it: nothing unmounts it before the copy, and the
# rename lives in the journal alone. commit=1 and a pause stand in for the
# kernel's own commit; sync would also write the metadata home and leave
# nothing for the journal to hold (falcon-core/tools/mk-test-jbd2.sh).
hibernated_disk() {
    local work=$R/station-hibernating.img img=$R/station-hibernated.img feat
    rm -f "$work" "$img"
    cp --sparse=always build/test-station.img "$work"
    MAPPER=falcon-core-rig-hib
    LOOP=$(sudo losetup -f --show -P "$work")
    printf '%s' "$PASS" | sudo cryptsetup open "${LOOP}p2" "$MAPPER" -
    MNT=$(mktemp -d)
    sudo mount -o commit=1 "/dev/mapper/$MAPPER" "$MNT"
    sudo cp "$MNT/uki/personal.efi" "$MNT/uki/resumed.efi"
    sudo sync "$MNT/uki/resumed.efi"
    sudo rm "$MNT/uki/personal.efi"
    sleep 2
    cp --sparse=always "$work" "$img"
    sudo umount "$MNT"
    rmdir "$MNT"
    MNT=
    sudo cryptsetup close "$MAPPER"
    sudo losetup -d "$LOOP"
    LOOP=
    rm -f "$work"
    # The copy is what it claims to be: a shelf that needs recovery.
    LOOP=$(sudo losetup -f --show -r -P "$img")
    printf '%s' "$PASS" | sudo cryptsetup open --readonly "${LOOP}p2" \
        "$MAPPER" -
    feat=$(sudo dumpe2fs -h "/dev/mapper/$MAPPER" 2>/dev/null |
        sed -n 's/^Filesystem features://p')
    sudo cryptsetup close "$MAPPER"
    MAPPER=
    sudo losetup -d "$LOOP"
    LOOP=
    [[ "$feat" == *needs_recovery* ]]
}

case_hibernated() { # case_hibernated CASE FIRMWARE
    local why=
    hibernated_disk || why="the copy's shelf does not need recovery"
    [[ -z "$why" ]] && boot "$2" "$R/station-hibernated.img:boot"
    [[ -z "$why" ]] && { wait_for "Personal OS (encrypted)" 90 || why="no Personal OS row"; }
    if [[ -z "$why" ]]; then
        sleep 1
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 90 ||
            why="no passphrase prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys "$PASS"
        wait_for "FALCON-UKI-DONE" 120 || why="the UKI never reported"
    fi
    [[ -z "$why" ]] && { has "reading resumed.efi" ||
        why="the loader did not read the UKI the journal names"; }
    if [[ -z "$why" && "$2" == bios ]]; then
        has "falcon-bios: the shelf is read through its journal" ||
            why="stage 3 did not say it read the journal"
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "the shelf read through its journal: resumed.efi booted"
    else
        result "$1" 0 "$why"
    fi
}

case_station() { # case_station CASE FIRMWARE
    local id why=
    id=$("$MKSTICK" ident build/test-station.img |
         awk -F '\t' '$2 == "boot" { print $3; exit }')
    boot "$2" build/test-station.img:boot
    # Either station opens at its menu (DESIGN Section 12.3), the shelf's
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

# The first list's rows, as the transcript has them (falcon-core/src/tui
# writes a menu's rows on serial as it opens): the lines after "menu: "
# up to the key hints, the selection's '>' dropped. The same text for the
# same rows on either loader.
first_rows() {
    log_text "$R/serial.log" |
        awk '/^menu: / { m = 1; next } m && /^enter boot/ { exit }
             m { sub(/^[> ] /, ""); print }' |
        tr -s ' \n' ' '
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
        n=$(log_text "$R/serial.log" | grep -aoF "c chain   d direct" | wc -l)
        mon "sendkey spc"
        local i
        for (( i = 0; i < 100; i++ )); do
            (( $(log_text "$R/serial.log" | grep -aoF "c chain   d direct" |
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
        n=$(log_text "$R/serial.log" | grep -aoF "c chain   d direct" | wc -l)
        mon "sendkey spc"
        for (( i = 0; i < 100; i++ )); do
            (( $(log_text "$R/serial.log" | grep -aoF "c chain   d direct" |
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

# ---- validation off (DESIGN Section 7.4) ----

wait_for_re() { # wait_for_re REGEX SECONDS: 0 once the log matches it
    local i
    for (( i = 0; i < $2 * 10; i++ )); do
        log_text "$R/serial.log" | grep -aqE -- "$1" && return 0
        kill -0 "$QPID" 2>/dev/null || { log_text "$R/serial.log" |
            grep -aqE -- "$1"; return; }
        sleep 0.1
    done
    return 1
}

# The tag columns a root-screen row was drawn with, every drawing of it:
# its brackets and, when there is one, the brace after them.
row_tags() {    # row_tags NAME
    squeezed | grep -aoE -- "$1( \[[^]]*\])+( \{[A-Z0-9 -]+\})?"
}

# Any key on a route's page, then the menu's key hint drawn once more.
menu_back() {   # menu_back: 0 once the menu is back
    local n i
    n=$(log_text "$R/serial.log" | grep -aoF "c chain   d direct" | wc -l)
    mon "sendkey spc"
    for (( i = 0; i < 100; i++ )); do
        (( $(log_text "$R/serial.log" | grep -aoF "c chain   d direct" |
             wc -l) > n )) && return 0
        sleep 0.1
    done
    return 1
}

# The validation-off stick's rows (build_sticks), and the words the loader
# uses for the state: sb_mode_label's (falcon-efi/src/secboot/sb.c), the
# row's reason (DESIGN Section 7.8), and load_ops_inner_load_advice's
# (falcon-efi/src/loader/loadops.c).
VOFF_BLESSED_ISO=blessed-test.iso
VOFF_STALE_ISO=payload-test.iso
VOFF_BANNER='SB: on (shim validation DISABLED)'
VOFF_TAG='{VALIDATION OFF}'
VOFF_POS_ROW='Personal OS \(encrypted\) +\{VALIDATION OFF\}'
VOFF_ADVICE='so shim left the'
# sd-stub's own line for the firmware's refusal of its kernel (src/boot/linux.c).
VOFF_REFUSED='Error loading kernel image'

case_voff_menu() { # case_voff_menu CASE
    local why= one
    boot uefi-moksb "$R/stick-voff.img:boot"
    wait_for "$VOFF_BLESSED_ISO" 90 || why="the menu never listed $VOFF_BLESSED_ISO"
    if [[ -z "$why" ]]; then
        has "$VOFF_BANNER" || why="the banner does not say validation is disabled"
    fi
    if [[ -z "$why" ]]; then
        wait_for_re "$VOFF_BLESSED_ISO +\[blessed\] +\{VALIDATION OFF\}" 30 ||
            why="$VOFF_BLESSED_ISO is not drawn [blessed] $VOFF_TAG"
    fi
    if [[ -z "$why" ]]; then
        log_text "$R/serial.log" | grep -aqE "$VOFF_POS_ROW" ||
            why="the Personal OS row is not drawn with $VOFF_TAG"
    fi
    # Rows that boot the ordinary way have no reason in this state: the
    # probe has been inside each (its brackets are filled) and drew no
    # brace. The tree ISO is the one with a verdict to suppress; the
    # payload ISO and its stale twin have no first stage to predict.
    for one in "$TREE_ISO:\[unsigned\]" \
               "$UNSIGNED_ISO:\[no efi\]" \
               "$VOFF_STALE_ISO:\[no efi\] +\[blessed, stale\]"; do
        [[ -n "$why" ]] && break
        wait_for_re "${one%%:*} +${one#*:}" 30 ||
            why="${one%%:*} is not drawn ${one#*:}"
        [[ -n "$why" ]] && break
        row_tags "${one%%:*}" | grep -aqF "{" &&
            why="${one%%:*} is drawn with a brace"
    done
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "banner validation off; $VOFF_TAG on the Personal OS and $VOFF_BLESSED_ISO; $TREE_ISO [unsigned] with no brace; $UNSIGNED_ISO and $VOFF_STALE_ISO untagged"
    else
        result "$1" 0 "$why"
    fi
}

# Enter on the personal OS: the prompt, the chooser (this shelf has two
# UKIs), the warning before the read, the handover, and the stub's own line
# that the firmware refused its kernel (DESIGN Section 7.4). The fixture
# UKI does not come back from there under OVMF: after the refusal the
# machine faults inside the firmware, on the bless route too, with 4 GB and
# with SMM on alike, so the page with the advice is judged only when it
# appears. The corpus's blessed wraps come back and show it
# (docs/coverage.md, the moksb-blessed column).
case_voff_pos() { # case_voff_pos CASE
    local why= i after
    boot uefi-moksb "$R/stick-voff.img:boot"
    wait_for "Personal OS (encrypted)" 90 || why="no Personal OS row"
    if [[ -z "$why" ]]; then
        sleep 1
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 30 ||
            why="no passphrase prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys "$PASS"
        for (( i = 0; i < 900; i++ )); do
            log_text "$R/serial.log" |
                grep -aq 'personal\.efi\|very unlikely to start' && break
            sleep 0.1
        done
        sleep 0.5
        if ! has "very unlikely to start on this machine"; then
            choose_personal || why=$POS_WHY
        fi
    fi
    if [[ -z "$why" ]]; then
        wait_for "very unlikely to start on this machine" 30 ||
            why="no warning before the read"
    fi
    if [[ -z "$why" ]]; then
        wait_for "handing over to the image" 120 || why="no handover"
    fi
    if [[ -z "$why" ]]; then
        wait_for_re "$VOFF_REFUSED|$VOFF_ADVICE|FALCON-UKI-INIT" 120 ||
            why="the image neither reported a refusal nor came back"
    fi
    if [[ -z "$why" ]]; then
        has "FALCON-UKI-INIT" && why="the UKI started"
    fi
    if [[ -z "$why" ]]; then
        # The page after the image, when the image comes back at all.
        if wait_for_re "the image exited, status|personal OS boot failed while" 20; then
            after="the page with the advice, the menu again"
            wait_for "$VOFF_ADVICE" 10 || why="the page after the UKI has no advice"
            [[ -n "$why" ]] || wait_for "press any key to return to the menu" 10 ||
                why="the page has no way back"
            [[ -n "$why" ]] || menu_back || why="the menu did not come back"
        elif has "Exception Type"; then
            after="then the machine faulted in a firmware driver"
        else
            after="then nothing came back"
        fi
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "warned before the read; the firmware refused the UKI's kernel; $after"
    else
        result "$1" 0 "$why"
    fi
}

# AArch64's chainload: c on the payload ISO starts its BOOTAA64.EFI, which
# the x64 one sits beside; and an ISO whose only way in is an x86-64 EFI
# stub kernel is tagged for that machine.
case_chain_aa64() { # case_chain_aa64 CASE FIRMWARE
    local why= row i
    boot "$2" "$R/stick-a.img:boot"
    wait_for "payload-test.iso" 90 || why="the menu never listed payload-test.iso"
    if [[ -z "$why" ]]; then
        wait_for_re 'a-efi-stub\.iso .*\{X64 ONLY\}' 60 ||
            why="a-efi-stub.iso is not tagged {X64 ONLY}"
    fi
    if [[ -z "$why" ]]; then
        row=$(row_of "payload-test.iso") || true
        [[ -n "$row" ]] || why="no row for payload-test.iso"
    fi
    if [[ -z "$why" ]]; then
        for (( i = 0; i < row; i++ )); do
            mon "sendkey down"
            sleep 0.2
        done
        sleep 0.3
        mon "sendkey c"
        wait_for "FALCON-PAYLOAD-OK" 90 || why="the payload never ran"
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "{X64 ONLY} on the x86-64 stub ISO; c chainloaded the payload's BOOTAA64.EFI"
    else
        result "$1" 0 "$why"
    fi
}

# The unsigned payload ISO, Enter on its row: the payload runs.
case_voff_iso() { # case_voff_iso CASE
    local why= row i
    boot uefi-moksb "$R/stick-voff.img:boot"
    wait_for "$UNSIGNED_ISO" 90 || why="the menu never listed $UNSIGNED_ISO"
    if [[ -z "$why" ]]; then
        sleep 1
        row=$(row_of "$UNSIGNED_ISO") || true
        [[ -n "$row" ]] || why="no row for $UNSIGNED_ISO"
    fi
    if [[ -z "$why" ]]; then
        for (( i = 0; i < row; i++ )); do
            mon "sendkey down"
            sleep 0.2
        done
        sleep 0.3
        mon "sendkey ret"
        wait_for "FALCON-PAYLOAD-OK" 90 || why="the payload never ran"
    fi
    if [[ -z "$why" ]]; then
        wait_for "the payload exited, status" 30 || why="no page after the payload"
        wait_for "press any key to return to the menu" 10 ||
            why="${why:-the page has no way back}"
    fi
    if [[ -z "$why" ]]; then
        menu_back || why="the menu did not come back"
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "$UNSIGNED_ISO ran its payload; the menu again"
    else
        result "$1" 0 "$why"
    fi
}

# The station under the same firmware: its banner, its Personal OS row
# tagged, and Enter on it the prompt, as case_station judges.
case_voff_station() { # case_voff_station CASE
    local why=
    boot uefi-moksb build/test-station.img:boot
    wait_for "Personal OS (encrypted)" 90 || why="no Personal OS row"
    if [[ -z "$why" ]]; then
        has "$VOFF_BANNER" || why="the banner does not say validation is disabled"
        wait_for_re "$VOFF_POS_ROW" 10 ||
            why="${why:-the Personal OS row is not drawn with $VOFF_TAG}"
    fi
    if [[ -z "$why" ]]; then
        sleep 1
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 90 ||
            why="no passphrase prompt"
    fi
    halt
    if [[ -z "$why" ]]; then
        result "$1" 1 "banner validation off; $VOFF_TAG on the Personal OS row; Enter on it is the prompt"
    else
        result "$1" 0 "$why"
    fi
}

build_all

A_ISOS="archlinux-fake.iso"
CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(uefi-stick uefi-stick-sb uefi-stick-aa64 uefi-stick-aa64-sb
           chain-uefi-aa64 bios-stick two-sticks-uefi two-sticks-bios
           mbr-disk-uefi mbr-disk-bios foreign-uefi foreign-bios 4kn-uefi
           4kn-bios 4kn-bios-marker station-uefi station-bios
           station-stick-uefi station-stick-bios bios-only-uefi
           bios-only-uefi-sb efi-only-bios 64-only-bios32
           validation-off-uefi validation-off-pos-uefi
           validation-off-iso-uefi validation-off-station-uefi
           kdf-memory-uefi kdf-memory-bios hibernated-uefi hibernated-bios
           relaunch-uefi relaunch-uefi-sb probe-keys-uefi probe-keys-bios
           update-stick-uefi-sb update-stick-bios update-station-uefi-sb
           update-station-bios
           update-refuse timing-uefi timing-stall-uefi lanes-uefi
           lanes-smt-uefi lanes-alone-uefi lanes-cheap-uefi lanes-bios
           lanes-smt-bios lanes-alone-bios lanes-cheap-bios)

for c in "${CASES[@]}"; do
    case $c in
    uefi-stick)    stick_case "$c" uefi "$ID_A" "$A_ISOS" "" "" "$R/stick-a.img:boot" ;;
    uefi-stick-sb) stick_case "$c" uefi-sb "$ID_A" "$A_ISOS" "" "" "$R/stick-a.img:boot" ;;
    chain-uefi-aa64) case_chain_aa64 "$c" uefi-aa64 ;;
    uefi-stick-aa64|uefi-stick-aa64-sb)
        stick_case "$c" "uefi-aa64${c#uefi-stick-aa64}" "$ID_A" "$A_ISOS" "" "" \
            "$R/stick-a.img:boot" ;;
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
    kdf-memory-uefi) case_kdf_memory "$c" uefi ;;
    kdf-memory-bios) case_kdf_memory "$c" bios ;;
    update-stick-uefi-sb)   case_update_stick "$c" uefi-sb ;;
    update-stick-bios)      case_update_stick "$c" bios ;;
    update-station-uefi-sb) case_update_station "$c" uefi-sb ;;
    update-station-bios)    case_update_station "$c" bios ;;
    update-refuse)          case_update_refuse "$c" ;;
    relaunch-uefi)   case_relaunch "$c" uefi ;;
    relaunch-uefi-sb) case_relaunch "$c" uefi-sb ;;
    probe-keys-uefi) case_probe_keys "$c" uefi ;;
    probe-keys-bios) case_probe_keys "$c" bios ;;
    hibernated-uefi) case_hibernated "$c" uefi ;;
    hibernated-bios) case_hibernated "$c" bios ;;
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
    validation-off-uefi)         case_voff_menu "$c" ;;
    validation-off-pos-uefi)     case_voff_pos "$c" ;;
    validation-off-iso-uefi)     case_voff_iso "$c" ;;
    validation-off-station-uefi) case_voff_station "$c" ;;
    # The host's own leaves, whichever way they answer; level=0xd hides
    # 0x15 and 0x16, as an Ivy Bridge has neither.
    # stick-kdf's p3 is 512 MiB, t=4, 4 lanes, worth the workers: four
    # cores is every lane its own; four cores of two threads leave the
    # second threads out and come to the same; one processor is the
    # baseline. stick-a's p3, the stick's own cost, is not worth them.
    lanes-uefi|lanes-bios)
        case_lanes "$c" "${c#lanes-}" "$R/stick-kdf.img" 4 4 ;;
    lanes-smt-uefi|lanes-smt-bios)
        case_lanes "$c" "${c#lanes-smt-}" "$R/stick-kdf.img" \
            "8,sockets=1,cores=4,threads=2" 4 ;;
    lanes-alone-uefi|lanes-alone-bios)
        case_lanes "$c" "${c#lanes-alone-}" "$R/stick-kdf.img" 1 1 ;;
    lanes-cheap-uefi|lanes-cheap-bios)
        case_lanes "$c" "${c#lanes-cheap-}" "$R/stick-a.img" 4 1 ;;
    timing-uefi)       case_timing "$c" host "" ;;
    timing-stall-uefi) case_timing "$c" host,level=0xd stall ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
done
exit "$FAILED"
