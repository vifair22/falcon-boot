#!/usr/bin/env bash
# falcon-boot: the TPM rig (docs/notes/argon2-totp.md Section 2), each case
# booted headless under OVMF with a software TPM (swtpm, through
# falcon-efi/tools/swtpm.sh) and judged from serial, with Secure Boot off
# (uefi-*) and on (uefi-sb-*, the test MOK enrolled and its cert in db).
#
# What the firmware here does, measured 2026-09-28 with the probe below
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
#             with a TPM attached and what it starts still sees one. This
#             is the shape the loader's own TPM cases take once that code
#             exists (the TOTP under the banner, the warning after a
#             changed PCR, ciphertext on the wire).
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
# Env:   SWTPM_LOG_LEVEL  swtpm's log level into build/tpm-rig/swtpm/ (0)
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
MOK_VARS=build/mok-test/OVMF_VARS.secboot.mok.fd
MOK_CRT=build/mok-test/falcon-test-mok.crt
TIMEOUT=180

# virt-fw-vars from PATH, else the venv mk-test-mok.sh makes when PATH has
# none; only the Secure Boot cases need it.
VFV=$(command -v virt-fw-vars || true)
[[ -z $VFV && -x build/shim/venv/bin/virt-fw-vars ]] &&
    VFV=build/shim/venv/bin/virt-fw-vars

QPID=
cleanup() {
    [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null || true
    swtpm_stop
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
    mcopy -i "$img@@1M" "$efi" ::/EFI/BOOT/BOOTX64.EFI
}

# The station disk, copied, with the loader as built now and the probe as
# memtest.efi, which STATION.INI makes the default.
station_disk() {  # station_disk IMAGE EFI
    local img=$1 efi=$2 dir
    cp --reflink=auto "$STATION" "$img"
    for dir in falcon BOOT; do
        mcopy -o -i "$img@@1M" "$LOADER" ::/EFI/$dir/grubx64.efi
    done
    mcopy -o -i "$img@@1M" "$efi" ::/EFI/falcon/memtest.efi
    printf '[general]\ndefault = memtest\ntimeout = 1\n' > "$R/STATION.INI"
    mcopy -o -i "$img@@1M" "$R/STATION.INI" ::/EFI/falcon/STATION.INI
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
    if [[ $fw == uefi ]]; then
        code=$(ovmf_find CODE plain)
        vars=$(ovmf_find VARS plain)
    else
        code=$(ovmf_find CODE secboot)
        vars=$MOK_VARS
    fi
    cp "$vars" "$R/vars.fd"
    chmod u+w "$R/vars.fd"
    if [[ $fw == uefi-sb ]]; then
        "$VFV" --inplace "$R/vars.fd" \
            --add-db 5eedd15c-0000-4000-8000-00000000d00b "$MOK_CRT" \
            >/dev/null 2>&1
    fi
    case $tpm in
    tpm2)    swtpm_start 2 "$R/swtpm" ;;
    tpm2crb) swtpm_start 2crb "$R/swtpm" ;;
    tpm12)   swtpm_start 1.2 "$R/swtpm" ;;
    esac
    rm -f "$R/serial.log" "$R/mon.sock"
    timeout "$TIMEOUT" qemu-system-x86_64 -machine q35 -m 1024 \
        -enable-kvm -cpu host -display none -net none \
        -drive "if=pflash,format=raw,readonly=on,file=$code" \
        -drive "if=pflash,format=raw,file=$R/vars.fd" \
        -drive "if=none,id=d0,format=raw,file=$disk" \
        -device ide-hd,drive=d0,bus=ide.0,bootindex=0 \
        "${SWTPM_QEMU[@]}" \
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
    wait_for "booting: Memory test (memtest86+)" 90 || why="the station did not start its default"
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

# ---- run ----

ALL=(uefi-tpm2-probe uefi-tpm2crb-probe uefi-tpm12-probe
     uefi-sb-tpm2-probe uefi-sb-tpm12-probe
     uefi-tpm2-station uefi-tpm12-station
     uefi-sb-tpm2-station uefi-sb-tpm12-station)
CASES=("$@")
[[ ${#CASES[@]} -eq 0 ]] && CASES=("${ALL[@]}")

for c in "${CASES[@]}"; do
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
    probe)   case_probe "$c" "$fw" "$tpm" ;;
    station) case_station "$c" "$fw" "$tpm" ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
done
exit "$FAILED"
