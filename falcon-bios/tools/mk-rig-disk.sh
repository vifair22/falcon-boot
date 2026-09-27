#!/usr/bin/env bash
# falcon-bios's rig disks, on the single BIOS layout (docs/DESIGN.md Section
# 3.3), made from files alone: no loop device, no mount, no root.
#
#   mbr  build/test-bios.img: an MBR disk whose one partition, at LBA 63 as
#        on an old disk (the A21m's shape), is a hidden FAT32 `boot`
#        partition (type 0x1C): stage 3 in its reserved sectors, the marker
#        sector, and its identity record. Stage 1 in LBA 0 with stage 2's
#        place written in, stage 2 at LBA 1, in the 62 sectors in front of
#        the partition. It boots the stick's stage 3 as the only disk; it
#        has no ISO library of its own.
#   gpt  build/test-bios-gpt.img: a stick. p1 the ESP, FAT32, carrying stage
#        3 in its reserved sectors and the boot record; stage 2 at LBA 34,
#        in the gap in front of p1; p2 exFAT with a /.falcon library record
#        of the same install ID. What the jump rig boots over USB, and what
#        the stick's stage 3 finds its library on by record.
#   big  the mbr disk's layout on a sparse disk past 128 GiB, its boot
#        partition starting at LBA 2^28 + 2048: stage 2 reads stage 3 from
#        past the last sector a 28-bit ATA command reaches, which only a
#        driver that takes the disk's 48-bit commands can do. The jump rig's
#        cf48 case; a few megabytes on the host.
#   chs  a 2.88 MB floppy image laid out as the mbr disk is (a FAT12 boot
#        partition at 63): a BIOS offers no INT 13h extensions on a floppy
#        drive, so booted as one, stage 1 and stage 2 read it by CHS. The
#        jump rig's chs case; no real medium is laid out this way.
#
# Every number comes from falcon-bios/src/start/biosdefs.h, never written
# here: STAGE2_LBA, STAGE2_LBA_MBR, STAGE1_S2_*_OFF and BOOTPART_*.
#
# Usage: falcon-bios/tools/mk-rig-disk.sh mbr|gpt|big|chs OUT
# The boot records and the reserved-sector count are mkstick's own (`ident
# --write-boot`, `ident --reserved-sectors`), so a rig disk's boot partition
# is formatted and stamped as `mkstick create` does a stick's p1.
#
# Needs: sfdisk, sgdisk, mkfs.fat (dosfstools), mkfs.exfat (exfatprogs),
#        python3; the stages and falcon-boot-mkstick already built.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

KIND=${1:?usage: mk-rig-disk.sh mbr|gpt|big|chs OUT}
OUT=${2:?usage: mk-rig-disk.sh mbr|gpt|big|chs OUT}

S1=build/bin/falcon-bios-stage1.bin
S2=build/bin/falcon-bios-stage2.bin
S3=build/bin/falcon-bios-stage3.bin
MKSTICK=${MKSTICK:-build/bin/falcon-boot-mkstick}

# Fixed install IDs, so a log names the same install every run.
RIG_MBR_ID=f41c0b1057a6e0000000000000000001
RIG_GPT_ID=f41c0b1057a6e0000000000000000002
RIG_CHS_ID=f41c0b1057a6e0000000000000000003
RIG_BIG_ID=f41c0b1057a6e0000000000000000004

def() {       # def NAME: its value in biosdefs.h, hex or decimal
    local v
    v=$(sed -n "s/^#define[ \t]*$1[ \t]*\([0-9A-Fa-fx]*\).*/\1/p" \
        falcon-bios/src/start/biosdefs.h | head -1)
    [[ -n "$v" ]] || { echo "ERROR: $1 is not in biosdefs.h" >&2; exit 1; }
    echo $((v))
}

STAGE2_LBA=$(def STAGE2_LBA)
STAGE2_LBA_MBR=$(def STAGE2_LBA_MBR)
S2_LBA_OFF=$(def STAGE1_S2_LBA_OFF)
S2_SECTORS_OFF=$(def STAGE1_S2_SECTORS_OFF)
STAGE3=$(def BOOTPART_STAGE3_SECTOR)

s2_sectors=$(( ($(stat -c %s "$S2") + 511) / 512 ))
s3_sectors=$(( ($(stat -c %s "$S3") + 511) / 512 ))
# The reserved sectors, by mkstick create's rule.
reserved=$("$MKSTICK" ident --reserved-sectors --stage3 "$S3")

le() {        # le VALUE BYTES: VALUE as BYTES little-endian bytes
    local v=$1 n=$2 i out=
    for ((i = 0; i < n; i++)); do
        out+=$(printf '\\%03o' $(( (v >> (8 * i)) & 255 )))
    done
    printf "$out"
}

# Stage 1's 440 bytes (the table behind them is the disk's), then stage 2's
# place and length into it, then stage 2.
stages12() {  # stages12 IMG S2_LBA
    dd if="$S1" of="$1" bs=440 count=1 conv=notrunc status=none
    le "$2" 4 | dd of="$1" bs=1 seek="$S2_LBA_OFF" conv=notrunc status=none
    le "$s2_sectors" 2 |
        dd of="$1" bs=1 seek="$S2_SECTORS_OFF" conv=notrunc status=none
    dd if="$S2" of="$1" bs=512 seek="$2" conv=notrunc status=none
}

# A FAT boot partition, partition NUMBER at START, SIZE_KIB long, in place
# in the image: stage 3 in its reserved sectors, its record in its boot
# sector.
bootpart() {  # bootpart IMG START SIZE_KIB ID NUMBER [FAT_BITS]
    local log
    # The image runs past the partition; mkfs.fat says so and does as told.
    # FAT32 wants many small clusters, FAT12 few: 1 and 4 sectors.
    log=$(mkfs.fat -F "${6:-32}" -S 512 -s $(( ${6:-32} == 12 ? 4 : 1 )) \
        -R "$reserved" -h "$2" \
        --offset "$2" -n FALCON-BOOT "$1" "$3" 2>&1) || {
        echo "$log" >&2; exit 1; }
    dd if="$S3" of="$1" bs=512 seek=$(( $2 + STAGE3 )) conv=notrunc \
        status=none
    "$MKSTICK" ident "$1" --write-boot "$5" --id "$4" >/dev/null
}

# One file at the root of a fresh exFAT image, written straight into its
# structures: mkfs.exfat makes the volume and nothing makes a file without a
# mount, so this does. A free cluster from the allocation bitmap, marked
# used; the file's three directory entries (file, stream extension, name)
# with their set checksum and name hash, in the root directory's first free
# slot; the data in the cluster, contiguous (NoFatChain). Enough for the
# small text file a /.falcon is, which is all it is for.
exfat_put() { # exfat_put IMG NAME CONTENT
    python3 - "$1" "$2" "$3" <<'PY'
import struct, sys
img, name, data = sys.argv[1], sys.argv[2], sys.argv[3].encode()
f = open(img, 'r+b')
vbr = f.read(512)
bps = 1 << vbr[108]
spc = 1 << vbr[109]
cb = bps * spc
fat_off = struct.unpack_from('<I', vbr, 80)[0] * bps
heap = struct.unpack_from('<I', vbr, 88)[0] * bps
root = struct.unpack_from('<I', vbr, 96)[0]
assert len(data) <= cb and len(name) <= 15
def cl(n):
    return heap + (n - 2) * cb
f.seek(cl(root)); rd = bytearray(f.read(cb))
bitmap = None
for i in range(0, cb, 32):
    if rd[i] == 0x81:
        bitmap = struct.unpack_from('<IQ', rd, i + 20)
f.seek(cl(bitmap[0])); bm = bytearray(f.read(bitmap[1]))
free = next(i for i in range(len(bm) * 8) if not bm[i // 8] & (1 << (i % 8)))
bm[free // 8] |= 1 << (free % 8)
f.seek(cl(bitmap[0])); f.write(bm)
c = free + 2
f.seek(fat_off + 4 * c); f.write(struct.pack('<I', 0xFFFFFFFF))
f.seek(cl(c)); f.write(data.ljust(cb, b'\0'))
up = name.upper()
h = 0
for ch in up.encode('utf-16-le'):
    h = (((h << 15) | (h >> 1)) + ch) & 0xFFFF
ent = bytearray(96)
ent[0] = 0x85; ent[1] = 2
struct.pack_into('<H', ent, 4, 0x20)
ent[32] = 0xC0; ent[33] = 0x03; ent[35] = len(name)
struct.pack_into('<H', ent, 36, h)
struct.pack_into('<Q', ent, 40, len(data))
struct.pack_into('<I', ent, 52, c)
struct.pack_into('<Q', ent, 56, len(data))
ent[64] = 0xC1
ent[66:66 + 2 * len(name)] = name.encode('utf-16-le')
s = 0
for i, b in enumerate(ent):
    if i in (2, 3):
        continue
    s = (((s << 15) | (s >> 1)) + b) & 0xFFFF
struct.pack_into('<H', ent, 2, s)
slot = next(i for i in range(0, cb, 32) if rd[i] == 0)
assert slot + 96 <= cb
rd[slot:slot + 96] = ent
f.seek(cl(root)); f.write(rd)
f.close()
PY
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
rm -f "$OUT"
mkdir -p "$(dirname "$OUT")"

case $KIND in
mbr)
    START=63
    SIZE_MIB=40
    (( STAGE2_LBA_MBR + s2_sectors <= START )) || {
        echo "ERROR: stage 2 ($s2_sectors sectors) does not fit in front of LBA $START" >&2
        exit 1; }
    truncate -s $(( (START * 512) + (SIZE_MIB << 20) + (1 << 20) )) "$OUT"
    printf 'label: dos\nlabel-id: 0x%s\nstart=%d, size=%d, type=1c\n' \
        "${RIG_MBR_ID:0:8}" "$START" $(( SIZE_MIB * 2048 )) |
        sfdisk -q --no-reread --no-tell-kernel "$OUT" >/dev/null 2>&1
    bootpart "$OUT" "$START" $(( SIZE_MIB * 1024 )) "$RIG_MBR_ID" 1
    stages12 "$OUT" "$STAGE2_LBA_MBR"
    echo "$OUT: MBR, boot partition p1 (0x1c) at LBA $START, stage 2 at LBA" \
         "$STAGE2_LBA_MBR ($s2_sectors sectors), stage 3 at +$STAGE3" \
         "($s3_sectors of $(( reserved - STAGE3 )) sectors), install $RIG_MBR_ID"
    ;;
big)
    START=$(( (1 << 28) + 2048 ))
    SIZE_MIB=40
    truncate -s $(( (START * 512) + (SIZE_MIB << 20) + (1 << 20) )) "$OUT"
    printf 'label: dos\nlabel-id: 0x%s\nstart=%d, size=%d, type=1c\n' \
        "${RIG_BIG_ID:0:8}" "$START" $(( SIZE_MIB * 2048 )) |
        sfdisk -q --no-reread --no-tell-kernel "$OUT" >/dev/null 2>&1
    bootpart "$OUT" "$START" $(( SIZE_MIB * 1024 )) "$RIG_BIG_ID" 1
    stages12 "$OUT" "$STAGE2_LBA_MBR"
    echo "$OUT: MBR, $(( $(stat -c %s "$OUT") >> 30 )) GiB sparse, boot" \
         "partition p1 (0x1c) at LBA $START, past 2^28, install $RIG_BIG_ID"
    ;;
gpt)
    ESP_MIB=40
    LIB_MIB=8
    truncate -s $(( (ESP_MIB + LIB_MIB + 2) << 20 )) "$OUT"
    sgdisk -Z "$OUT" >/dev/null 2>&1 || true
    sgdisk -n 1:2048:+${ESP_MIB}M -t 1:EF00 -c 1:FALCON-ESP \
           -n 2:0:+${LIB_MIB}M -t 2:0700 -c 2:FALCON-ISO "$OUT" >/dev/null
    esp=$(sgdisk -i 1 "$OUT" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    lib=$(sgdisk -i 2 "$OUT" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
    (( STAGE2_LBA + s2_sectors <= esp )) || {
        echo "ERROR: stage 2 ($s2_sectors sectors) does not fit in front of LBA $esp" >&2
        exit 1; }
    bootpart "$OUT" "$esp" $(( ESP_MIB * 1024 )) "$RIG_GPT_ID" 1
    truncate -s $(( LIB_MIB << 20 )) "$tmp/lib.img"
    mkfs.exfat -n FALCON-ISO "$tmp/lib.img" >/dev/null
    exfat_put "$tmp/lib.img" .falcon "falcon 1 library $RIG_GPT_ID"$'\n'
    dd if="$tmp/lib.img" of="$OUT" bs=512 seek="$lib" conv=notrunc status=none
    stages12 "$OUT" "$STAGE2_LBA"
    echo "$OUT: GPT, ESP p1 at LBA $esp (boot record, stage 3 at +$STAGE3)," \
         "exFAT p2 at LBA $lib (library record), stage 2 at LBA $STAGE2_LBA," \
         "install $RIG_GPT_ID"
    ;;
chs)
    START=63
    SECTORS=5760                        # 2.88 MB: 80 x 2 x 36
    truncate -s $(( SECTORS * 512 )) "$OUT"
    printf 'label: dos\nlabel-id: 0x%s\nstart=%d, size=%d, type=1\n' \
        "${RIG_CHS_ID:0:8}" "$START" $(( SECTORS - START )) |
        sfdisk -q --no-reread --no-tell-kernel "$OUT" >/dev/null 2>&1
    bootpart "$OUT" "$START" $(( (SECTORS - START) / 2 )) "$RIG_CHS_ID" 1 12
    stages12 "$OUT" "$STAGE2_LBA_MBR"
    echo "$OUT: 2.88 MB floppy with an MBR, FAT12 boot partition at LBA" \
         "$START, stage 2 at LBA $STAGE2_LBA_MBR, install $RIG_CHS_ID"
    ;;
*)
    echo "usage: mk-rig-disk.sh mbr|gpt|chs OUT" >&2
    exit 2
    ;;
esac
