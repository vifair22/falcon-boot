#!/usr/bin/env bash
# falcon-boot: a station updated from a station bundle (docs/DESIGN.md
# Section 13.14), the way DevLink recovery updates one in the field.
#
# A scratch MBR disk (sparse, the station partition past 4 GiB) with a
# partition of its own gets station-legacy from the installer, with memtest
# and the rig's fixture UKI in /uki; the install is then aged, its stage 3
# swapped for the plain station's and its drivers removed, so the update has
# something to change that a boot can see. The bundle's update.sh runs
# against the disk attached as a loop device with its partitions, as the
# machine's disk is, and the disk is booted under SeaBIOS (needs sudo):
#
#   update   update.sh reports success, the stage 3 on disk is the bundle's,
#            STATION.INI and /uki are byte for byte what the install wrote,
#            and the drivers are back
#   boot     the menu comes up as the bundle's version, logo and all
#   refuse   the same bundle refuses a disk whose boot partition is not a
#            station's, and a bundle whose MANIFEST does not match
#
# Nothing touches a real disk. Usage: falcon-bios/tools/run-bundle-rig.sh
# Env: BUNDLE  a bundle built elsewhere (the harness container, where
#              make station-bundle can pass check-floor.sh --userland)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
R=build/bundle-rig
MK=build/bin/falcon-boot-mkstick
MEMTEST=${MEMTEST:-build/memtest/mt86p_810_i586}

command -v qemu-system-i386 >/dev/null ||
    { echo "no qemu-system-i386" >&2; exit 2; }
[[ -f "$MEMTEST" ]] || { echo "no memtest86+ at $MEMTEST (MEMTEST)" >&2; exit 2; }
make --no-print-directory "$MK" >/dev/null
if [[ -z "${BUNDLE:-}" ]]; then
    make --no-print-directory station-bundle MEMTEST="$MEMTEST" >/dev/null
    BUNDLE=$(ls -t build/bin/falcon-station-*.tar.gz | head -1)
fi

rm-guard rm -rf "$R" >/dev/null 2>&1 || true
mkdir -p "$R/unpacked"
gunzip -c "$BUNDLE" | tar -C "$R/unpacked" -xf -
B=$R/unpacked/station-bundle
VERSION=$(cat "$B/VERSION")

FAILED=0
result() {      # result CASE OK DETAIL
    if [[ $2 -eq 1 ]]; then
        printf 'PASS  %-7s %s\n' "$1" "$3"
    else
        printf 'FAIL  %-7s %s\n' "$1" "$3"
        FAILED=1
    fi
}

# The disk, and the station on it as an older falcon would have left it.
D=$R/disk.img
# Sparse, and the first partition large enough that the station partition
# after it starts past 4 GiB: a 32-bit off_t stops at 2 GiB and a 32-bit
# mtools' @@offset wraps at 4 GiB, and the A21m's starts at 9.99 GB, where
# the first two bundles read nothing.
truncate -s 12G "$D"
printf 'label: dos\nunit: sectors\nstart=2048, size=16777216, type=7\n' |
    sfdisk -q "$D"
"$MK" station install "$D" --bios --legacy --no-scan \
    --stage3 build/bin/falcon-bios-station-legacy-stage3.bin \
    --mods build/bin/mod --memtest "$MEMTEST" --uki build/test/fixture-uki.efi \
    --backup "$R/install.bak" >"$R/install.log" 2>&1 ||
    { tail -20 "$R/install.log"; exit 1; }
read -r _ _ _ start < <("$MK" ident "$D" | awk '$2 == "boot"')
IMG="$D@@$((start * 512))"
dd if=build/bin/falcon-bios-station-stage3.bin of="$D" bs=512 \
    seek=$((start + 32)) conv=notrunc status=none
for m in ATA EHCI UHCI OHCI; do mdel -i "$IMG" "::/$m.MOD"; done
mcopy -i "$IMG" ::/STATION.INI "$R/ini.before"
UKI=$(mdir -b -i "$IMG" ::/uki | head -1 | xargs basename)
mcopy -i "$IMG" "::/uki/$UKI" "$R/uki.before"

# update, on a block device as the machine's disk is, so mtools goes
# through the partition's own node as it does there.
WHY=
LOOP=$(sudo losetup -P -f --show "$D")
sudo udevadm settle 2>/dev/null || sleep 1
if ! sudo sh "$B/update.sh" "$LOOP" --yes >"$R/update.log" 2>&1; then
    WHY="update.sh failed ($R/update.log)"
fi
sudo losetup -d "$LOOP"
if [[ -z "$WHY" ]]; then
    S=$B/stage3-legacy.bin
    dd if="$D" bs=512 skip=$((start + 32)) \
        count=$(( ($(stat -c%s "$S") + 511) / 512 )) status=none |
        head -c "$(stat -c%s "$S")" | cmp -s - "$S" ||
        WHY="the stage 3 on disk is not the bundle's"
fi
mcopy -o -i "$IMG" ::/STATION.INI "$R/ini.after"
[[ -z "$WHY" ]] && ! cmp -s "$R/ini.before" "$R/ini.after" &&
    WHY="STATION.INI changed"
if [[ -z "$WHY" ]]; then
    mcopy -o -i "$IMG" "::/uki/$UKI" "$R/uki.after"
    cmp -s "$R/uki.before" "$R/uki.after" || WHY="/uki changed"
fi
if [[ -z "$WHY" ]]; then
    for m in ATA EHCI UHCI OHCI PCC; do
        mdir -i "$IMG" "::/$m.MOD" >/dev/null 2>&1 ||
            WHY="$m.MOD was not put back"
    done
fi
result update "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
    "${WHY:-stage 3 from the bundle, STATION.INI and /uki unchanged, drivers back}"

# boot
: > "$R/com1.log"
timeout 25 qemu-system-i386 -machine pc -cpu pentium3 -m 256 -display none \
    -net none -drive file="$D",format=raw,if=ide \
    -serial file:"$R/com1.log" >/dev/null 2>&1 || true
text=$(tr -d '\r' < "$R/com1.log" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]/ /g')
WHY=
grep -aqF "falcon-boot $VERSION" <<<"$text" ||
    WHY="the menu is not the bundle's version ($VERSION)"
[[ -z "$WHY" ]] && ! grep -aqF '/_/  \__,_/_/\___/' <<<"$text" &&
    WHY="no logo on the menu"
result boot "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
    "${WHY:-the menu is $VERSION, with the logo}"

# refuse
WHY=
cp "$D" "$R/stick.img"
mdel -i "$R/stick.img@@$((start * 512))" ::/STATION.INI
if sh "$B/update.sh" "$ROOT/$R/stick.img" --yes >"$R/refuse-stick.log" 2>&1; then
    WHY="updated a boot partition that is not a station's"
fi
if [[ -z "$WHY" ]]; then
    echo tampered >> "$B/VERSION"
    if sh "$B/update.sh" "$ROOT/$D" --yes >"$R/refuse-manifest.log" 2>&1; then
        WHY="ran with a MANIFEST that does not match"
    fi
fi
result refuse "$([[ -z "$WHY" ]] && echo 1 || echo 0)" \
    "${WHY:-a stick boot partition and a tampered bundle both refused}"

exit "$FAILED"
