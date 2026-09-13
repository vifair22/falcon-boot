#!/usr/bin/env python3
"""Append a fixed-VHD footer to a raw image (in place).

The whole trick under test: a fixed VHD is raw data plus this 512-byte
trailer, and everything that reads the data by its own length (ISO9660/UDF
readers included) ignores trailing bytes. So ISO + footer is simultaneously
a valid ISO and a diskpart-attachable VHD. If WinPE's udfs.sys mounts the
UDF volume off the attached disk-class device, the Windows injector needs
no driver at all (DESIGN: Windows route A spike).

Usage: vhdfoot.py IMAGE   (appends in place; refuses if a footer is present)
"""

import struct
import sys
import uuid


def chs(size):
    # Algorithm from the VHD spec (Virtual Hard Disk Image Format 1.0,
    # "CHS Calculation"), which attach implementations expect verbatim.
    sectors = size // 512
    if sectors > 65535 * 16 * 255:
        sectors = 65535 * 16 * 255
    if sectors >= 65535 * 16 * 63:
        spt, heads = 255, 16
        cth = sectors // spt
    else:
        spt = 17
        cth = sectors // spt
        heads = max((cth + 1023) // 1024, 4)
        if cth >= heads * 1024 or heads > 16:
            spt, heads = 31, 16
            cth = sectors // spt
        if cth >= heads * 1024:
            spt, heads = 63, 16
            cth = sectors // spt
    return cth // heads, heads, spt


def footer(size):
    cyl, heads, spt = chs(size)
    # The checksum field is packed as a zero placeholder and filled in after,
    # because the checksum is defined over the footer with that field zeroed.
    # It must be present in the pack, not spliced in later: leaving it out
    # shifts every field after it and yields a 508-byte footer that readers
    # reject as corrupt.
    f = struct.pack(
        ">8sIIQI4sI4sQQHBBII16sB427x",
        b"conectix",          # cookie
        0x00000002,           # features: reserved bit, always set
        0x00010000,           # format version 1.0
        0xFFFFFFFFFFFFFFFF,   # data offset: none for fixed disks
        0,                    # timestamp (epoch 2000; zero is legal)
        b"flcn",              # creator application
        0x00010000,           # creator version
        b"Wi2k",              # creator OS: the value Windows tools expect
        size,                 # original size
        size,                 # current size
        cyl, heads, spt,      # geometry
        2,                    # disk type: fixed
        0,                    # checksum placeholder
        uuid.uuid4().bytes,   # unique id
        0,                    # saved state
    )
    if len(f) != 512:
        raise AssertionError(f"footer is {len(f)} bytes, must be 512")
    checksum = (~sum(f)) & 0xFFFFFFFF
    return f[:64] + struct.pack(">I", checksum) + f[68:]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    with open(path, "r+b") as img:
        img.seek(0, 2)
        size = img.tell()
        if size % 512 != 0:
            sys.exit(f"{path}: size {size} is not a multiple of 512")
        img.seek(-512, 2)
        if img.read(8) == b"conectix":
            sys.exit(f"{path}: already has a VHD footer")
        img.seek(0, 2)
        img.write(footer(size))
    print(f"{path}: fixed-VHD footer appended (data size {size})")


if __name__ == "__main__":
    main()
