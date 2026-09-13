#!/usr/bin/env bash
# Launch a spike run headless and take a numbered screendump every two
# seconds, so a screen that is only drawn for a few seconds (Microsoft's
# "Press any key to boot from CD or DVD" prompt) cannot be missed.
#
# Usage: shot-series.sh <tag> <settle-seconds> <shots> <interval> [qemu args...]
# Output: build/win-spike/<tag>-NN.png

set -uo pipefail

cd "$(dirname "$0")/../.."

TAG=${1:?usage: shot-series.sh <tag> <settle> <shots> [qemu args...]}
SETTLE=${2:?}
SHOTS=${3:?}
INTERVAL=${4:?}
shift 4

D=build/win-spike
MON=$D/$TAG.mon.sock

pkill -f falcon-win-shell >/dev/null 2>&1 || true
sleep 2
rm -f "$MON" "$D/$TAG.serial.log" "$D/$TAG"-[0-9][0-9].ppm "$D/$TAG"-[0-9][0-9].png

tools/win-spike/run-shell.sh "$TAG" "$D/$TAG.nsh" "$@" >/dev/null 2>&1 &
sleep "$SETTLE"

for ((i = 1; i <= SHOTS; i++)); do
    n=$(printf '%02d' "$i")
    printf 'screendump %s\n' "$PWD/$D/$TAG-$n.ppm" \
        | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1 || true
    sleep "$INTERVAL"
done

for f in "$D/$TAG"-[0-9][0-9].ppm; do
    [[ -e $f ]] || continue
    python3 -c "
from PIL import Image
Image.open('$f').save('${f%.ppm}.png')
" 2>/dev/null || true
    rm -f "$f"
done

pkill -f falcon-win-shell >/dev/null 2>&1 || true
echo "done: $TAG"
