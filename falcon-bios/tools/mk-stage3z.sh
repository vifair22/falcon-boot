#!/usr/bin/env bash
# A compressed stage 3 (falcon-bios/src/start/biosdefs.h, STAGE3_MAGIC_GZ):
# the "BOSZ" header, then a gzip member of the ordinary stage 3 image, which
# stage 2 inflates (load3.c). For falcon jump's floppy, where the bytes read
# are the time taken (docs/DESIGN.md Section 14.3).
#
# Usage: mk-stage3z.sh STAGE3.bin OUT.bin
# gzip -9 -n: the smallest deflate gzip makes, and no name or time in the
# header, so the same stage 3 always makes the same file.
set -euo pipefail

in=$1
out=$2

le32() {
    printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) \
        $((($1 >> 8) & 255)) $((($1 >> 16) & 255)) $((($1 >> 24) & 255)))"
}

magic=$(od -An -tu4 -N4 "$in" | tr -d ' ')
len=$(stat -c %s "$in")
mem=$(od -An -tu4 -j8 -N4 "$in" | tr -d ' ')
hdr_len=$(od -An -tu4 -j4 -N4 "$in" | tr -d ' ')
# "BOS3", and its header's own idea of its length, or this is not a stage 3.
if [[ "$magic" != 861097794 || "$hdr_len" != "$len" ]]; then
    echo "mk-stage3z: $in is not a stage 3 image" >&2
    exit 1
fi

gzip -9 -n -c "$in" > "$out.gz"
file=$(( 16 + $(stat -c %s "$out.gz") ))
{
    le32 1515409218     # "BOSZ"
    le32 "$file"
    le32 "$mem"
    le32 "$len"
    cat "$out.gz"
} > "$out"
rm -f "$out.gz"
echo "$out: $len bytes in $file"
