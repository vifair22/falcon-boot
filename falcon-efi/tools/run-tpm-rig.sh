#!/usr/bin/env bash
# falcon-boot: the TPM rig (DESIGN Section 12.15), each case
# booted headless under OVMF with a software TPM (swtpm, through
# falcon-efi/tools/swtpm.sh) and judged from serial, with Secure Boot off
# (uefi-*) and on (uefi-sb-*, the test MOK enrolled and its cert in db).
#
# What the firmware here does, with the probe below
# (falcon-efi/tools/tpm-probe/): Gentoo's sys-firmware/edk2-bin-202408
# (/usr/share/edk2/OvmfX64/OVMF_CODE.fd and OVMF_CODE.secboot.fd) supports
# both. With a TPM 2.0 on TIS or CRB it installs EFI_TCG2_PROTOCOL
# (protocol 1.1, TPMPresentFlag 1, SHA-1/256/384/512 banks all active, event
# logs in both formats) and measures the boot: PCR0 read back through
# SubmitCommand is not zero. With a TPM 1.2 it installs EFI_TCG_PROTOCOL
# (spec 1.2, present, not deactivated) and PCR0 read through
# PassThroughToTpm is not zero. A fresh swtpm needs no swtpm_setup for
# either: the firmware's TPM_Startup is enough to read and extend PCRs.
# EDK2 lays EFI_TCG2_BOOT_SERVICE_CAPABILITY out with natural alignment
# (36 bytes), not packed; a packed copy reads TPMPresentFlag as 0.
#
# Case names are <firmware>-<tpm>-<case>: firmware uefi or uefi-sb, tpm
# tpm2 (2.0 on TIS), tpm2crb (2.0 on CRB) or tpm12 (1.2 on TIS).
#
#   probe     the probe as the removable-media default loader on a disk of
#             its own: the protocol for that TPM is installed, the TPM is
#             present, and PCR0 read through the protocol is not zero
#   station   the UEFI station (make test-station's disk) with the probe as
#             its memtest.efi, started as the default: the station boots
#             with a TPM attached and what it starts still sees one.
#   totp      the station's TOTP check (DESIGN Section 12.15), over four
#             boots of one TPM state (SWTPM_KEEP) with the clock at
#             RTC_BASE:
#               1. the station starts the probe, which saves the firmware's
#                  event log onto the ESP;
#               2. on the host, between boots, `mkstick station totp
#                  enroll` against the same state (--tpm-socket) with that
#                  log (--event-log), sealing to PCRs 0, 2, 4 and 7 as the
#                  station will see them, and writing [totp];
#               3. the station shows the code the host computes from the
#                  printed secret for the clock, and the seed's bytes never
#                  cross the TPM's wire in the clear (swtpm's own log,
#                  level 5; on 1.2 the enrollment's plain NV write is the
#                  control that the search can see them); once the probe
#                  has run and come back, the menu says the code is shown
#                  after a restart, since the seed was wiped at handover;
#                  then, in a boot of its own, the Personal OS row chosen
#                  over the countdown shows the code above the passphrase
#                  prompt;
#               4. the same disk and varstore under the other OVMF build
#                  (the Secure Boot one for uefi-*, the plain one for
#                  uefi-sb-*), which moves PCR 0 as a firmware update
#                  would: the station says the TPM did not release the
#                  code, and still boots its default. A second db entry
#                  would move PCR 7 instead, but OVMF's TPM 1.2 driver
#                  does not measure db, so it would prove nothing there.
#             The event-log replay, the seal and the unseal are the ones a
#             machine runs; only the TPM's transport is swapped.
#   totp-update  totp's steps 1 and 2, then `mkstick update` to a station
#             loader with another Authenticode digest, resealing from the
#             old loader's boot log (DESIGN Section 12.14); the next boot
#             shows the code of the secret update printed.
#   totp-jump  the station without shim (DESIGN Section 12.2): the loader
#             as \EFI\falcon\falconx64.efi, the jumper as
#             \EFI\BOOT\BOOTX64.EFI, and the `falcon` entry naming it.
#             totp's steps 1 and 2 with the first boot through that entry,
#             then the code on a boot through the entry and on one with no
#             entry at all, through the removable-media path: the jumper
#             either way, so the same PCR 4 and the code both times.
#
# A case whose firmware installs no protocol for its TPM version SKIPs and
# says so: that is this OVMF, not falcon. OVMF_DIR points the rig at
# another build (EDK2 OvmfPkg with TPM2_ENABLE and TPM1_ENABLE).
#
# Nothing touches a real disk or this machine's TPM: every disk is an image
# file, every varstore a copy, and every TPM a swtpm with a fresh state
# directory under build/tpm-rig.
#
# Usage: falcon-efi/tools/run-tpm-rig.sh [case...]   (default: every case)
# Env:   SWTPM_LOG_LEVEL  swtpm's log level into build/tpm-rig/swtpm/ (0;
#                         the totp cases set 5 for their wire check)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. falcon-efi/tools/ovmf.sh
. falcon-efi/tools/swtpm.sh

R=build/tpm-rig
PROBE=build/test/tpm-probe.efi
PROBE_SIGNED=build/test/tpm-probe-signed.efi
STATION=build/test-station.img
LOADER=build/bin/falcon-boot-station-signed.efi
JUMPER=build/bin/falcon-esp-jump-signed.efi
MOK_VARS=build/mok-test/OVMF_VARS.secboot.mok.fd
MOK_CRT=build/mok-test/falcon-test-mok.crt
MKSTICK=build/bin/falcon-boot-mkstick
TIMEOUT=180
# x64, or aa64 for the aa64- cases (set_arch): the images, the names on the
# ESP, the firmware, the bus, the TPM's device, and SCALE, which stretches
# every wait for an emulated machine.
ARCH=x64
UP=X64
LOW=x64
SCALE=1
SUFFIX=
ARCH_OPT=()
MEMTEST_LABEL="Memory test (memtest86+)"
# The name the firmware gives the disk's own boot option: OVMF's for an IDE
# disk, AAVMF's for a virtio one.
DISK_BOOT_NAME="UEFI QEMU HARDDISK"

# Where the running case's swtpm keeps its state; the totp cases use their
# own. RTC_ARGS sets the guest's clock (the totp cases). OTHER_FW=1 boots
# the other OVMF build with the same varstore, which moves PCR 0.
TPM_DIR=$R/swtpm
RTC_ARGS=()
OTHER_FW=0
RTC_BASE=2026-09-28T12:00:00

# virt-fw-vars from PATH, else the venv mk-test-mok.sh makes when PATH has
# none; only the Secure Boot cases need it.
VFV=$(command -v virt-fw-vars || true)
[[ -z $VFV && -x build/shim/venv/bin/virt-fw-vars ]] &&
    VFV=build/shim/venv/bin/virt-fw-vars

QPID=
cleanup() {
    [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null || true
    swtpm_stop
    swtpm_serve_stop
}
trap cleanup EXIT

skip_all() {
    echo "SKIP  test-tpm-rig: $*"
    exit 0
}

swtpm_have || skip_all "swtpm is not installed (Gentoo app-crypt/swtpm, Debian swtpm)."
[[ -n "$(ovmf_find CODE plain || true)" ]] ||
    skip_all "no OVMF found (set OVMF_DIR)."
mkdir -p "$R"

# ---- images ----

# A GPT disk whose ESP has the probe as \EFI\BOOT\BOOTX64.EFI. OVMF does
# not boot a FAT with no partition table from a hard disk, hence the GPT.
probe_disk() {  # probe_disk IMAGE EFI
    local img=$1 efi=$2
    rm -f "$img"
    truncate -s 48M "$img"
    sgdisk -n 1:2048:0 -t 1:EF00 "$img" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n TPMPROBE "$img" $(( 46 * 1024 )) \
        >/dev/null 2>&1
    mmd -i "$img@@1M" ::/EFI ::/EFI/BOOT
    mcopy -i "$img@@1M" "$efi" "::/EFI/BOOT/BOOT$UP.EFI"
}

# The station disk, copied, with the loader as built now and the probe as
# memtest.efi, which STATION.INI makes the default.
station_disk() {  # station_disk IMAGE EFI
    local img=$1 efi=$2 dir
    cp --reflink=auto "$STATION" "$img"
    for dir in falcon BOOT; do
        mcopy -o -i "$img@@1M" "$LOADER" "::/EFI/$dir/grub$LOW.efi"
    done
    mcopy -o -i "$img@@1M" "$efi" ::/EFI/falcon/memtest.efi
    printf '[general]\ndefault = memtest\ntimeout = %s\n' "${INI_TIMEOUT:-1}" \
        > "$R/STATION.INI"
    mcopy -o -i "$img@@1M" "$R/STATION.INI" ::/EFI/falcon/STATION.INI
    # JUMP=1: a station without shim, as `station install` lays it out
    # (DESIGN Section 12.2): the loader as \EFI\falcon\falconx64.efi and
    # the jumper as \EFI\BOOT\BOOTX64.EFI, no shim anywhere.
    if [[ ${JUMP:-0} == 1 ]]; then
        mdel -i "$img@@1M" "::/EFI/falcon/shim$LOW.efi" \
            "::/EFI/falcon/mm$LOW.efi" "::/EFI/falcon/grub$LOW.efi" \
            "::/EFI/BOOT/grub$LOW.efi" "::/EFI/BOOT/mm$LOW.efi" \
            2>/dev/null || true
        mcopy -o -i "$img@@1M" "$LOADER" "::/EFI/falcon/falcon$LOW.efi"
        mcopy -o -i "$img@@1M" "$JUMPER" "::/EFI/BOOT/BOOT$UP.EFI"
    fi
}

# ENTRY_JUMP=1: the varstore gets the `falcon` entry install makes without
# shim, naming the disk's \EFI\BOOT\BOOTX64.EFI, first in BootOrder; with
# none the firmware boots the same file by the removable-media path.
jump_entry_json() {     # jump_entry_json IMAGE
    local facts
    facts=$(sgdisk -i 1 "$1" | awk '
        /^First sector:/          { s = $3 }
        /^Partition size:/        { n = $3 }
        /^Partition unique GUID:/ { g = $4 }
        END { print s, n, g }')
    python3 - $facts "$ARCH" "$UP" <<'EOF'
import json, struct, sys, uuid
start, size, guid, arch, up = sys.argv[1:6]
pre = struct.pack("<BBHII", 2, 1, 12, 0x0A0341D0, 0)
if arch == "aa64":
    # PciRoot(0)/Pci(5,0): virt's virtio-blk, at the address boot gives it.
    pre += struct.pack("<BBHBB", 1, 1, 6, 0, 5)
else:
    pre += struct.pack("<BBHBB", 1, 1, 6, 2, 0x1F)
    pre += struct.pack("<BBHHHH", 3, 0x12, 10, 0, 0xFFFF, 0)
hd = struct.pack("<BBHIQQ", 4, 1, 42, 1, int(start), int(size))
hd += uuid.UUID(guid).bytes_le + bytes([2, 2])
name = ("\\EFI\\BOOT\\BOOT" + up + ".EFI" + "\0").encode("utf-16-le")
fp = struct.pack("<BBH", 4, 4, 4 + len(name)) + name
dp = pre + hd + fp + bytes([0x7F, 0xFF, 4, 0])
desc = "falcon\0".encode("utf-16-le")
g = "8be4df61-93ca-11d2-aa0d-00e098032b8c"
print(json.dumps({"version": 2, "variables": [
    {"name": "Boot0001", "guid": g, "attr": 7,
     "data": (struct.pack("<IH", 1, len(dp)) + desc + dp).hex()},
    {"name": "BootOrder", "guid": g, "attr": 7, "data": "0100"}]}))
EOF
}

# ---- the machine ----

log_text() {
    tr -d '\r' 2>/dev/null < "$1" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}

mon() { printf '%s\n' "$*" | socat - "UNIX-CONNECT:$R/mon.sock" >/dev/null 2>&1 || true; }

wait_for() {    # wait_for PATTERN SECONDS
    local i
    for (( i = 0; i < $2 * 10 * SCALE; i++ )); do
        log_text "$R/serial.log" | grep -aqF -- "$1" && return 0
        kill -0 "$QPID" 2>/dev/null || { log_text "$R/serial.log" |
            grep -aqF -- "$1"; return; }
        sleep 0.1
    done
    return 1
}

has() { log_text "$R/serial.log" | grep -aqF -- "$1"; }

# boot FIRMWARE TPM DISK: a fresh swtpm, then the disk under OVMF. As in
# the station rig, a machine that has not reached BdsDxe in a minute is
# started again once, from a fresh varstore and a fresh TPM, and says so.
boot() {
    boot_once "$@"
    if ! wait_for "BdsDxe: " 60; then
        halt
        echo "note: $1 $2: the firmware stopped before its boot manager; started again" >&2
        boot_once "$@"
    fi
}

boot_once() {
    local fw=$1 tpm=$2 disk=$3 code vars
    if [[ $ARCH == aa64 ]]; then
        local secure
        secure=$(aavmf_find CODE secboot)
        # Debian builds TCG2 into the Secure Boot AAVMF alone; with a blank
        # store (no PK) it is a machine with Secure Boot off. The other
        # firmware is its strict-NX build: TCG2 too, and another PCR 0.
        code=$secure
        [[ $OTHER_FW == 1 ]] && code=${secure%.fd}.strictnx.fd
        if [[ $fw == uefi ]]; then
            vars=$(aavmf_find VARS plain)
        else
            vars=$MOK_VARS
        fi
    elif [[ $fw == uefi ]]; then
        code=$(ovmf_find CODE plain)
        vars=$(ovmf_find VARS plain)
        [[ $OTHER_FW == 1 ]] && code=$(ovmf_find CODE secboot)
    else
        code=$(ovmf_find CODE secboot)
        vars=$MOK_VARS
        [[ $OTHER_FW == 1 ]] && code=$(ovmf_find CODE plain)
    fi
    cp "$vars" "$R/vars.fd"
    chmod u+w "$R/vars.fd"
    if [[ $fw == uefi-sb ]]; then
        "$VFV" --inplace "$R/vars.fd" \
            --add-db 5eedd15c-0000-4000-8000-00000000d00b "$MOK_CRT" \
            >/dev/null 2>&1
    fi
    if [[ ${ENTRY_JUMP:-0} == 1 ]]; then
        jump_entry_json "$disk" > "$R/vars.json"
        "$VFV" --inplace "$R/vars.fd" --set-json "$R/vars.json" >/dev/null 2>&1
    fi
    case $tpm in
    tpm2)    swtpm_start 2 "$TPM_DIR" ;;
    tpm2crb) swtpm_start 2crb "$TPM_DIR" ;;
    tpm12)   swtpm_start 1.2 "$TPM_DIR" ;;
    esac
    rm -f "$R/serial.log" "$R/mon.sock"
    if [[ $ARCH == aa64 ]]; then
        timeout "$(( TIMEOUT * SCALE ))" qemu-system-aarch64 -machine virt \
            -m 1024 -cpu max -display none -net none \
            -drive "if=pflash,format=$(fw_format "$code"),readonly=on,file=$code" \
            -drive "if=pflash,format=$(fw_format "$vars"),file=$R/vars.fd" \
            -device qemu-xhci -device usb-kbd -device ramfb \
            -drive "if=none,id=d0,format=raw,file=$disk" \
            -device virtio-blk-pci,drive=d0,addr=5,bootindex=0 \
            "${SWTPM_QEMU[@]}" "${RTC_ARGS[@]}" \
            -serial "file:$R/serial.log" \
            -monitor "unix:$R/mon.sock,server,nowait" >"$R/qemu.log" 2>&1 &
        QPID=$!
        return
    fi
    timeout "$TIMEOUT" qemu-system-x86_64 -machine q35 -m 1024 \
        -enable-kvm -cpu host -display none -net none \
        -drive "if=pflash,format=raw,readonly=on,file=$code" \
        -drive "if=pflash,format=raw,file=$R/vars.fd" \
        -drive "if=none,id=d0,format=raw,file=$disk" \
        -device ide-hd,drive=d0,bus=ide.0,bootindex=0 \
        "${SWTPM_QEMU[@]}" "${RTC_ARGS[@]}" \
        -serial "file:$R/serial.log" \
        -monitor "unix:$R/mon.sock,server,nowait" >"$R/qemu.log" 2>&1 &
    QPID=$!
}

halt() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
    swtpm_stop
}

FAILED=0
result() {      # result CASE WHY DETAIL
    if [[ -z "$2" ]]; then
        printf 'PASS  %-24s %s\n' "$1" "$3"
    else
        printf 'FAIL  %-24s %s\n' "$1" "$2"
        FAILED=1
    fi
    log_text "$R/serial.log" > "$R/$1.log" 2>/dev/null || true
}

skip() {        # skip CASE WHY
    printf 'SKIP  %-24s %s\n' "$1" "$2"
}

# ---- what the probe says ----

# The protocol a TPM version is reached through, as the probe names it.
proto_of() {
    case $1 in
    tpm12) echo TCG ;;
    *)     echo TCG2 ;;
    esac
}

# The digest a version's PCR0 is judged by: SHA-256 on 2.0, where the
# loader will read it, SHA-1 on 1.2, which has nothing else.
pcr0_line() {
    case $1 in
    tpm12) echo "FALCON-TPM TCG PCR0 sha1 " ;;
    *)     echo "FALCON-TPM TCG2 PCR0 sha256 " ;;
    esac
}

# Why a probe run fails, or nothing; "absent" when the firmware installs no
# protocol for the version, which callers turn into a SKIP.
judge_probe() {  # judge_probe TPM
    local p
    p=$(proto_of "$1")
    has "FALCON-TPM-PROBE-DONE" || { echo "the probe never finished"; return; }
    has "FALCON-TPM $p absent" && { echo absent; return; }
    has "FALCON-TPM $p present 01" || { echo "$p found, the TPM not present"; return; }
    log_text "$R/serial.log" | grep -aF -- "$(pcr0_line "$1")" |
        grep -aq ' measured$' || echo "PCR0 not read, or zero"
}

pcr0_of() {
    log_text "$R/serial.log" | grep -aF -- "$(pcr0_line "$1")" | head -n 1 |
        awk '{ print $5 }'
}

# ---- the cases ----

probe_efi() {   # the probe the firmware state needs
    if [[ $1 == uefi-sb ]]; then echo "$PROBE_SIGNED"; else echo "$PROBE"; fi
}

# Why a firmware state cannot be booted here, or nothing.
fw_missing() {
    [[ $1 == uefi ]] && return
    [[ -n $VFV ]] || { echo "virt-fw-vars not found (make mok-test makes a venv with it)"; return; }
    [[ -f $MOK_VARS && -f $MOK_CRT ]] || echo "$MOK_VARS missing (make mok-test)"
}

case_probe() {
    local c=$1 fw=$2 tpm=$3 why
    why=$(fw_missing "$fw")
    [[ -n $why ]] && { skip "$c" "$why"; return; }
    probe_disk "$R/probe.img" "$(probe_efi "$fw")"
    boot "$fw" "$tpm" "$R/probe.img"
    wait_for "FALCON-TPM-PROBE-DONE" 60 || true
    halt
    why=$(judge_probe "$tpm")
    if [[ $why == absent ]]; then
        skip "$c" "this OVMF installs no EFI_$(proto_of "$tpm")_PROTOCOL (set OVMF_DIR to a TPM-enabled build)"
        return
    fi
    result "$c" "$why" "$(proto_of "$tpm") present, PCR0 $(pcr0_of "$tpm")"
}

case_station() {
    local c=$1 fw=$2 tpm=$3 why=
    why=$(fw_missing "$fw")
    [[ -n $why ]] && { skip "$c" "$why"; return; }
    if [[ ! -f "$STATION" ]]; then
        skip "$c" "$STATION missing (make test-station)"
        return
    fi
    station_disk "$R/station.img" "$(probe_efi "$fw")"
    boot "$fw" "$tpm" "$R/station.img"
    wait_for "booting: $MEMTEST_LABEL" 90 || why="the station did not start its default"
    [[ -z "$why" ]] && { wait_for "FALCON-TPM-PROBE-DONE" 30 || why="the probe did not run"; }
    halt
    if [[ -z "$why" ]]; then
        why=$(judge_probe "$tpm")
        if [[ $why == absent ]]; then
            skip "$c" "this OVMF installs no EFI_$(proto_of "$tpm")_PROTOCOL (set OVMF_DIR to a TPM-enabled build)"
            return
        fi
    fi
    result "$c" "$why" "station started the probe; PCR0 $(pcr0_of "$tpm")"
}

# ---- the TOTP check ----

# The codes an authenticator holding secret (base32) shows over the first
# eight steps from epoch t, one per line as "123 456".
codes_for() {   # codes_for SECRET T
    python3 - "$1" "$2" <<'EOF'
import base64, hashlib, hmac, struct, sys
s = sys.argv[1]
key = base64.b32decode(s + "=" * (-len(s) % 8))
for k in range(8):
    h = hmac.new(key, struct.pack(">Q", int(sys.argv[2]) // 30 + k),
                 hashlib.sha1).digest()
    o = h[-1] & 15
    v = (struct.unpack(">I", h[o:o + 4])[0] & 0x7FFFFFFF) % 1000000
    print("%03d %03d" % (v // 1000, v % 1000))
EOF
}

# The seed's bytes as hex, from the secret.
seed_hex() {
    python3 -c 'import base64, sys; s = sys.argv[1]; print(base64.b32decode(s + "=" * (-len(s) % 8)).hex())' "$1"
}

# Does an swtpm log (level 5) carry these bytes, on the wire in either
# direction? Its dumps are rows of space-separated hex bytes; the rows of
# one exchange are joined before the search, so a run that crosses a row
# is still found. grep reads to the end rather than -q: under pipefail an
# awk cut short by an early exit would turn a hit into a miss.
wire_has() {    # wire_has LOG HEX
    [[ -f "$1" ]] || return 1
    awk '{ line = $0; gsub(/^[ \t]+/, "", line)
           if (line ~ /^([0-9a-fA-F][0-9a-fA-F]( |$))+$/) {
               gsub(/ /, "", line); buf = buf line
           } else { print tolower(buf); buf = "" } }
         END { print tolower(buf) }' "$1" | grep -- "$2" >/dev/null
}

# Steps 1 and 2 of the totp cases: the station's first boot saves the event
# log, and mkstick enrolls against the same TPM state. It fills the calling
# case's locals (c, fw and tpm read; ver, why, out, chk, secret, saved and
# owner set), and returns 1 when it has skipped the case.
totp_enrolled() {
    case $tpm in
    tpm12) ver=1.2 ;;
    *)     ver=2 ;;
    esac
    TPM_DIR=$R/$c
    rm -rf "$TPM_DIR"
    mkdir -p "$TPM_DIR/state"
    [[ $tpm == tpm12 ]] && swtpm_setup12 "$TPM_DIR"
    export SWTPM_KEEP=1
    RTC_ARGS=(-rtc "base=$RTC_BASE")
    # Three seconds: step 3b has to stop the countdown.
    INI_TIMEOUT=3 station_disk "$R/totp.img" "$(probe_efi "$fw")"

    # 1. the event log, as the station's boot writes it
    boot "$fw" "$tpm" "$R/totp.img"
    wait_for "FALCON-TPM-PROBE-DONE" 90 || why="the first boot did not reach the probe"
    halt
    log_text "$R/serial.log" > "$R/$c-1.log"
    if [[ -z $why ]] && has "FALCON-TPM $(proto_of "$tpm") absent"; then
        skip "$c" "this OVMF installs no EFI_$(proto_of "$tpm")_PROTOCOL"
        TPM_DIR=$R/swtpm; RTC_ARGS=(); unset SWTPM_KEEP
        return 1
    fi
    rm -f "$R/evlog.bin" "$R/evsave.bin"
    [[ -z $why ]] && { mcopy -i "$R/totp.img@@1M" ::/falcon-evlog.bin "$R/evlog.bin" 2>/dev/null ||
        why="the probe saved no event log"; }
    # The station saves the PCRs at its menu on every boot with a TPM, with
    # edk2's log on a 1.2 behind EFI_TCG_PROTOCOL (DESIGN Section 12.15);
    # enrollment is from that record.
    saved=()
    if [[ -z $why ]]; then
        if mcopy -i "$R/totp.img@@1M" ::/falcon-evsave.bin "$R/evsave.bin" 2>/dev/null; then
            saved=(--saved-log "$R/evsave.bin")
        else
            why="the station saved no record on a TPM $ver"
        fi
    fi
    # And says so in FalconEvLogStatus, which the probe prints.
    if [[ -z $why ]] && ! has "FALCON-TPM evsave-status saved: TPM"; then
        why="the station's status line does not say it saved the record"
    fi

    # 2. enrollment, on the host, against the same TPM state
    if [[ -z $why ]]; then
        SWTPM_LOG_LEVEL=5 swtpm_serve "$ver" "$TPM_DIR" || why="swtpm would not serve its state"
    fi
    # swtpm_setup's well-known owner secret is SHA-1 of 20 zero bytes, not
    # the bytes (swtpm_setup12), so the 1.2 enrollment is given it as a
    # 20-byte file, which --owner-auth-file takes as the secret itself.
    owner=()
    if [[ $tpm == tpm12 ]]; then
        python3 -c 'import hashlib, sys; sys.stdout.buffer.write(hashlib.sha1(bytes(20)).digest())' \
            > "$TPM_DIR/owner.bin"
        owner=(--owner-auth-file "$TPM_DIR/owner.bin")
    fi
    if [[ -z $why ]]; then
        out=$("$MKSTICK" station totp enroll "$R/totp.img" "${ARCH_OPT[@]}" --pcrs 0,2,4,7 \
                --label rig --tpm-socket "$TPM_DIR/server.sock" \
                --event-log "$R/evlog.bin" "${saved[@]}" "${owner[@]}" 2>&1) || why="enroll failed: $(printf '%s' "$out" | tail -n 1)"
        printf '%s\n' "$out" > "$R/$c-enroll.log"
        # The station's read from the host: this TPM's PCRs have moved past
        # the station's, so everything before them must check out.
        if [[ -z $why ]]; then
            chk=$("$MKSTICK" station totp check "$R/totp.img" "${ARCH_OPT[@]}" \
                    --tpm-socket "$TPM_DIR/server.sock" 2>&1) || true
            printf '%s\n' "$chk" > "$R/$c-check.log"
            grep -qF "the TPM refused on the PCRs" <<<"$chk" ||
                why="station totp check: $(printf '%s' "$chk" | tail -n 1)"
        fi
        [[ -z $why && $tpm == tpm12 ]] &&
            ! grep -qF "PCRs from the event log the station saved" <<<"$out" &&
            why="the 1.2 enrollment did not use the station's saved log"
        [[ -z $why && $tpm != tpm12 ]] &&
            ! grep -qF "PCRs the station read, matching the event log" <<<"$out" &&
            why="the 2.0 enrollment did not use the station's record"
        swtpm_serve_stop
        secret=$(printf '%s\n' "$out" | sed -n 's/^.*secret: *\([A-Z2-7]*\)$/\1/p' | head -n 1)
        [[ -z $why && -z $secret ]] && why="enroll printed no secret"
    fi
    return 0
}

case_totp() {
    local c=$1 fw=$2 tpm=$3 why= ver= out= secret= shown= want= hex= base= unseal= chk=
    local saved=() owner=()
    why=$(fw_missing "$fw")
    [[ -n $why ]] && { skip "$c" "$why"; return; }
    [[ -f "$STATION" ]] || { skip "$c" "$STATION missing (make test-station)"; return; }
    [[ -x "$MKSTICK" ]] || { skip "$c" "$MKSTICK missing (make cli)"; return; }
    command -v python3 >/dev/null || { skip "$c" "python3 is not installed"; return; }
    command -v swtpm_setup >/dev/null || { skip "$c" "swtpm_setup is not installed"; return; }

    totp_enrolled || return 0
    [[ -n $secret ]] && hex=$(seed_hex "$secret")

    # 3. the code, and the seed off the wire
    if [[ -z $why ]]; then
        SWTPM_LOG_LEVEL=5 boot "$fw" "$tpm" "$R/totp.img"
        wait_for "FALCON-TPM-PROBE-DONE" 120 || why="the second boot did not reach the probe"
        if [[ -z $why ]]; then
            wait_for "the image exited" 30 || why="the probe did not come back to the station"
        fi
        if [[ -z $why ]]; then
            mon "sendkey ret"
            wait_for "The TOTP code is shown again after a restart." 30 ||
                why="the code was still shown after the probe ran"
        fi
        halt
        log_text "$R/serial.log" > "$R/$c-3.log"
        cp "$TPM_DIR/swtpm.log" "$R/$c-wire.log" 2>/dev/null || : > "$R/$c-wire.log"
        # The unseal's own command, so a log that missed it cannot pass:
        # NV_Read on 2.0, ExecuteTransport around it on 1.2.
        if [[ $tpm == tpm12 ]]; then unseal=000000e7; else unseal=0000014e; fi
        shown=$(grep -aoE 'TOTP: [0-9]{3} [0-9]{3}' "$R/$c-3.log" | head -n 1 | cut -c7-)
        base=$(date -u -d "${RTC_BASE/T/ }" +%s)
        want=$(codes_for "$secret" "$base")
        if [[ -z $why && -z $shown ]]; then
            why="no code under the banner: $(grep -aoE 'TPM[^|]{0,70}|No TPM[^|]{0,20}' "$R/$c-3.log" | head -n 1)"
        elif [[ -z $why ]] && ! grep -qx -- "$shown" <<<"$want"; then
            why="the code $shown is not the seed's for the clock"
        elif [[ -z $why ]] && ! grep -aq "(falcon-station:rig)" "$R/$c-3.log"; then
            why="the code's label is missing"
        elif [[ -z $why ]] && ! wire_has "$R/$c-wire.log" "$unseal"; then
            why="swtpm's log does not hold the unseal, so the wire check saw nothing"
        elif [[ -z $why ]] && wire_has "$R/$c-wire.log" "$hex"; then
            why="the seed crossed the TPM's wire in the clear"
        elif [[ -z $why && $tpm == tpm12 ]] && ! wire_has "$TPM_DIR/serve.log" "$hex"; then
            why="the wire check cannot see the seed even in the plain enrollment write"
        fi
    fi

    # 3b. the code above the passphrase prompt: the countdown stopped, the
    # first row (the encrypted shelf) chosen
    if [[ -z $why ]]; then
        boot "$fw" "$tpm" "$R/totp.img"
        wait_for "TOTP: " 90 || why="the prompt boot showed no code"
        for _ in 1 2 3 4 5 6 7 8; do mon "sendkey up"; done
        mon "sendkey ret"
        [[ -z $why ]] && { wait_for "passphrase for the encrypted partition" 30 ||
            why="no passphrase prompt"; }
        # Past a 30-second step at the prompt: the code is redrawn in place.
        # An emulated guest's clock runs behind the host's, so SCALE steps.
        [[ -z $why ]] && sleep $(( 32 * SCALE ))
        halt
        log_text "$R/serial.log" > "$R/$c-3b.log"
        [[ -z $why ]] && { awk '/booting: Personal OS/ { p = 1 } p' "$R/$c-3b.log" |
            grep -aqE 'TOTP: [0-9]{3} [0-9]{3}' || why="no code above the passphrase prompt"; }
        [[ -z $why ]] && (( $(awk '/booting: Personal OS/ { p = 1 } p' "$R/$c-3b.log" |
            grep -aoE 'TOTP: [0-9]{3} [0-9]{3}' | sort -u | wc -l) >= 2 )) ||
            why="${why:-the code above the prompt was not redrawn when it rolled}"
    fi

    # 4. a changed PCR
    if [[ -z $why ]]; then
        OTHER_FW=1 boot "$fw" "$tpm" "$R/totp.img"
        wait_for "FALCON-TPM-PROBE-DONE" 120 || why="the third boot did not reach the probe"
        halt
        log_text "$R/serial.log" > "$R/$c-4.log"
        if [[ -z $why ]] && ! { grep -aqF "The TPM did not release the code:" "$R/$c-4.log" &&
                grep -aqF "firmware, its settings or the boot chain changed since enrollment." "$R/$c-4.log"; }; then
            why="no warning after PCR 0 moved: $(grep -aoE 'TOTP: [0-9 ]{7}|TPM[^|]{0,60}' "$R/$c-4.log" | head -n 1)"
        fi
    fi

    TPM_DIR=$R/swtpm
    RTC_ARGS=()
    unset SWTPM_KEEP
    result "$c" "$why" "code $shown matches the seed, off the wire, gone after a handover, above the prompt; PCR 0 moved: warned"
}

# The code shown on a boot of the enrolled totp disk; sets why on a miss.
jump_code() {   # jump_code STEP ENTRY_JUMP WANT-ROUTE
    local log=$R/$c-$1.log n
    ENTRY_JUMP=$2 boot "$fw" "$tpm" "$R/totp.img"
    wait_for "FALCON-TPM-PROBE-DONE" 120 || why="boot $1 did not reach the probe"
    halt
    log_text "$R/serial.log" > "$log"
    # Counted rather than grep -q: a pipe cut short fails under pipefail.
    n=$(grep -acF "starting Boot0001 \"$3" "$log" || true)
    [[ -z $why && $n == 0 ]] && why="boot $1 was not through $3"
    shown=$(grep -aoE 'TOTP: [0-9]{3} [0-9]{3}' "$log" | head -n 1 | cut -c7-)
    if [[ -z $why && -z $shown ]]; then
        why="boot $1 ($3) showed no code: $(grep -aoE 'The TPM did not release[^|]{0,60}|TPM[^|]{0,70}' "$log" | head -n 1)"
    elif [[ -z $why ]] && ! grep -qx -- "$shown" <<<"$want"; then
        why="boot $1 ($3): the code $shown is not the seed's for the clock"
    fi
}

# The station without shim (JUMP=1): enrolled from a boot through the
# `falcon` entry, which names the jumper; then the code on a boot through
# that entry and on one with no entry, where the firmware's removable-media
# path is the same jumper. PCR 4 measures the jumper and then the loader on
# both, so both unseal (DESIGN Section 12.15).
case_totp_jump() {
    local c=$1 fw=$2 tpm=$3 why= ver= out= secret= chk= shown= want= base= s2= n=
    local saved=() owner=()
    why=$(fw_missing "$fw")
    [[ -n $why ]] && { skip "$c" "$why"; return; }
    [[ -f "$STATION" ]] || { skip "$c" "$STATION missing (make test-station)"; return; }
    [[ -x "$MKSTICK" ]] || { skip "$c" "$MKSTICK missing (make cli)"; return; }
    [[ -f "$JUMPER" ]] || { skip "$c" "$JUMPER missing (make station sign)"; return; }
    [[ -n "$VFV" ]] || { skip "$c" "virt-fw-vars is not installed"; return; }
    command -v python3 >/dev/null || { skip "$c" "python3 is not installed"; return; }
    command -v swtpm_setup >/dev/null || { skip "$c" "swtpm_setup is not installed"; return; }

    export JUMP=1 ENTRY_JUMP=1
    totp_enrolled || { unset JUMP ENTRY_JUMP; return 0; }
    unset ENTRY_JUMP
    n=$(grep -acF 'starting Boot0001 "falcon' "$R/$c-1.log" || true)
    [[ -z $why && $n == 0 ]] && why="the enrolled boot was not through the falcon entry"
    base=$(date -u -d "${RTC_BASE/T/ }" +%s)
    want=$(codes_for "$secret" "$base")
    [[ -z $why ]] && jump_code 2 1 "falcon"
    s2=$shown
    [[ -z $why ]] && jump_code 3 0 "$DISK_BOOT_NAME"
    unset JUMP

    TPM_DIR=$R/swtpm
    RTC_ARGS=()
    unset SWTPM_KEEP
    result "$c" "$why" "enrolled after a boot through the jumper; code $s2 through the falcon entry, $shown with no entry"
}

# A station loader whose Authenticode digest is not the build's: one byte
# of a section's slack (past its VirtualSize, inside SizeOfRawData) is
# hashed and never loaded, so the loader runs as before.
other_loader() { # other_loader IN OUT
    python3 - "$1" "$2" <<'EOF'
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
pe = struct.unpack_from("<I", d, 0x3C)[0]
n = struct.unpack_from("<H", d, pe + 6)[0]
sh = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0]
for i in range(n):
    vs, va, rs, rp = struct.unpack_from("<IIII", d, sh + 40 * i + 8)
    if rp and rs > vs:
        d[rp + vs] ^= 0x5A
        open(sys.argv[2], "wb").write(d)
        sys.exit(0)
sys.exit(1)
EOF
}

case_totp_update() {
    local c=$1 fw=$2 tpm=$3 why= ver= out= secret= chk= shown= want= base= new=
    local saved=() owner=() f
    why=$(fw_missing "$fw")
    [[ -n $why ]] && { skip "$c" "$why"; return; }
    [[ -f "$STATION" ]] || { skip "$c" "$STATION missing (make test-station)"; return; }
    [[ -x "$MKSTICK" ]] || { skip "$c" "$MKSTICK missing (make cli)"; return; }
    for f in falcon-boot-station.efi falcon-bios-stage1.bin \
             falcon-bios-stage2.bin falcon-bios-station-stage3.bin; do
        [[ -f build/bin/$f ]] || { skip "$c" "build/bin/$f missing (make station)"; return; }
    done
    command -v python3 >/dev/null || { skip "$c" "python3 is not installed"; return; }
    command -v swtpm_setup >/dev/null || { skip "$c" "swtpm_setup is not installed"; return; }
    command -v sbsign >/dev/null || { skip "$c" "sbsign is not installed"; return; }

    totp_enrolled || return 0

    # 3. the update, from the old loader's log. The station disk has the
    # BIOS station too, so its stages come along.
    if [[ -z $why ]]; then
        rm -rf "$R/upd-from" "$R/upd-backup"
        mkdir -p "$R/upd-from" "$R/upd-backup"
        cp build/bin/falcon-bios-stage1.bin build/bin/falcon-bios-stage2.bin \
           build/bin/falcon-bios-station-stage3.bin "$R/upd-from/"
        other_loader "build/bin/falcon-boot-station$SUFFIX.efi" \
            "$R/upd-from/falcon-boot-station$SUFFIX.efi" ||
            why="the station loader has no section slack to change"
    fi
    if [[ -z $why ]]; then
        swtpm_serve "$ver" "$TPM_DIR" || why="swtpm would not serve its state"
    fi
    if [[ -z $why ]]; then
        out=$("$MKSTICK" update "$R/totp.img" "${ARCH_OPT[@]}" --from "$R/upd-from" \
                --key build/mok-test/falcon-test-mok.key --cert "$MOK_CRT" \
                --backup-dir "$R/upd-backup" \
                --tpm-socket "$TPM_DIR/server.sock" --event-log "$R/evlog.bin" \
                "${saved[@]}" "${owner[@]}" --yes 2>&1) ||
            why="update failed: $(printf '%s' "$out" | tail -n 1)"
        printf '%s\n' "$out" > "$R/$c-update.log"
        swtpm_serve_stop
        grep -qF "was not resealed" <<<"$out" &&
            why="${why:-update did not reseal: $(grep -F "was not resealed" <<<"$out")}"
        new=$(printf '%s\n' "$out" | sed -n 's/^.*secret: *\([A-Z2-7]*\)$/\1/p' | head -n 1)
        [[ -z $why && -z $new ]] && why="update printed no new secret"
        [[ -z $why && $new == "$secret" ]] && why="the new secret is the old one"
    fi
    # The rig boots the removable path, whose copy of the loader
    # station_disk keeps beside \EFI\falcon's; a machine boots the
    # firmware's entry for \EFI\falcon.
    if [[ -z $why ]]; then
        rm -f "$R/upd-loader.efi"
        mcopy -n -i "$R/totp.img@@1M" "::/EFI/falcon/grub$LOW.efi" "$R/upd-loader.efi"
        cmp -s "$R/upd-loader.efi" "$LOADER" && why="the loader on the ESP is the old one"
        mcopy -o -i "$R/totp.img@@1M" "$R/upd-loader.efi" "::/EFI/BOOT/grub$LOW.efi"
    fi

    # 4. the new loader shows the new secret's code
    if [[ -z $why ]]; then
        boot "$fw" "$tpm" "$R/totp.img"
        wait_for "FALCON-TPM-PROBE-DONE" 120 || why="the boot after the update did not reach the probe"
        halt
        log_text "$R/serial.log" > "$R/$c-4.log"
        shown=$(grep -aoE 'TOTP: [0-9]{3} [0-9]{3}' "$R/$c-4.log" | head -n 1 | cut -c7-)
        base=$(date -u -d "${RTC_BASE/T/ }" +%s)
        want=$(codes_for "$new" "$base")
        if [[ -z $why && -z $shown ]]; then
            why="no code after the update: $(grep -aoE 'The TPM did not release[^|]{0,60}|TPM[^|]{0,70}' "$R/$c-4.log" | head -n 1)"
        elif [[ -z $why ]] && ! grep -qx -- "$shown" <<<"$want"; then
            why="the code $shown is not the new seed's for the clock"
        fi
    fi

    TPM_DIR=$R/swtpm
    RTC_ARGS=()
    unset SWTPM_KEEP
    result "$c" "$why" "enrolled, updated to another loader and resealed from the old boot's log: code $shown is the new seed's"
}

# ---- run ----

ALL=(uefi-tpm2-probe uefi-tpm2crb-probe uefi-tpm12-probe
     uefi-sb-tpm2-probe uefi-sb-tpm12-probe
     uefi-tpm2-station uefi-tpm12-station
     uefi-sb-tpm2-station uefi-sb-tpm12-station
     uefi-tpm2-totp uefi-tpm12-totp uefi-sb-tpm2-totp uefi-sb-tpm12-totp
     uefi-tpm2-totp-update uefi-tpm12-totp-update
     uefi-sb-tpm2-totp-update uefi-sb-tpm12-totp-update
     uefi-tpm2-totp-jump uefi-tpm12-totp-jump
     uefi-sb-tpm2-totp-jump uefi-sb-tpm12-totp-jump)
CASES=("$@")
[[ ${#CASES[@]} -eq 0 ]] && CASES=("${ALL[@]}")

# AArch64: the probe, the station disk and images built for it, AAVMF, the
# TPM on virt's sysbus. virt has no CRB or 1.2 device, so only tpm2 cases.
set_arch() {    # set_arch x64|aa64
    [[ $1 == "$ARCH" ]] && return
    ARCH=$1
    if [[ $ARCH == aa64 ]]; then
        UP=AA64 LOW=aa64 SCALE=${AA64_SCALE:-6}
        SUFFIX=-aa64 ARCH_OPT=(--arch aa64)
        MEMTEST_LABEL="Memory test (MemTest86)"
        DISK_BOOT_NAME="UEFI Misc Device"
        PROBE=build/test/tpm-probe-aa64.efi
        PROBE_SIGNED=build/test/tpm-probe-aa64-signed.efi
        STATION=build/test-station-aa64.img
        LOADER=build/bin/falcon-boot-station-aa64-signed.efi
        JUMPER=build/bin/falcon-esp-jump-aa64-signed.efi
        MOK_VARS=build/mok-test/AAVMF_VARS.secboot.mok.fd
        export SWTPM_TIS_DEV=tpm-tis-device
        make --no-print-directory "$PROBE" "$PROBE_SIGNED" "$LOADER" \
            "$JUMPER" >/dev/null
        [[ -f $STATION ]] ||
            { echo "ERROR: $STATION missing (make test-station-aa64)" >&2; exit 1; }
    else
        UP=X64 LOW=x64 SCALE=1
        SUFFIX= ARCH_OPT=()
        MEMTEST_LABEL="Memory test (memtest86+)"
        DISK_BOOT_NAME="UEFI QEMU HARDDISK"
        PROBE=build/test/tpm-probe.efi
        PROBE_SIGNED=build/test/tpm-probe-signed.efi
        STATION=build/test-station.img
        LOADER=build/bin/falcon-boot-station-signed.efi
        JUMPER=build/bin/falcon-esp-jump-signed.efi
        MOK_VARS=build/mok-test/OVMF_VARS.secboot.mok.fd
        unset SWTPM_TIS_DEV
    fi
}

for c in "${CASES[@]}"; do
    case $c in
    aa64-*) set_arch aa64; c=${c#aa64-}; label=aa64-$c ;;
    *)      set_arch x64; label=$c ;;
    esac
    case $c in
    uefi-sb-*) fw=uefi-sb; rest=${c#uefi-sb-} ;;
    uefi-*)    fw=uefi;    rest=${c#uefi-} ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
    tpm=${rest%%-*}
    name=${rest#*-}
    case $tpm in
    tpm2|tpm2crb|tpm12) ;;
    *) echo "unknown TPM in case: $c" >&2; exit 2 ;;
    esac
    case $name in
    probe)   case_probe "$label" "$fw" "$tpm" ;;
    station) case_station "$label" "$fw" "$tpm" ;;
    totp)    case_totp "$label" "$fw" "$tpm" ;;
    totp-update) case_totp_update "$label" "$fw" "$tpm" ;;
    totp-jump) case_totp_jump "$label" "$fw" "$tpm" ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
done
exit "$FAILED"
