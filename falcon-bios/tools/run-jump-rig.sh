#!/usr/bin/env bash
# falcon jump's rig (docs/DESIGN.md Section 13.9): every route booted
# headless under SeaBIOS and judged from the serial log.
#
# One case per route, each a JUMP.INI naming it with a one-second countdown,
# so nothing waits for a key:
#
#   hdd       an IDE disk carrying falcon-bios (build/test-bios.img, an MBR
#             disk with its boot partition at LBA 63): its stage 3 loads off
#             drive 0x80
#   cd        the rig's memprobe disc in an IDE CD drive: it boots and
#             reports the memory map it was handed
#   noe820    the cd case booted under a hard disk boot sector
#             (tools/noe820.S) whose INT 15h filter fails E820, as a BIOS
#             without the call does: stage 3 builds its map from E801, and
#             memprobe reports that map with usable memory from 1 MB and
#             the resident's block carved out
#   noe820-88 the same with E801 failed too, so the map comes from AH=88h
#   usb-uhci  the rig stick (build/test-bios-gpt.img, a GPT falcon stick)
#   usb-ohci  on each controller: jump knows it by its boot record, and the
#   usb-ehci  stick's own stage 3 loads off the drive the reverse thunk
#             serves and finds its library by record
#   usb-hub-stick  the rig stick behind a usb-hub on the UHCI, found
#             through the hub and booted the same way
#   cf        a CompactFlash card in the PC Card bridge falcon-pcc.c models,
#             carrying falcon-bios: its stage 3 loads off the card
#   cf3v      the same with a card that takes only 3.3 V, as the A21m's
#             SanDisk reports itself: the bridge powers it only when both
#             of its voltage registers ask for 3.3 V
#   cf48      a card past 128 GiB (mk-rig-disk.sh big, sparse) whose boot
#             partition starts past sector 2^28: its stage 2 reads stage 3
#             from there through PCC.MOD, which takes 48-bit commands for
#             it (the model's lba48=on), and stage 3's CRC-32 holds
#   cf-rom    the cf case with option ROMs (-option-rom, dummies the rig
#             makes) placed so that a 6 KB one starts at 0xD0000, as the
#             A21m's mini-PCI boot agent does, and the model's shadow-roms=on
#             hiding the bridge in every 16 KB segment a ROM touches: the
#             probe names the ROMs, opens its window past them (trying the
#             free pages the shadow still covers first) and the card boots
#   cf-io     the cf case with an isa-ide squatting 0x1E0-0x1E7, a third
#             IDE channel's ports: the probe reads the range answered, says
#             so, and serves the card at 0x1C0, where it boots
#   mem       memtest86+, when MEMTEST names a build with the boot protocol
#             header: the handoff to it
#   giveup    the cf route to a card whose boot sector only calls INT 18h,
#             a medium with nothing to boot: jump's resident resets the
#             machine, which comes back to the floppy, rather than the BIOS
#             starting its next device on top of the resident (DESIGN 13.6)
#   s2crc     one byte of stage 2 flipped on the floppy: stage 2's own
#             CRC-32 stops it before it runs past its first sector
#   s3crc     one byte of the compressed stage 3 flipped: stage 2 reads it
#             twice and stops; on the screen, stage 2's loading line with its
#             spinner, and the fatal under it
#   stick3crc one byte of the rig stick's stage 3 flipped, the usb route
#             chosen: the stick's stage 2 reads it twice and stops
#   modcrc    one byte of UHCI.MOD flipped, the usb route chosen: the module
#             is read twice and refused
#   chs       no floppy of jump's: the stick's stages on an MBR-partitioned
#             2.88 MB floppy image, which the BIOS serves without INT 13h
#             extensions, so stage 1 and stage 2 read it by CHS
#   menu      the countdown interrupted: a five-second default=hdd, a Down
#             key two seconds in, then nothing may boot for six seconds;
#             Enter then boots the row Down moved to (the CD drive, empty in
#             QEMU's default machine), not the default. The keys go in
#             through QEMU's monitor.
#   kbd       the usb route with a usb-kbd beside the stick on the UHCI,
#             which QEMU then sends every key to, and SeaBIOS's own USB
#             keyboard ends when jump takes the controller: Escape typed on
#             the stick's menu, read through the resident's INT 16h hook,
#             takes the stick back to the BIOS (INT 18h), which the resident
#             answers with its reset (DESIGN 13.6). Caps Lock first,
#             which the resident answers with the LED report, said on
#             serial as `usb kbd leds: 02` (judged as kbd-leds)
#   kbd-hub   the same keyboard behind a usb-hub on the UHCI's second port
#   kbd-hub2  and behind a usb-hub behind another, the deepest msc.c walks
#   kbd-ehci  an ICH9 EHCI with its three UHCI companions, the stick and
#             the keyboard on EHCI root ports: the keyboard enables there
#             at high speed, EHCI.MOD hands its port to the companion, and
#             UHCI.MOD finds it
#   kbd-legacy  as kbd, with station-legacy on the stick in place of the
#             stick's loader (`mkstick station install --bios --legacy` on
#             an image with one FAT32 partition): Escape on the station's
#             menu, which runs on top of jump's resident, is its INT 18h
#   kbd-giveup  the usb route with a usb-kbd and no stick: the route comes
#             back with the keyboard up, and the keyboard alone answers the
#             failure page (Enter) and then jump's menu (Escape, its INT
#             18h, with the keyboards released first; DESIGN 13.6)
#   kbd-hdd   the same start, then the disk row chosen on the keyboard (Up
#             to the top, Enter): the disk (the hdd case's falcon-bios)
#             starts with the keyboard block behind INT 16h, and Escape on
#             its menu reaches it there and comes back to the block's reset
#
# The cf case needs the QEMU build-qemu.sh makes; the rest the system's.
#
# Usage: falcon-bios/tools/run-jump-rig.sh [case...]   (default: all)
# Env:   MEMTEST  memtest86+ for the mem case (skipped without it)
#        QEMU     the system's qemu-system-i386 (default: on PATH)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/jump-rig
QEMU=${QEMU:-qemu-system-i386}
QPCC=build/qemu-pcc/build/qemu-system-i386
TIMEOUT=45

mkdir -p "$R"
make --no-print-directory bios test-bios-img test-bios-probe build/noe820.bin >/dev/null

def() {       # def NAME: its value in biosdefs.h
    sed -n "s/^#define[ \t]*$1[ \t]*\([0-9]*\).*/\1/p" \
        falcon-bios/src/start/biosdefs.h | head -1
}
STAGE2_LBA=$(def STAGE2_LBA)
FD_STAGE3_SECTOR=$(def FD_STAGE3_SECTOR)
BOOTPART_STAGE3_SECTOR=$(def BOOTPART_STAGE3_SECTOR)

# The rig stick, as build.mk makes it: stages and records already on it.
cp build/test-bios-gpt.img "$R/stick.img"
# Where its stage 3 is: the ESP's start plus the record's stage 3 sector.
STICK_S3=$(( $(sgdisk -i 1 "$R/stick.img" |
    sed -n 's/^First sector: \([0-9]*\).*/\1/p') + BOOTPART_STAGE3_SECTOR ))

floppy() {    # floppy DEFAULT [SECONDS]
    printf 'default=%s\ntimeout=%s\n' "$1" "${2:-1}" > "$R/JUMP.INI"
    make --no-print-directory jump JUMP_INI="$R/JUMP.INI" \
        ${MEMTEST:+MEMTEST=$MEMTEST} >/dev/null
    cp build/bin/falcon-jump.img "$R/jump.img"
}

boot() {      # boot QEMU-BINARY ARGS...
    local q=$1
    shift
    cp build/test-bios.img "$R/disk.img"
    timeout "$TIMEOUT" "$q" -L /usr/share/qemu -machine pc -cpu pentium3 \
        -m 128 -display none -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img" \
        -drive file="$R/disk.img",format=raw,if=ide,index=0 \
        -serial file:"$R/serial.log" -monitor none "$@" \
        >/dev/null 2>&1 || true
    log_text > "$R/serial.txt"
}

# boot, stopped once serial carries UNTIL: the VGA text memory saved first
# (to $R/screen.txt, one row a line), for a case judged by what is on the
# screen when a fatal is waiting for its key.
boot_until() { # boot_until QEMU-BINARY UNTIL ARGS...
    local q=$1 until=$2 qpid i
    shift 2
    cp build/test-bios.img "$R/disk.img"
    rm -f "$R/serial.log" "$R/mon.sock" "$R/screen.vga" "$R/screen.txt"
    "$q" -L /usr/share/qemu -machine pc -cpu pentium3 -m 128 -display none \
        -boot a -drive if=floppy,format=raw,file="$R/jump.img" \
        -drive file="$R/disk.img",format=raw,if=ide,index=0 \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait "$@" >/dev/null 2>&1 &
    qpid=$!
    for ((i = 0; i < TIMEOUT * 10; i++)); do
        [[ -f "$R/serial.log" ]] && log_text | grep -aqF -- "$until" && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 0.5
    monitor "pmemsave 0xb8000 4000 $R/screen.vga"
    sleep 0.5
    monitor quit
    wait "$qpid" 2>/dev/null || true
    log_text > "$R/serial.txt"
    [[ -f "$R/screen.vga" ]] &&
        python3 -c 'import sys; d = open(sys.argv[1], "rb").read()[0::2]; print("\n".join(d[i:i + 80].decode("ascii", "replace").rstrip() for i in range(0, 2000, 80)))' \
            "$R/screen.vga" > "$R/screen.txt"
    return 0
}

# Flips one byte of the floppy image, at a byte offset.
flip() {      # flip OFFSET
    local b
    b=$(od -An -tu1 -j "$1" -N1 "$R/jump.img" | tr -d ' ')
    printf "\\$(printf %03o $((b ^ 0xFF)))" |
        dd of="$R/jump.img" bs=1 seek="$1" conv=notrunc status=none
}

# The byte offset of a file's contents in the floppy image: where its bytes
# as built (the .gz the image recipe copies) sit.
file_off() {  # file_off BUILT-FILE
    python3 - "$R/jump.img" "$1" <<'PY'
import sys
img, f = (open(p, 'rb').read() for p in sys.argv[1:3])
print(img.index(f[:64]))
PY
}

log_text() {
    tr -d '\r' < "$R/serial.log" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g'
}

monitor() {   # monitor COMMAND
    echo "$1" | socat - UNIX-CONNECT:"$R/mon.sock" >/dev/null 2>&1 || true
}

# The countdown, interrupted. The judgement is in three parts, each its
# own reason for a FAIL: the countdown ran, a key stopped it (no boot in the
# six seconds after, where an unstopped five-second count would have booted
# the disk), and Enter took the row the key moved to.
menu_case() {
    local i

    floppy hdd 5
    cp build/test-bios.img "$R/disk.img"
    rm -f "$R/mon.sock" "$R/serial.log"
    timeout "$TIMEOUT" "$QEMU" -L /usr/share/qemu -machine pc -cpu pentium3 \
        -m 128 -display none -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img" \
        -drive file="$R/disk.img",format=raw,if=ide,index=0 \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait >/dev/null 2>&1 &
    for i in $(seq 1 300); do
        log_text 2>/dev/null | grep -aq 'Booting in' && break
        sleep 0.1
    done
    sleep 2
    monitor "sendkey down"
    sleep 6
    log_text > "$R/serial.txt"
    local counted=0 booted=0
    grep -aq 'Booting in 5 s' "$R/serial.txt" && counted=1
    grep -aq 'booting: ' "$R/serial.txt" && booted=1
    monitor "sendkey ret"
    sleep 4
    monitor quit
    wait 2>/dev/null || true
    log_text > "$R/serial.txt"
    cp "$R/serial.txt" "$R/menu.log"

    if [[ $counted -eq 0 ]]; then
        echo "FAIL  menu      the countdown never started"
        FAILED=1
    elif [[ $booted -eq 1 ]]; then
        echo "FAIL  menu      a key did not stop the countdown"
        FAILED=1
    elif ! grep -aq 'booting: CD' "$R/serial.txt" ||
         grep -aq 'booting: Disk' "$R/serial.txt"; then
        echo "FAIL  menu      Enter did not boot the row the key moved to"
        FAILED=1
    else
        echo "PASS  menu      stopped by a key, Enter booted the next row"
    fi
}

# PASS when the log carries `want`, the boot the case is for, and every
# `also` besides.
judge() {     # judge CASE WANT [ALSO...]
    local c=$1 want=$2 also ok=1
    shift 2
    grep -aq -- "$want" "$R/serial.txt" || ok=0
    for also in "$@"; do
        grep -aq -- "$also" "$R/serial.txt" || ok=0
    done
    set -- "$c" "$want"
    if [[ $ok -eq 1 ]]; then
        printf 'PASS  %-9s %s\n' "$1" "$(grep -ao -- "$2.\{0,40\}" \
            "$R/serial.txt" | tail -1)"
    else
        printf 'FAIL  %-9s %s\n' "$1" "$(grep -aoE \
            'failed while.{0,60}|there is no.{0,50}|none of.{0,50}' \
            "$R/serial.txt" | tail -1)"
        FAILED=1
    fi
    cp "$R/serial.txt" "$R/$1.log"
}

usb() {       # usb CASE HC-DEVICES...
    local c=$1
    shift
    floppy usb
    boot "$QEMU" "$@" -drive if=none,id=stick,format=raw,file="$R/stick.img" \
        -device usb-storage,bus=hc.0,drive=stick
    judge "$c" 'boot drive 0x8[1-9a-f], sector size 512' \
        'boot partition p1 at LBA' 'p2  LBA [0-9.]*  \[library\]' \
        'p2 mounted as exfat'
}

# The stick behind a hub: as usb, with the hub between, and the module must
# have walked it.
usb_hub_stick() {
    floppy usb
    boot "$QEMU" -device piix3-usb-uhci,id=hc \
        -device usb-hub,bus=hc.0,port=1 \
        -drive if=none,id=stick,format=raw,file="$R/stick.img" \
        -device usb-storage,bus=hc.0,port=1.1,drive=stick
    judge usb-hub-stick 'boot drive 0x8[1-9a-f], sector size 512' \
        'a hub on root port' 'boot partition p1 at LBA' \
        'p2  LBA [0-9.]*  \[library\]' 'p2 mounted as exfat'
}

# The usb route with a USB keyboard and no PS/2 key: once the menu of what
# the stick holds is up (serial carries `menu`), Escape through QEMU's
# monitor, which goes to the newest keyboard, the USB one. The stick's
# loader answers Escape on its empty list with INT 18h, as station-legacy
# does on its list, and the resident answers that with a reset. PASS needs
# the module to have found the keyboard (`found`), INT 16h hooked for it,
# and the reset. The stick is $R/stick.img unless STICK names another.
# LEDS, when set, is the serial line Caps Lock typed before the Escape must
# bring, judged as CASE-leds.
kbd_case() {  # kbd_case CASE FOUND MENU QEMU-ARGS...
    local c=$1 found=$2 menu=$3 stick=${STICK:-$R/stick.img} qpid i
    shift 3
    floppy usb
    cp build/test-bios.img "$R/disk.img"
    rm -f "$R/serial.log" "$R/mon.sock"
    "$QEMU" -L /usr/share/qemu -machine pc -cpu pentium3 -m 128 \
        -display none -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img" \
        -drive file="$R/disk.img",format=raw,if=ide,index=0 \
        -drive if=none,id=stick,format=raw,file="$stick" \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait "$@" >/dev/null 2>&1 &
    qpid=$!
    for ((i = 0; i < TIMEOUT * 10; i++)); do
        [[ -f "$R/serial.log" ]] && log_text | grep -aq -- "$menu" && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 1
    if [[ -n ${LEDS:-} ]]; then
        monitor "sendkey caps_lock"
        for ((i = 0; i < 50; i++)); do
            log_text | grep -aq -- "$LEDS" && break
            sleep 0.1
        done
    fi
    monitor "sendkey esc"
    for ((i = 0; i < 100; i++)); do
        log_text | grep -aq 'returned to the BIOS; resetting' && break
        sleep 0.1
    done
    monitor quit
    wait "$qpid" 2>/dev/null || true
    log_text > "$R/serial.txt"
    judge "$c" 'returned to the BIOS; resetting' "$found" \
        'INT 16h for [0-9]'
    [[ -z ${LEDS:-} ]] || judge "$c-leds" "$LEDS"
}

# Waits until serial carries PATTERN (grep -E) at least N times, or QEMU
# has gone. Nonzero when it timed out.
wait_count() { # wait_count PATTERN N QEMU-PID
    local i
    for ((i = 0; i < TIMEOUT * 10; i++)); do
        [[ -f "$R/serial.log" ]] &&
            [[ $(log_text | grep -acE -- "$1") -ge $2 ]] && return 0
        kill -0 "$3" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# The usb route with only a usb-kbd on the UHCI, no stick: it comes back
# with the keyboard up (serial says it stays), and every key from here is
# the keyboard's, polled by jump's stage 3 itself. Enter on the failure
# page brings the menu back, a second hint line on serial. Then the case's
# own keys: kbd-giveup Escape, kbd-hdd the disk row.
kbd_back_case() { # kbd_back_case CASE
    local c=$1 qpid i
    local hint='up/down select \| enter choose \| esc back'
    floppy usb
    cp build/test-bios.img "$R/disk.img"
    rm -f "$R/serial.log" "$R/mon.sock"
    "$QEMU" -L /usr/share/qemu -machine pc -cpu pentium3 -m 128 \
        -display none -boot a \
        -drive if=floppy,format=raw,file="$R/jump.img" \
        -drive file="$R/disk.img",format=raw,if=ide,index=0 \
        -serial file:"$R/serial.log" \
        -monitor unix:"$R/mon.sock",server,nowait \
        -device piix3-usb-uhci,id=hc -device usb-kbd,bus=hc.0,port=2 \
        >/dev/null 2>&1 &
    qpid=$!
    wait_count 'keyboards stay up' 1 "$qpid" || true
    sleep 1
    monitor "sendkey ret"
    wait_count "$hint" 2 "$qpid" || true
    sleep 1
    if [[ $c == kbd-giveup ]]; then
        monitor "sendkey esc"
        wait_count 'returning to firmware' 1 "$qpid" || true
        sleep 1
    else
        for i in 1 2 3 4; do
            monitor "sendkey up"
            sleep 0.3
        done
        monitor "sendkey ret"
        # The disk's falcon-bios, up to its menu: its hint, after the
        # handover.
        wait_count 'handing over' 1 "$qpid" || true
        wait_count 'd direct \| esc back|no entries \| esc back' 1 "$qpid" ||
            true
        sleep 1
        monitor "sendkey esc"
        wait_count 'returned to the BIOS; resetting' 1 "$qpid" || true
    fi
    monitor quit
    wait "$qpid" 2>/dev/null || true
    log_text > "$R/serial.txt"
    if [[ $c == kbd-giveup ]]; then
        judge kbd-giveup 'keyboards are released' 'a keyboard on root port' \
            'there is no USB storage' 'keyboards stay up' \
            'returning to firmware'
    else
        judge kbd-hdd 'returned to the BIOS; resetting' \
            'a keyboard on root port' 'keyboards stay up' \
            'reverse thunk for no drive at .*INT 16h for 1' \
            'MBR of drive 0x80 to 0x7c00' 'boot drive 0x80, sector size 512'
    fi
}

# A dummy option ROM of SIZE bytes, a multiple of 512: 55 AA, its size in
# 512-byte units, a far return at the init entry SeaBIOS far-calls, and its
# last byte making the sum zero, which SeaBIOS checks before running it.
mkrom() {     # mkrom FILE SIZE
    local n=$(( $2 / 512 ))
    head -c "$2" /dev/zero > "$1"
    printf "\\x55\\xaa\\x$(printf %02x "$n")\\xcb" |
        dd of="$1" conv=notrunc status=none
    printf "\\x$(printf %02x $(( (256 - (0x55 + 0xAA + n + 0xCB) % 256) % 256 )))" |
        dd of="$1" bs=1 seek=$(( $2 - 1 )) conv=notrunc status=none
}

# cf-rom: SeaBIOS puts the VGA BIOS at 0xC0000 and the -option-rom files
# after it, each at the next 2 KB, in reverse order of their names (there
# is no NIC, so no PCI ROM, and vapic=off leaves out QEMU's kvmvapic ROM).
# A first boot with the 6 KB "boot agent" alone says, in the probe's own
# lines, where it lands; a filler the size of the gap, named to come before
# it, then puts it at 0xD0000, as the A21m's is, and the first free page is
# 0xD2000, inside the agent's shadowed 16 KB segment. PASS needs a ROM at
# 0xD0000, the probe to have tried a page the shadow covers and moved on,
# the window that opened outside every segment a ROM touches, and the card
# booted.
cf_rom() {
    local first gap at len chosen inside=0 q=(-nic none
        -global apic-common.vapic=off)
    mkrom "$R/rom1-agent.bin" 6144
    cp build/test-bios.img "$R/cf.img"
    floppy cf
    boot "$QPCC" "${q[@]}" -option-rom "$R/rom1-agent.bin" \
        -drive if=none,id=cf,format=raw,file="$R/cf.img" \
        -device falcon-pcc,drive=cf
    first=$(grep -ao 'an option ROM at 0x[0-9a-f]*, bytes 0x1800' \
        "$R/serial.txt" | head -1 | sed 's/.* at \(0x[0-9a-f]*\),.*/\1/' ||
        true)
    gap=$(( 0xD0000 - ${first:-0xD0000} ))
    ROMS=()
    if (( gap > 0 )); then
        mkrom "$R/rom2-filler.bin" "$gap"
        ROMS+=(-option-rom "$R/rom2-filler.bin")
    fi
    ROMS+=(-option-rom "$R/rom1-agent.bin")
    cp build/test-bios.img "$R/cf.img"
    boot "$QPCC" "${q[@]}" "${ROMS[@]}" \
        -drive if=none,id=cf,format=raw,file="$R/cf.img" \
        -device falcon-pcc,drive=cf,shadow-roms=on
    chosen=$(grep -ao 'attribute memory window at 0x[0-9a-f]*' \
        "$R/serial.txt" | tail -1 | sed 's/.* //' || true)
    while read -r at len; do
        [[ -n "$at" && -n "$chosen" ]] || continue
        (( chosen < ((at + len + 0x3FFF) & ~0x3FFF) &&
           chosen + 0x1000 > (at & ~0x3FFF) )) && inside=1
    done < <(grep -ao 'an option ROM at 0x[0-9a-f]*, bytes 0x[0-9a-f]*' \
        "$R/serial.txt" | sed 's/.* at \(0x[0-9a-f]*\), bytes \(0x[0-9a-f]*\)/\1 \2/' ||
        true)
    if ! grep -aq 'an option ROM at 0xd0000, bytes 0x1800' "$R/serial.txt"; then
        printf 'FAIL  %-9s %s\n' cf-rom "the agent ROM did not land at 0xD0000 (first boot: ${first:-none})"
        FAILED=1
    elif [[ -z "$chosen" ]] || (( inside == 1 )); then
        printf 'FAIL  %-9s %s\n' cf-rom "the window opened at ${chosen:-no address}, not past the ROMs"
        FAILED=1
    else
        judge cf-rom 'boot drive 0x81, sector size 512' \
            "attribute memory window at $chosen" \
            'the window reads what was there before it opened, at 0xd2000'
        grep -ao 'PCC.MOD: \(an option ROM\|the window reads\|attribute memory window\).*' \
            "$R/serial.txt" | sed 's/^/      /'
    fi
    cp "$R/serial.txt" "$R/cf-rom.log"
}

FAILED=0
CASES=("$@")
[[ ${#CASES[@]} -gt 0 ]] ||
    CASES=(hdd cd noe820 noe820-88 usb-uhci usb-ohci usb-ehci usb-hub-stick
           cf cf3v cf48 cf-rom cf-io
           giveup
           mem s2crc s3crc stick3crc modcrc chs menu kbd kbd-hub kbd-hub2
           kbd-ehci kbd-legacy kbd-giveup kbd-hdd)

for c in "${CASES[@]}"; do
    case $c in
    hdd)
        floppy hdd
        boot "$QEMU"
        judge hdd 'boot drive 0x80, sector size 512' \
            'boot partition p1 at LBA 63'
        ;;
    cd)
        floppy cd
        boot "$QEMU" -drive file=build/memprobe.iso,media=cdrom,if=ide,index=2
        judge cd 'MEMPROBE DONE'
        ;;
    noe820|noe820-88)
        # The shim as the hard disk's MBR, booted first (-boot c), the
        # floppy still drive 0 under it, the memprobe disc as in cd. PASS
        # needs the shim's line, stage 3 saying which call its map came
        # from, and in the map memprobe was handed usable memory from 1 MB
        # and the resident's block (vcd.c says where it went) reserved.
        head -c 4194304 /dev/zero > "$R/noe820.img"
        dd if=build/noe820.bin of="$R/noe820.img" conv=notrunc status=none
        from='memory map from E801'
        if [[ $c == noe820-88 ]]; then
            printf '\x01' |
                dd of="$R/noe820.img" bs=1 seek=508 conv=notrunc status=none
            from='memory map from AH=88h'
        fi
        floppy cd
        rm -f "$R/serial.log"
        timeout "$TIMEOUT" "$QEMU" -L /usr/share/qemu -machine pc \
            -cpu pentium3 -m 128 -display none -boot c \
            -drive if=floppy,format=raw,file="$R/jump.img" \
            -drive file="$R/noe820.img",format=raw,if=ide,index=0 \
            -drive file=build/memprobe.iso,media=cdrom,if=ide,index=2 \
            -serial file:"$R/serial.log" -monitor none >/dev/null 2>&1 || true
        log_text > "$R/serial.txt"
        blk=$(grep -ao 'serving the medium on drive 0x[0-9a-f]* from 0x[0-9a-f]*' \
            "$R/serial.txt" | tail -1 | sed 's/.* from 0x//' || true)
        judge "$c" "$from" 'NOE820: INT 15h E820 fails' 'MEMPROBE DONE' \
            'MEMPROBE E820 0000000000100000 [0-9A-F]* 00000001' \
            "MEMPROBE E820 $(printf %016X "0x${blk:-0}") [0-9A-F]* 00000002"
        ;;
    usb-uhci) usb usb-uhci -device piix3-usb-uhci,id=hc ;;
    usb-ohci) usb usb-ohci -device pci-ohci,id=hc ;;
    usb-ehci) usb usb-ehci -device usb-ehci,id=hc ;;
    usb-hub-stick) usb_hub_stick ;;
    cf|cf3v)
        if [[ ! -x "$QPCC" ]]; then
            printf 'SKIP  %-9s no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)\n' "$c"
            continue
        fi
        cp build/test-bios.img "$R/cf.img"
        floppy cf
        boot "$QPCC" -drive if=none,id=cf,format=raw,file="$R/cf.img" \
            -device falcon-pcc,drive=cf$([[ $c == cf3v ]] && echo ,volts=3)
        judge "$c" 'boot drive 0x81, sector size 512'
        ;;
    cf48)
        if [[ ! -x "$QPCC" ]]; then
            printf 'SKIP  %-9s no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)\n' "$c"
            continue
        fi
        falcon-bios/tools/mk-rig-disk.sh big "$R/cf48.img" >/dev/null
        floppy cf
        boot "$QPCC" -drive if=none,id=cf,format=raw,file="$R/cf48.img" \
            -device falcon-pcc,drive=cf,lba48=on
        rm -f "$R/cf48.img"
        judge cf48 'boot partition p1 at LBA 268437504' \
            'stage 3 inflated' 'boot drive 0x81, sector size 512'
        ;;
    cf-rom)
        if [[ ! -x "$QPCC" ]]; then
            printf 'SKIP  %-9s no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)\n' "$c"
            continue
        fi
        cf_rom
        ;;
    cf-io)
        if [[ ! -x "$QPCC" ]]; then
            printf 'SKIP  %-9s no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)\n' "$c"
            continue
        fi
        cp build/test-bios.img "$R/cf.img"
        floppy cf
        boot "$QPCC" -device isa-ide,iobase=0x1e0,iobase2=0x3e6,irq=11 \
            -drive if=none,id=cf,format=raw,file="$R/cf.img" \
            -device falcon-pcc,drive=cf
        judge cf-io 'boot drive 0x81, sector size 512' \
            'I/O at 0x1e0 answers already' 'I/O window at 0x1c0'
        ;;
    giveup)
        if [[ ! -x "$QPCC" ]]; then
            echo "SKIP  giveup    no rig QEMU (falcon-bios/tools/qemu-pcc/build-qemu.sh)"
            continue
        fi
        # INT 18h, then a halt that is never reached.
        head -c 4194304 /dev/zero > "$R/cf.img"
        printf '\xcd\x18\xfa\xf4' |
            dd of="$R/cf.img" conv=notrunc status=none
        printf '\x55\xaa' |
            dd of="$R/cf.img" bs=1 seek=510 conv=notrunc status=none
        floppy cf
        boot "$QPCC" -drive if=none,id=cf,format=raw,file="$R/cf.img" \
            -device falcon-pcc,drive=cf
        if grep -aq 'returned to the BIOS; resetting' "$R/serial.txt" &&
           [[ $(grep -ac 'stage 1, floppy' "$R/serial.txt") -ge 2 ]]; then
            echo "PASS  giveup    INT 18h reset the machine, back to the floppy"
        else
            echo "FAIL  giveup    INT 18h did not come back through a reset"
            FAILED=1
        fi
        cp "$R/serial.txt" "$R/giveup.log"
        ;;
    s2crc)
        floppy hdd
        flip $((STAGE2_LBA * 512 + 0x1000))
        boot "$QEMU"
        judge s2crc 'stage 2 did not read back as it was written'
        ;;
    s3crc)
        # Stage 3 never draws here, so the screen is still stage 2's: its
        # loading line, the spinner at a step, and the fatal under it.
        floppy hdd
        flip $((FD_STAGE3_SECTOR * 512 + 0x2000))
        boot_until "$QEMU" 'stage 3 did not read back as it was written, twice'
        if ! grep -aqF 'stage 3 did not read back as it was written, twice' "$R/serial.txt"; then
            judge s3crc 'stage 3 did not read back as it was written, twice'
        elif ! grep -qE '^falcon-boot: loading [-/|\\]$' "$R/screen.txt"; then
            printf 'FAIL  %-9s %s\n' s3crc "no 'falcon-boot: loading' line with its spinner on the screen"
            FAILED=1
        elif ! awk '/^falcon-boot: loading/ { s = 1; next }
                    s && /stage 3 did not read back as it was written, twice/ { f = 1 }
                    END { exit !f }' "$R/screen.txt"; then
            printf 'FAIL  %-9s %s\n' s3crc "the fatal is not on the screen under the loading line"
            FAILED=1
        else
            printf 'PASS  %-9s %s\n' s3crc "the loading line and its spinner on screen, the fatal under it"
        fi
        ;;
    stick3crc)
        floppy usb
        cp "$R/stick.img" "$R/stick-ok.img"
        b=$(od -An -tu1 -j $((STICK_S3 * 512 + 0x4000)) -N1 "$R/stick.img" |
            tr -d ' ')
        printf "\\$(printf %03o $((b ^ 0xFF)))" |
            dd of="$R/stick.img" bs=1 seek=$((STICK_S3 * 512 + 0x4000)) \
                conv=notrunc status=none
        boot "$QEMU" -device piix3-usb-uhci,id=hc \
            -drive if=none,id=stick,format=raw,file="$R/stick.img" \
            -device usb-storage,bus=hc.0,drive=stick
        mv "$R/stick-ok.img" "$R/stick.img"
        judge stick3crc 'stage 3 did not read back as it was written, twice'
        ;;
    modcrc)
        floppy usb
        flip $(( $(file_off build/bin/mod/UHCI.MOD.gz) + 0x400 ))
        boot "$QEMU" -device piix3-usb-uhci,id=hc
        judge modcrc 'did not read back as it was written, twice'
        ;;
    chs)
        falcon-bios/tools/mk-rig-disk.sh chs "$R/chs.img" >/dev/null
        rm -f "$R/serial.log"
        timeout "$TIMEOUT" "$QEMU" -L /usr/share/qemu -machine pc \
            -cpu pentium3 -m 128 -display none -boot a \
            -drive if=floppy,format=raw,file="$R/chs.img" \
            -serial file:"$R/serial.log" -monitor none >/dev/null 2>&1 || true
        log_text > "$R/serial.txt"
        judge chs 'boot partition p1 at LBA 63' \
            'no INT 13h extensions, reading by CHS' 'C is running'
        ;;
    menu) menu_case ;;
    kbd)
        LEDS='usb kbd leds: 02' kbd_case kbd 'a keyboard on root port' 'no entries | esc back' \
            -device piix3-usb-uhci,id=hc \
            -device usb-storage,bus=hc.0,port=1,drive=stick \
            -device usb-kbd,bus=hc.0,port=2
        ;;
    kbd-hub)
        kbd_case kbd-hub 'a keyboard, behind a hub on root port' \
            'no entries | esc back' -device piix3-usb-uhci,id=hc \
            -device usb-storage,bus=hc.0,port=1,drive=stick \
            -device usb-hub,bus=hc.0,port=2 \
            -device usb-kbd,bus=hc.0,port=2.1
        ;;
    kbd-hub2)
        kbd_case kbd-hub2 'a keyboard, behind two hubs on root port' \
            'no entries | esc back' -device piix3-usb-uhci,id=hc \
            -device usb-storage,bus=hc.0,port=1,drive=stick \
            -device usb-hub,bus=hc.0,port=2 \
            -device usb-hub,bus=hc.0,port=2.1 \
            -device usb-kbd,bus=hc.0,port=2.1.1
        ;;
    kbd-ehci)
        kbd_case kbd-ehci 'handed to the companion controller' \
            'no entries | esc back' \
            -device ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on \
            -device ich9-usb-uhci1,addr=1d.0,multifunction=on,masterbus=ehci.0,firstport=0 \
            -device ich9-usb-uhci2,addr=1d.1,multifunction=on,masterbus=ehci.0,firstport=2 \
            -device ich9-usb-uhci3,addr=1d.2,multifunction=on,masterbus=ehci.0,firstport=4 \
            -device usb-storage,bus=ehci.0,port=1,drive=stick \
            -device usb-kbd,bus=ehci.0,port=3
        # Twice: EHCI.MOD naming the port it hands on, UHCI.MOD finding it.
        if [[ $(grep -ac 'a keyboard on root port' "$R/serial.txt") -lt 2 ]]; then
            echo "FAIL  kbd-ehci  the companion's module did not find the keyboard"
            FAILED=1
        fi
        ;;
    kbd-legacy)
        make --no-print-directory station-legacy \
            build/bin/falcon-boot-mkstick >/dev/null
        rm -f "$R/legacy.img" "$R/legacy.bak"
        truncate -s 256M "$R/legacy.img"
        printf 'label: dos\nunit: sectors\nstart=2048, size=262144, type=c\n' |
            sfdisk -q "$R/legacy.img"
        mkfs.fat -F 32 --offset 2048 -n DATA "$R/legacy.img" 131072 \
            >/dev/null 2>&1
        build/bin/falcon-boot-mkstick station install "$R/legacy.img" \
            --bios --legacy --no-scan --backup "$R/legacy.bak" \
            >"$R/legacy-install.log" 2>&1 ||
            { tail -5 "$R/legacy-install.log"; exit 1; }
        STICK=$R/legacy.img kbd_case kbd-legacy 'a keyboard on root port' \
            'entries, [0-9]* shown' \
            -device piix3-usb-uhci,id=hc \
            -device usb-storage,bus=hc.0,port=1,drive=stick \
            -device usb-kbd,bus=hc.0,port=2
        rm -f "$R/legacy.img"
        ;;
    kbd-giveup|kbd-hdd) kbd_back_case "$c" ;;
    mem)
        if [[ -z "${MEMTEST:-}" ]]; then
            echo "SKIP  mem       no MEMTEST"
            continue
        fi
        floppy memtest
        boot "$QEMU"
        judge mem 'entering the kernel'
        ;;
    *)
        echo "unknown case: $c" >&2
        exit 2
        ;;
    esac
done
exit "$FAILED"
