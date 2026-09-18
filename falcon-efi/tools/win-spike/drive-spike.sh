#!/usr/bin/env bash
# Drive one headless spike run to completion: launch run-shell.sh with the
# given arguments, feed the guest Enter keys for a while (Microsoft's
# cdboot.efi wants one), then screendump and shut the guest down.
#
# Usage: drive-spike.sh <tag> <settle-seconds> <key-seconds> [qemu args...]
# Output: build/win-spike/<tag>.serial.log and build/win-spike/<tag>-shot.png

set -uo pipefail

cd "$(dirname "$0")/../../.."

TAG=${1:?usage: drive-spike.sh <tag> <settle> <keys> [qemu args...]}
SETTLE=${2:?}
KEYS=${3:?}
shift 3

D=build/win-spike
MON=$D/$TAG.mon.sock

pkill -f falcon-win-shell >/dev/null 2>&1 || true
sleep 2
rm -f "$MON" "$D/$TAG.serial.log"

falcon-efi/tools/win-spike/run-shell.sh "$TAG" "$D/$TAG.nsh" "$@" >/dev/null 2>&1 &
sleep "$SETTLE"

for ((i = 0; i < KEYS; i++)); do
    printf 'sendkey ret\n' | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1 || true
    sleep 1
done

printf 'screendump %s\n' "$PWD/$D/$TAG-shot.ppm" \
    | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1 || true
sleep 2
python3 -c "
from PIL import Image
import sys
Image.open('$D/$TAG-shot.ppm').save('$D/$TAG-shot.png')
" 2>/dev/null || true

pkill -f falcon-win-shell >/dev/null 2>&1 || true
echo "done: $TAG"
