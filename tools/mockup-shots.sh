#!/usr/bin/env bash
# Photograph screens of docs/tui-mockup.html for the README, into docs/img/.
#
# Each screen is drawn alone through the mockup's #<screen>-<platform>-<mirror>
# hash, by a headless Chrome on the host (the harness carries no browser),
# and trimmed to the screen. Rerun after the mockup changes.
#
# Usage: tools/mockup-shots.sh [chrome binary]

set -euo pipefail

cd "$(dirname "$0")/.."

CHROME=${1:-$(command -v google-chrome-stable || command -v google-chrome ||
              command -v chromium || command -v chromium-browser || true)}
[[ -n $CHROME ]] || { echo "mockup-shots: no Chrome or Chromium on PATH" >&2; exit 1; }
command -v magick >/dev/null || { echo "mockup-shots: needs ImageMagick (magick)" >&2; exit 1; }

# id, then the file it is saved as.
SHOTS=(
    "menu-UEFI-vga         stick-menu"
    "booting-UEFI-vga      booting"
    "chooser-UEFI-vga      personal-os"
    "station-UEFI-vga      station"
    "jump-BIOS-vga         jump"
)

OUT=docs/img
mkdir -p "$OUT"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for s in "${SHOTS[@]}"; do
    read -r id name <<<"$s"
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars \
        --user-data-dir="$tmp/profile" --window-size=800,520 \
        --screenshot="$tmp/$name.png" \
        "file://$PWD/docs/tui-mockup.html#$id" >/dev/null 2>&1
    # The page's own background is #1b1b1b and the screen's is black: a
    # border in the page's colour gives trim a corner to match.
    magick "$tmp/$name.png" -bordercolor '#1b1b1b' -border 1 -trim +repage \
        "$OUT/$name.png"
    echo "mockup-shots: $OUT/$name.png"
done
