#!/usr/bin/env bash
# falcon-boot: the UEFI station's rig (docs/DESIGN.md Sections 12.3-12.8),
# each case booted headless under OVMF and judged from serial, with Secure
# Boot off (uefi-*) and on (uefi-sb-*, the test MOK enrolled, and its cert
# in db standing in for Microsoft's, so the firmware starts the fake
# Windows Boot Manager a firmware entry names).
#
# Two disks. The station disk is mk-test-station.sh's (make test-station):
# its ESP carries the station loader under shim, memtest.efi, the boot
# record, and the STATION.INI each case writes into it with mtools; its
# shelf the fixture UKI. The second is a fake Windows disk, GPT with a
# fixed DiskGUID, whose ESP carries \EFI\Microsoft\Boot\bootmgfw.efi: the
# stand-in (falcon-efi/tools/payload/standin.c), which prints the path it
# was started as. The varstore gets a "Windows Boot Manager" Boot0009
# naming that file on that partition unless a case says otherwise.
#
#   menu         no STATION.INI: the list has the Personal OS, the firmware
#                entry and memtest, and no ESP row for a Windows the
#                firmware entry already names; Enter on the first row
#                prompts, and the fixture UKI reports the passphrase
#                credential and falcon.ident.cred naming the install
#   countdown    default = memtest, timeout = 3: the key line counts, and
#                memtest's stand-in starts with no key pressed (MEM)
#   key-stops    the same with a key pressed during the count: nothing
#                starts, and the key line is the ordinary hint again
#   nxt          default = the firmware entry: BootNext is set and the
#                machine restarts into the fake bootmgfw.efi (NXT)
#   method-efi   [win] disk = gpt:<fake disk>, method = efi: the fake
#                bootmgfw.efi is loaded by device path, after the
#                BitLocker line (ESP)
#   esp-found    no Boot0009: the fake disk's ESP is a row of its own,
#                "Windows Boot Manager (disk gpt:5eedd15c, partition 1)",
#                and boots (ESP)
#   next-boot    FalconNextBoot = memtest: it starts without the menu, and
#                the variable is gone afterwards
#   next-uki     FalconNextBoot = uki-personal, a UKI on the encrypted
#                shelf: the prompt with no menu, then the UKI
#   next-unknown FalconNextBoot names nothing: the sentence, then the menu
#   ini-error    an unknown key: the error page names the line, and the
#                menu waits with no countdown
#   no-default   default names an entry this machine does not have
#   unsigned     memtest.efi replaced by the unsigned stand-in: it starts
#                with Secure Boot off, and with it on the load is refused
#                and the page says why
#   idle-menu    poweroff = 5 and no default: the menu left alone, the
#                machine powers off (QEMU exits), and not before the time
#   idle-countdown  timeout = 8, poweroff = 3: a countdown longer than the
#                idle time still boots its default
#   idle-prompt  poweroff = 5: the passphrase prompt left with three
#                characters typed, and the machine powers off
#   bad-pass     five wrong passphrases: no wait after the first, 4, 8 and
#                16 s after the next three, then the power-off (DESIGN
#                Section 5.2)
#   retry        a wrong passphrase, no wait, then the right one: the UKI
#   timing       default = linux ([linux] uki = personal.efi), timeout = 3:
#                the countdown runs out, 2 s at the prompt, the UKI. The
#                boot-timing variables (DESIGN Section 5.5) are judged as
#                run-core-rig.sh's timing cases judge them, the wait at
#                least the countdown and the prompt's 2 s
#                (falcon-efi/tools/bli-judge.sh)
#
# Nothing touches a real disk or this machine's variables: every disk is an
# image file and every varstore a copy under build/station-rig.
#
# The disk is made once (make test-station, which needs sudo); each case
# copies it, puts the loader as built now onto the copy's ESP, and writes
# its STATION.INI there, so only the copy is ever booted.
#
# Usage: falcon-efi/tools/run-station-rig.sh [case...]
#        (default: every case, uefi- and uefi-sb-)
# Env:   FALCON_TEST_PASS  the station's passphrase (default falcon-test)
#        KEEP=1            reuse the fake Windows disk from an earlier run
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. falcon-efi/tools/ovmf.sh
. falcon-efi/tools/bli-judge.sh

R=build/station-rig
PASS="${FALCON_TEST_PASS:-falcon-test}"
MKSTICK=build/bin/falcon-boot-mkstick
STATION=build/test-station.img
STANDIN=build/test/standin-signed.efi
LOADER=build/bin/falcon-boot-station-signed.efi
MOK_CRT=build/mok-test/falcon-test-mok.crt
TIMEOUT=240

# The fake Windows disk: its DiskGUID, its ESP's partition GUID.
WIN_DISK=5EEDD15C-0000-4000-8000-000000000001
WIN_PART=5EEDD15C-0000-4000-8000-0000000000E1
WIN_SHORT=gpt:5eedd15c
WIN_NAME=gpt-5eedd15c-p1
BOOTMGFW='\EFI\Microsoft\Boot\bootmgfw.efi'
FALCON_GUID=50614485-081b-4b7c-8b54-2abeffdf2fb3
GLOBAL_GUID=8be4df61-93ca-11d2-aa0d-00e098032b8c

mkdir -p "$R"
QPID=

cleanup() {
    [[ -n "$QPID" ]] && kill "$QPID" 2>/dev/null || true
}
trap cleanup EXIT

# ---- images ----

build_win_disk() {
    local img=$R/win.img

    [[ "${KEEP:-0}" == 1 && -f "$img" ]] && return
    rm -f "$img"
    truncate -s 64M "$img"
    sgdisk -U "$WIN_DISK" -n 1:2048:+40M -t 1:EF00 -u "1:$WIN_PART" \
        -c 1:EFI -n 2:0:0 -t 2:0700 -c "2:Basic data" "$img" >/dev/null
    mkfs.fat -F 32 --offset 2048 -n WINESP "$img" $(( 40 * 1024 )) >/dev/null 2>&1
    mmd -i "$img@@1M" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot
    mcopy -i "$img@@1M" "$STANDIN" ::/EFI/Microsoft/Boot/bootmgfw.efi
}

# The partition's size in sectors, for Boot0009's HARDDRIVE node.
win_part_size() {
    sgdisk -i 1 "$R/win.img" | awk '/^Partition size:/ { print $3 }'
}

# vars JSON for virt-fw-vars: Boot0009 (unless NO_WIN_ENTRY), BootOrder,
# and FalconNextBoot when NEXT is set.
vars_json() {
    python3 - "$WIN_PART" "$(win_part_size)" "$BOOTMGFW" "${NO_WIN_ENTRY:-0}" \
        "${NEXT:-}" "$FALCON_GUID" "$GLOBAL_GUID" <<'EOF'
import json, struct, sys, uuid
part, size, path, no_entry, nxt, fguid, gguid = sys.argv[1:8]
v = []
if no_entry != "1":
    desc = "Windows Boot Manager\0".encode("utf-16-le")
    hd = struct.pack("<BBHIQQ", 4, 1, 42, 1, 2048, int(size))
    hd += uuid.UUID(part).bytes_le + bytes([2, 2])
    name = (path + "\0").encode("utf-16-le")
    fp = struct.pack("<BBH", 4, 4, 4 + len(name)) + name
    end = bytes([0x7F, 0xFF, 4, 0])
    # The whole path, as the firmware names the disk: OVMF drops a boot
    # option that matches no disk QEMU's bootorder lists, and matches by
    # this prefix. PciRoot(0)/Pci(0x1f,0x2)/Sata(1,0xFFFF,0), the second
    # AHCI port.
    pre = struct.pack("<BBHII", 2, 1, 12, 0x0A0341D0, 0)
    pre += struct.pack("<BBHBB", 1, 1, 6, 2, 0x1F)
    pre += struct.pack("<BBHHHH", 3, 0x12, 10, 1, 0xFFFF, 0)
    dp = pre + hd + fp + end
    opt = struct.pack("<IH", 1, len(dp)) + desc + dp
    v.append({"name": "Boot0009", "guid": gguid, "attr": 7,
              "data": opt.hex()})
    v.append({"name": "BootOrder", "guid": gguid, "attr": 7,
              "data": struct.pack("<H", 9).hex()})
if nxt:
    v.append({"name": "FalconNextBoot", "guid": fguid, "attr": 7,
              "data": (nxt + "\0").encode().hex()})
print(json.dumps({"version": 2, "variables": v}))
EOF
}

# STATION.INI into the station ESP's \EFI\falcon\, or none.
station_image() {  # station_image INI_TEXT
    local dir
    cp --reflink=auto "$STATION" "$R/station.img"
    # The loader as built now, so a disk made before the last change to it
    # still tests that change.
    for dir in falcon BOOT; do
        mcopy -o -i "$R/station.img@@1M" "$LOADER" ::/EFI/$dir/grubx64.efi
    done
    if [[ -n "$1" ]]; then
        printf '%s' "$1" > "$R/STATION.INI"
        mcopy -o -i "$R/station.img@@1M" "$R/STATION.INI" \
            ::/EFI/falcon/STATION.INI
    fi
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

# QEMU exits when the guest powers off; a reset would bring the firmware
# back instead.
wait_exit() {   # wait_exit SECONDS
    local i
    for (( i = 0; i < $1 * 10; i++ )); do
        kill -0 "$QPID" 2>/dev/null || return 0
        sleep 0.1
    done
    return 1
}
count() { log_text "$R/serial.log" | grep -aoF -- "$1" | wc -l; }

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

# boot FIRMWARE: the station disk first, the Windows disk second. OVMF
# under KVM on a loaded host now and then stops before its boot manager
# says anything (seen in this rig and in run-core-rig.sh alike, with the
# same inputs passing on the next run); a machine that has not reached
# BdsDxe in a minute is started again once, from a fresh varstore, and
# says so. Nothing of the loader has run by then.
boot() {
    boot_once "$1"
    if ! wait_for "BdsDxe: " 60; then
        halt
        echo "note: $1: the firmware stopped before its boot manager; started again" >&2
        boot_once "$1"
    fi
}

boot_once() {
    local fw=$1 code vars
    if [[ $fw == uefi ]]; then
        code=$(ovmf_find CODE plain)
        vars=$(ovmf_find VARS plain)
    else
        code=$(ovmf_find CODE secboot)
        vars=build/mok-test/OVMF_VARS.secboot.mok.fd
    fi
    cp "$vars" "$R/vars.fd"
    chmod u+w "$R/vars.fd"
    vars_json > "$R/vars.json"
    virt-fw-vars --inplace "$R/vars.fd" --set-json "$R/vars.json" \
        >/dev/null 2>&1
    if [[ $fw == uefi-sb ]]; then
        virt-fw-vars --inplace "$R/vars.fd" \
            --add-db 5eedd15c-0000-4000-8000-00000000d00b "$MOK_CRT" \
            >/dev/null 2>&1
    fi
    rm -f "$R/serial.log" "$R/mon.sock"
    timeout "$TIMEOUT" qemu-system-x86_64 -machine q35 -m 1024 \
        -enable-kvm -cpu host -display none -net none \
        -drive "if=pflash,format=raw,readonly=on,file=$code" \
        -drive "if=pflash,format=raw,file=$R/vars.fd" \
        -drive "if=none,id=d0,format=raw,file=$R/station.img" \
        -device ide-hd,drive=d0,bus=ide.0,bootindex=0 \
        -drive "if=none,id=d1,format=raw,file=$R/win.img" \
        -device ide-hd,drive=d1,bus=ide.1,bootindex=1 \
        -serial "file:$R/serial.log" \
        -monitor "unix:$R/mon.sock,server,nowait" >"$R/qemu.log" 2>&1 &
    QPID=$!
}

halt() {
    mon quit
    wait "$QPID" 2>/dev/null || true
    QPID=
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

MENU_UP="up/down choose"

# ---- the cases ----

case_menu() {
    local c=$1 fw=$2 why= id
    id=$("$MKSTICK" ident "$STATION" | awk -F '\t' '$2 == "boot" { print $3; exit }')
    station_image ""
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "$MENU_UP" 90 || why="no menu"
    sleep 1
    [[ -z "$why" ]] && { has "Personal OS (encrypted)" || why="no Personal OS row"; }
    [[ -z "$why" ]] && { has "Windows Boot Manager" || why="no firmware entry row"; }
    [[ -z "$why" ]] && { has "Memory test (memtest86+)" || why="no memtest row"; }
    [[ -z "$why" ]] && has "(disk $WIN_SHORT" && why="an ESP row for a Windows its entry names"
    [[ -z "$why" ]] && has "Booting in" && why="a countdown with no STATION.INI"
    if [[ -z "$why" ]]; then
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 30 || why="no passphrase prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys "$PASS"
        wait_for "FALCON-UKI-DONE" 120 || why="the UKI never reported"
    fi
    if [[ -z "$why" ]]; then
        has "FALCON-CRED-OK" || why="the passphrase credential is wrong"
        has "FALCON-CRED-HEAD falcon.ident.cred [falcon 1 boot $id" ||
            why="${why:-falcon.ident.cred missing or not install $id}"
    fi
    halt
    result "$c" "$why" "rows listed, no duplicate ESP row; UKI got both credentials"
}

case_countdown() {
    local c=$1 fw=$2 why=
    station_image $'[general]\ndefault = memtest\ntimeout = 3\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "Booting in 3 s" 90 || why="no countdown"
    [[ -z "$why" ]] && { wait_for "FALCON-STANDIN \\EFI\\falcon\\memtest.efi" 30 ||
        why="memtest did not start"; }
    [[ -z "$why" ]] && { has "Booting in 1 s" || why="the count did not reach 1"; }
    [[ -z "$why" ]] && { has "booting: Memory test (memtest86+)" || why="no route page"; }
    [[ -z "$why" ]] && { wait_for "the image exited, status 0x0" 15 || why="no exit page"; }
    halt
    result "$c" "$why" "counted 3 to 1, then memtest's stand-in ran"
}

case_key_stops() {
    local c=$1 fw=$2 why= n
    station_image $'[general]\ndefault = memtest\ntimeout = 4\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "Booting in 4 s" 90 || why="no countdown"
    if [[ -z "$why" ]]; then
        n=$(count "$MENU_UP")
        mon "sendkey down"
        sleep 7
        has "FALCON-STANDIN" && why="the default started after a key"
        (( $(count "$MENU_UP") > n )) || why="${why:-the key line was not restored}"
    fi
    halt
    result "$c" "$why" "a key stopped the count; nothing started in 7 s"
}

case_nxt() {
    local c=$1 fw=$2 why=
    station_image $'[general]\ndefault = nxt-0009\ntimeout = 1\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "booting: Windows Boot Manager" 90 || why="the entry was not chosen"
    [[ -z "$why" ]] && { has "restarting into it" || why="no NXT page"; }
    [[ -z "$why" ]] && { wait_for "FALCON-STANDIN $BOOTMGFW" 90 ||
        why="the firmware did not start the entry after the reset"; }
    halt
    result "$c" "$why" "BootNext set; after the reset the firmware started $BOOTMGFW"
}

case_method_efi() {
    local c=$1 fw=$2 why=
    station_image "$(printf '[general]\ndefault = win\ntimeout = 1\n\n[win]\ndisk = %s\nmethod = efi\n' \
        "gpt:$WIN_DISK")"
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "booting: Windows Boot Manager (disk $WIN_SHORT)" 90 || why="no ESP route page"
    [[ -z "$why" ]] && { has "BitLocker will ask for the recovery key" || why="no BitLocker line"; }
    [[ -z "$why" ]] && { wait_for "FALCON-STANDIN $BOOTMGFW" 30 || why="bootmgfw.efi did not start"; }
    [[ -z "$why" ]] && { wait_for "the image exited, status 0x0" 15 || why="no exit page"; }
    [[ -z "$why" ]] && has "restarting into it" && why="it went through BootNext"
    halt
    result "$c" "$why" "loaded $BOOTMGFW by device path"
}

case_esp_found() {
    local c=$1 fw=$2 why=
    station_image "$(printf '[general]\ndefault = %s\ntimeout = 2\n' "$WIN_NAME")"
    NEXT= NO_WIN_ENTRY=1 boot "$fw"
    wait_for "Windows Boot Manager (disk $WIN_SHORT, partition 1)" 90 ||
        why="no row for the ESP"
    [[ -z "$why" ]] && { wait_for "FALCON-STANDIN $BOOTMGFW" 30 || why="bootmgfw.efi did not start"; }
    halt
    result "$c" "$why" "the ESP with no entry is a row, named $WIN_NAME"
}

case_next_boot() {
    local c=$1 fw=$2 why=
    station_image ""
    NEXT=memtest NO_WIN_ENTRY=0 boot "$fw"
    wait_for "FALCON-STANDIN \\EFI\\falcon\\memtest.efi" 90 || why="memtest did not start"
    [[ -z "$why" ]] && has "$MENU_UP" && why="the menu was drawn first"
    halt
    if [[ -z "$why" ]] && virt-fw-vars -i "$R/vars.fd" -p 2>/dev/null |
            grep -q FalconNextBoot; then
        why="FalconNextBoot is still set"
    fi
    result "$c" "$why" "memtest without the menu; FalconNextBoot deleted"
}

case_next_uki() {
    local c=$1 fw=$2 why=
    station_image ""
    NEXT=uki-personal NO_WIN_ENTRY=0 boot "$fw"
    wait_for "passphrase for the encrypted partition" 90 || why="no passphrase prompt"
    [[ -z "$why" ]] && has "$MENU_UP" && why="the menu was drawn first"
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys "$PASS"
        wait_for "FALCON-UKI-DONE" 120 || why="the UKI never reported"
        has "FALCON-CRED-OK" || why="${why:-the passphrase credential is wrong}"
    fi
    halt
    result "$c" "$why" "prompt without the menu, then personal.efi with its credential"
}

case_next_unknown() {
    local c=$1 fw=$2 why=
    station_image ""
    NEXT=nxt-00ff NO_WIN_ENTRY=0 boot "$fw"
    wait_for "the next-boot marker names nxt-00ff, and no entry has that name." 90 ||
        why="no sentence"
    if [[ -z "$why" ]]; then
        sleep 0.5
        mon "sendkey spc"
        wait_for "$MENU_UP" 15 || why="the menu did not follow"
    fi
    halt
    result "$c" "$why" "said so, then the menu"
}

case_ini_error() {
    local c=$1 fw=$2 why=
    station_image $'[general]\nbogus = 1\ndefault = memtest\ntimeout = 2\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "STATION.INI line 2: unknown key 'bogus'." 90 || why="no error page"
    [[ -z "$why" ]] && { has "The menu will wait with no default." || why="no consequence line"; }
    if [[ -z "$why" ]]; then
        sleep 0.5
        mon "sendkey spc"
        wait_for "$MENU_UP" 15 || why="the menu did not follow"
        sleep 4
        has "Booting in" && why="a countdown after an error"
        has "FALCON-STANDIN" && why="the default started after an error"
    fi
    halt
    result "$c" "$why" "line 2 named; the menu waited"
}

case_no_default() {
    local c=$1 fw=$2 why=
    station_image $'[general]\ndefault = nxt-00ff\ntimeout = 2\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "STATION.INI's default, nxt-00ff, names no entry on this machine." 90 ||
        why="no sentence"
    if [[ -z "$why" ]]; then
        sleep 0.5
        mon "sendkey spc"
        wait_for "$MENU_UP" 15 || why="the menu did not follow"
        sleep 3
        has "Booting in" && why="a countdown with no default"
    fi
    halt
    result "$c" "$why" "said so; the menu waited"
}

case_unsigned() {
    local c=$1 fw=$2 why=
    station_image $'[general]\ndefault = memtest\ntimeout = 1\n'
    mcopy -o -i "$R/station.img@@1M" build/test/standin.efi \
        ::/EFI/falcon/memtest.efi
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "booting: Memory test (memtest86+)" 90 || why="memtest was not chosen"
    if [[ -z "$why" && $fw == uefi ]]; then
        wait_for "FALCON-STANDIN \\EFI\\falcon\\memtest.efi" 30 ||
            why="the unsigned image did not start with Secure Boot off"
    elif [[ -z "$why" ]]; then
        wait_for "MEM failed while LoadImage (shim): 0x800000000000001a" 30 ||
            why="no refusal page"
        has "the image is not signed with a key this machine trusts." ||
            why="${why:-no advice line}"
        has "FALCON-STANDIN" && why="the unsigned image ran under Secure Boot"
    fi
    halt
    if [[ $fw == uefi ]]; then
        result "$c" "$why" "the unsigned image ran"
    else
        result "$c" "$why" "refused with EFI_SECURITY_VIOLATION, said on the page"
    fi
}

case_idle_menu() {
    local c=$1 fw=$2 why= t0 t1
    station_image $'[general]\npoweroff = 5\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "$MENU_UP" 90 || why="no menu"
    t0=$(date +%s)
    [[ -z "$why" ]] && { wait_exit 30 || why="the machine was still on 30 s later"; }
    t1=$(date +%s)
    [[ -z "$why" ]] && (( t1 - t0 < 4 )) && why="powered off after $(( t1 - t0 )) s, before the time"
    [[ -z "$why" ]] && (( $(count "BdsDxe: starting") > 1 )) && why="the firmware started again (a reset, not a power-off)"
    halt
    result "$c" "$why" "the menu left alone; powered off after $(( t1 - t0 )) s"
}

case_idle_countdown() {
    local c=$1 fw=$2 why=
    station_image $'[general]\ndefault = memtest\ntimeout = 8\npoweroff = 3\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "Booting in 8 s" 90 || why="no countdown"
    [[ -z "$why" ]] && { wait_for "FALCON-STANDIN \\EFI\\falcon\\memtest.efi" 30 ||
        why="memtest did not start (powered off during the count?)"; }
    halt
    result "$c" "$why" "an 8 s count with a 3 s idle time booted its default"
}

case_idle_prompt() {
    local c=$1 fw=$2 why=
    station_image $'[general]\npoweroff = 5\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "$MENU_UP" 90 || why="no menu"
    if [[ -z "$why" ]]; then
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 30 || why="no passphrase prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys abc
        wait_exit 30 || why="the machine was still on 30 s later"
    fi
    [[ -z "$why" ]] && has "FALCON-UKI" && why="a UKI started"
    halt
    result "$c" "$why" "the prompt left with three characters typed; powered off"
}

case_bad_pass() {
    local c=$1 fw=$2 why= k d t0
    station_image ""
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "$MENU_UP" 90 || why="no menu"
    if [[ -z "$why" ]]; then
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 30 || why="no passphrase prompt"
    fi
    for k in 1 2 3 4; do
        [[ -n "$why" ]] && break
        d=$(( k == 1 ? 0 : 2 << (k - 1) ))
        sleep 0.5
        t0=$SECONDS
        send_keys "wrong$k"
        wait_count "wrong passphrase." "$k" $(( d + 60 )) || { why="no refusal after try $k"; break; }
        (( SECONDS - t0 >= d - 1 )) || { why="no $d s wait after try $k"; break; }
        wait_count "passphrase for the encrypted partition" $(( k + 1 )) 30 ||
            why="no prompt after try $k"
    done
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys wrong5
        wait_count "wrong passphrase." 5 60 || why="no refusal after try 5"
    fi
    [[ -z "$why" ]] && { wait_exit 30 || why="the machine was still on"; }
    [[ -z "$why" ]] && has "FALCON-UKI" && why="a UKI started"
    halt
    result "$c" "$why" "five wrong: waits of 0, 4, 8 and 16 s, then powered off"
}

case_retry() {
    local c=$1 fw=$2 why=
    station_image ""
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "$MENU_UP" 90 || why="no menu"
    if [[ -z "$why" ]]; then
        mon "sendkey ret"
        wait_for "passphrase for the encrypted partition" 30 || why="no passphrase prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys "wrong"
        wait_count "wrong passphrase." 1 60 || why="no refusal after a wrong one"
    fi
    [[ -z "$why" ]] && { wait_count "passphrase for the encrypted partition" 2 30 ||
        why="no second prompt"; }
    if [[ -z "$why" ]]; then
        sleep 0.5
        send_keys "$PASS"
        wait_for "FALCON-UKI-DONE" 120 || why="the UKI never reported"
    fi
    halt
    result "$c" "$why" "a wrong passphrase, no wait, then the right one: the UKI"
}

case_timing() {
    local c=$1 fw=$2 why=
    station_image $'[general]\ndefault = linux\ntimeout = 3\n\n[linux]\nuki = personal.efi\n'
    NEXT= NO_WIN_ENTRY=0 boot "$fw"
    wait_for "Booting in 3 s" 90 || why="no countdown"
    if [[ -z "$why" ]]; then
        wait_for "passphrase for the encrypted partition" 30 ||
            why="the countdown did not reach the prompt"
    fi
    if [[ -z "$why" ]]; then
        sleep 2
        send_keys "$PASS"
        wait_for "FALCON-UKI-DONE" 120 || why="the UKI never reported"
    fi
    halt
    if [[ -z "$why" ]]; then
        log_text "$R/serial.log" > "$R/$c.txt"
        bli_judge "$R/$c.txt" 4500000
        why=$BLI_WHY
    fi
    result "$c" "$why" "$BLI_DETAIL"
}

# ---- run ----

make --no-print-directory station cli "$STANDIN" "$LOADER" \
    build/test/standin.efi >/dev/null
[[ -f "$STATION" ]] || { echo "ERROR: $STATION missing (make test-station)" >&2; exit 1; }
build_win_disk

ALL=(menu countdown key-stops nxt method-efi esp-found next-boot next-uki
     next-unknown ini-error no-default unsigned idle-menu idle-countdown
     idle-prompt bad-pass retry timing)
CASES=("$@")
if [[ ${#CASES[@]} -eq 0 ]]; then
    for c in "${ALL[@]}"; do CASES+=("uefi-$c"); done
    for c in "${ALL[@]}"; do CASES+=("uefi-sb-$c"); done
fi

for c in "${CASES[@]}"; do
    case $c in
    uefi-sb-*) fw=uefi-sb; name=${c#uefi-sb-} ;;
    uefi-*)    fw=uefi;    name=${c#uefi-} ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
    case $name in
    menu)         case_menu "$c" "$fw" ;;
    countdown)    case_countdown "$c" "$fw" ;;
    key-stops)    case_key_stops "$c" "$fw" ;;
    nxt)          case_nxt "$c" "$fw" ;;
    method-efi)   case_method_efi "$c" "$fw" ;;
    esp-found)    case_esp_found "$c" "$fw" ;;
    next-boot)    case_next_boot "$c" "$fw" ;;
    next-uki)     case_next_uki "$c" "$fw" ;;
    next-unknown) case_next_unknown "$c" "$fw" ;;
    ini-error)    case_ini_error "$c" "$fw" ;;
    no-default)   case_no_default "$c" "$fw" ;;
    unsigned)     case_unsigned "$c" "$fw" ;;
    idle-menu)    case_idle_menu "$c" "$fw" ;;
    idle-countdown) case_idle_countdown "$c" "$fw" ;;
    idle-prompt)  case_idle_prompt "$c" "$fw" ;;
    bad-pass)     case_bad_pass "$c" "$fw" ;;
    retry)        case_retry "$c" "$fw" ;;
    timing)       case_timing "$c" "$fw" ;;
    *) echo "unknown case: $c" >&2; exit 2 ;;
    esac
done
exit "$FAILED"
