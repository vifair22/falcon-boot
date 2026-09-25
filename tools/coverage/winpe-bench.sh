#!/usr/bin/env bash
# falcon-boot: run programs by hand inside a stock Windows installer's WinPE.
#
# The harness judges a whole boot; this answers a smaller question it cannot:
# what does THIS WinPE say when it runs THIS program. It boots an unmodified
# Windows ISO under SeaBIOS, opens a command prompt in Setup with Shift+F10,
# and types the given commands, with a FAT disk carrying the files named on
# the command line attached as the first hard disk (C: in Setup's WinPE). A
# screenshot is taken after each command, so a program that will not start
# shows the loader's own status code instead of a silent reboot.
#
# Built for winjump (falcon-winjump), whose failures happen before it can log
# anything, and for the older Windows media whose WinPE differs.
#
# Usage (inside the harness container, tools/coverage/harness.sh):
#   tools/coverage/winpe-bench.sh ISO OUTDIR FILE... -- COMMAND...
# Env: BENCH_BOOT_WAIT seconds from power-on to Setup (default 70),
#      BENCH_CPU (default host), BENCH_MEM MiB (default 2048).
set -euo pipefail
# sgdisk and mkfs.fat are administrators' tools and live in sbin, which the
# harness's user PATH leaves out.
PATH=$PATH:/usr/sbin:/sbin

ISO=$1
OUT=$2
shift 2
FILES=()
while [[ $# -gt 0 && "$1" != -- ]]; do
    FILES+=("$1")
    shift
done
[[ "${1:-}" == -- ]] && shift
CMDS=("$@")

mkdir -p "$OUT"
TOOLS=$OUT/tools.img
MON=$OUT/mon.sock
rm -f "$TOOLS" "$MON" "$OUT"/cmd-*.png "$OUT"/cmd-*.ppm
# A partitioned disk with one FAT16 volume: Windows does not mount a bare
# filesystem on a fixed disk, only on removable media. One MBR entry, type
# 0x06, written directly: it is sixteen bytes, and every Windows reads it.
truncate -s 64M "$TOOLS"
python3 - "$TOOLS" <<'PY'
import struct, sys
start, count = 2048, 64 * 2048 - 2048
entry = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x06, b'\xfe\xff\xff',
                    start, count)
with open(sys.argv[1], 'r+b') as f:
    f.seek(440)
    f.write(struct.pack('<I', 0xFA1C0B00) + b'\0\0' + entry + b'\0' * 48 +
            b'\x55\xaa')
PY
mkfs.fat -F 16 -n BENCH --offset 2048 "$TOOLS" $(( (64 * 1024 - 1024) )) >/dev/null
for f in "${FILES[@]}"; do
    mcopy -i "$TOOLS@@1M" "$f" "::$(basename "$f")"
done

ACCEL=(-enable-kvm -cpu "${BENCH_CPU:-host}")
[[ -w /dev/kvm ]] || ACCEL=(-cpu "${BENCH_CPU:-max}")

qemu-system-x86_64 -machine pc -m "${BENCH_MEM:-2048}" "${ACCEL[@]}" \
    -drive "format=raw,file=$TOOLS,if=ide,index=0" \
    -drive "format=raw,file=$ISO,media=cdrom,readonly=on,if=ide,index=2" \
    -boot d -display none -net none \
    -monitor "unix:$MON,server,nowait" &
QEMU=$!
trap 'kill "$QEMU" 2>/dev/null || true' EXIT

mon() { echo "$1" | socat - UNIX-CONNECT:"$MON" >/dev/null 2>&1 || true; }
shot() { mon "screendump $(readlink -f "$OUT")/$1.ppm"; sleep 1; }

# "Press any key to boot from CD or DVD" waits a few seconds.
for i in $(seq 1 12); do
    sleep 1
    mon "sendkey spc"
done
sleep "${BENCH_BOOT_WAIT:-70}"
shot setup
mon "sendkey shift-f10"
sleep 8
shot shell

# The keys a command line needs, as QEMU names them.
type_line() {
    local s=$1 c k i
    for ((i = 0; i < ${#s}; i++)); do
        c=${s:i:1}
        case "$c" in
            [a-z0-9]) k=$c ;;
            [A-Z]) k="shift-${c,,}" ;;
            ' ') k=spc ;; '.') k=dot ;; '\') k=backslash ;; ':') k=shift-semicolon ;;
            '-') k=minus ;; '_') k=shift-minus ;; '/') k=slash ;; '>') k=shift-dot ;;
            '|') k=shift-backslash ;; '=') k=equal ;; '"') k=shift-apostrophe ;;
            '%') k=shift-5 ;; '(') k=shift-9 ;; ')') k=shift-0 ;; '*') k=shift-8 ;;
            '&') k=shift-7 ;; '!') k=shift-1 ;;
            *) continue ;;
        esac
        mon "sendkey $k"
    done
    mon "sendkey ret"
}

n=0
for c in "${CMDS[@]}"; do
    n=$((n + 1))
    type_line "$c"
    sleep "${BENCH_CMD_WAIT:-6}"
    shot "cmd-$n"
done
mon quit
wait "$QEMU" 2>/dev/null || true
trap - EXIT
ls "$OUT"
