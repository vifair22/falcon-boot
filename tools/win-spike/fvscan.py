#!/usr/bin/env python3
"""Scan an EDK2 firmware volume image for LZMA-compressed sections, decompress
them, and list the module UI names inside. Used to answer "does this OVMF
build contain UdfDxe".

Usage: fvscan.py OVMF_CODE.fd [name ...]
"""
import lzma
import re
import sys

LZMA_GUID = bytes.fromhex("98584eee143959429d6edc7bd79403cf")


def decomp_all(data):
    out = []
    for m in re.finditer(re.escape(LZMA_GUID), data):
        # GUIDed section: hdr(4) guid(16) dataoff(2) attr(2) then payload
        base = m.start() - 4
        if base < 0:
            continue
        dataoff = int.from_bytes(data[base + 20:base + 22], "little")
        p = base + dataoff
        # LZMA alone: 5 props + 8 size
        try:
            d = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
            out.append(d.decompress(data[p:]))
        except Exception as e:
            sys.stderr.write("lzma at 0x%x failed: %s\n" % (p, e))
    return out


def ui_names(blob):
    names = set()
    # EFI_SECTION_USER_INTERFACE == 0x15; find UCS-2 ascii-ish runs
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){3,60}\x00\x00", blob):
        s = m.group(0)[:-2].decode("utf-16-le")
        names.add(s)
    return names


def main():
    data = open(sys.argv[1], "rb").read()
    wanted = [w.lower() for w in sys.argv[2:]]
    blobs = decomp_all(data)
    sys.stderr.write("decompressed %d blob(s), %d bytes\n"
                     % (len(blobs), sum(len(b) for b in blobs)))
    names = set()
    for b in blobs:
        names |= ui_names(b)
        # nested volumes may contain further lzma sections
        for nb in decomp_all(b):
            names |= ui_names(nb)
    names |= ui_names(data)
    for n in sorted(names):
        if not wanted or any(w in n.lower() for w in wanted):
            print(n)


main()
