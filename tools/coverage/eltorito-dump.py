#!/usr/bin/env python3
"""Print an ISO's El Torito boot catalog: one line per entry.

A reading aid for corpus work (which boot image a medium offers a BIOS, and
in what emulation), independent of falcon-core's parser on purpose so the
two can be compared. Usage: eltorito-dump.py ISO...
"""
import struct
import sys

MEDIA = {0: "no-emulation", 1: "1.2M floppy", 2: "1.44M floppy",
         3: "2.88M floppy", 4: "hard disk"}
PLATFORM = {0: "x86", 1: "PowerPC", 2: "Mac", 0xEF: "UEFI"}


def dump(path):
    with open(path, "rb") as f:
        f.seek(17 * 2048)
        brvd = f.read(2048)
        if brvd[0] != 0 or brvd[1:6] != b"CD001" or \
                not brvd[7:30].startswith(b"EL TORITO SPECIFICATION"):
            print(f"{path}: no El Torito boot record")
            return
        cat_lba = struct.unpack_from("<I", brvd, 0x47)[0]
        f.seek(cat_lba * 2048)
        cat = f.read(2048)
    print(f"{path}: catalog at sector {cat_lba}")
    platform = cat[1]
    i = 32
    first = True
    while i + 32 <= len(cat):
        e = cat[i:i + 32]
        if e[0] in (0x90, 0x91):
            platform = e[1]
            i += 32
            continue
        if e[0] not in (0x88, 0x00) or (not first and e == b"\0" * 32):
            break
        boot, media, seg, sysid, _, count, lba = struct.unpack_from(
            "<BBHBBHI", e, 0)
        if first or boot == 0x88 or lba:
            print("  %-8s %-13s %-12s load_seg=%#06x sectors=%-5d lba=%d" % (
                "default" if first else ("boot" if boot == 0x88 else "noboot"),
                PLATFORM.get(platform, hex(platform)),
                MEDIA.get(media & 0x0F, hex(media)), seg, count, lba))
        first = False
        i += 32


for p in sys.argv[1:]:
    dump(p)
