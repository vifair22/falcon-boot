#!/usr/bin/env bash
# falcon-boot: boot a station fixture disk on its own and photograph it.
#
# The check that a fixture is what it claims: the image alone as the first
# IDE disk of an i440fx machine under SeaBIOS, nothing else attached, and a
# screendump after it has had time to reach its prompt or desktop. The guest
# runs from a throwaway overlay (-snapshot), so looking at a fixture never
# changes it.
#
# Usage: fixture-shot.sh [--wait SECS] [--cpu MODEL] [--mem MB] IMG OUT.png
#   --wait  seconds to let it boot before the shot (default 120)
#   --cpu   QEMU CPU model (default pentium2, which every fixture targets)
#   --mem   guest memory in MB (default 128)
# Needs qemu-system-i386 and python3 (the harness image has both).

set -euo pipefail

WAIT=120
CPU=pentium2
MEM=128
while [[ $# -gt 0 ]]; do
    case "$1" in
    --wait) WAIT=$2; shift 2 ;;
    --cpu) CPU=$2; shift 2 ;;
    --mem) MEM=$2; shift 2 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "ERROR: unknown option $1" >&2; exit 1 ;;
    *) break ;;
    esac
done
[[ $# -eq 2 ]] || { echo "usage: $0 [opts] IMG OUT.png" >&2; exit 1; }
IMG=$1
OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)

RUN=$(mktemp -d)
trap 'kill "$QPID" 2>/dev/null || true; rm -rf "$RUN"' EXIT
MON=$RUN/mon.sock

# TCG, not KVM: Windows 9x's timing loops misbehave on a host-speed CPU, and
# these fixtures stand in for machines of that era.
qemu-system-i386 -machine pc -accel tcg -cpu "$CPU" -m "$MEM" \
    -vga cirrus -nic none -rtc base=localtime -display none \
    -drive file="$IMG",format=raw,if=ide,index=0 -snapshot \
    -boot order=c -monitor unix:"$MON",server,nowait &
QPID=$!

sleep "$WAIT"
python3 "$HERE/vmctl.py" shot "$MON" "$OUT"
