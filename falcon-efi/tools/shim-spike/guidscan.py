#!/usr/bin/env python3
"""Scan PE binaries for the little-endian byte encoding of EFI GUIDs.

An EFI_GUID is stored as {UINT32 Data1; UINT16 Data2; UINT16 Data3;
UINT8 Data4[8]} so the first three fields are little-endian and the
last eight bytes are literal. A GUID a distro's shim references shows
up as those exact 16 bytes somewhere in .data or .rodata, which is
cheap evidence for "this build knows about that protocol" without
running anything.

Absence is stronger evidence than presence: a GUID that is not in the
image cannot be installed or looked up by it. Presence only says the
constant is linked in; the section it lands in narrows that further, so
the offsets are reported against the section map.

Presence alone over-reports: shim's lib/guid.c defines every GUID it
knows in one always-compiled table, so a GUID sits in .data whether or
not any code path uses it. --xref answers the sharper question by
disassembling .text and counting instructions that take the address of
each GUID symbol; distro shims keep their symbol table, so a zero count
means no code references it. That is what separates "linked in" from
"actually installed or looked up".

Usage: guidscan.py [--xref] FILE [FILE...]
"""

import struct
import subprocess
import sys

# name -> GUID string. Sources:
#   shim lib/guid.c (rhboot/shim), UEFI spec for the arch protocols.
GUIDS = {
    "SHIM_LOCK": "605dab50-e046-4300-abb6-3dd810dd8b23",
    "SHIM_IMAGE_LOADER": "1f492041-fadb-4e59-9e57-7cafe73a55ab",
    "SHIM_LOADED_IMAGE": "6e6baeb8-7108-4179-949d-a3493415ec97",
    "EFI_SECURITY_ARCH": "a46423e3-4617-49f1-b9ff-d1bfa9115839",
    "EFI_SECURITY2_ARCH": "94ab2f58-1438-4ef1-9152-18941a3a0e68",
    "EFI_LOADED_IMAGE": "5b1b31a1-9562-11d2-8e3f-00a0c969723b",
    "EFI_MEMORY_ATTRIBUTE": "f4560cf6-40ec-4b4a-a192-bf1d57d0b189",
    "MOK_VARIABLE_STORE": "c451ed2b-9694-45d3-baba-ed9f8988a389",
    "EFI_GLOBAL_VARIABLE": "8be4df61-93ca-11d2-aa0d-00e098032b8c",
    "SHIM_SECURITY_ARCH_OVERRIDE_MARKER": None,  # placeholder, unused
}
GUIDS = {k: v for k, v in GUIDS.items() if v}


def guid_bytes(s):
    a, b, c, d, e = s.split("-")
    return (
        struct.pack("<IHH", int(a, 16), int(b, 16), int(c, 16))
        + bytes.fromhex(d)
        + bytes.fromhex(e)
    )


def sections(path):
    """(name, file_off, size) from objdump -h, for locating a hit."""
    out = []
    try:
        text = subprocess.run(
            ["objdump", "-h", path], capture_output=True, text=True, check=True
        ).stdout
    except Exception:
        return out
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 7 and f[0].isdigit():
            try:
                out.append((f[1], int(f[5], 16), int(f[2], 16)))
            except ValueError:
                pass
    return out


def where(secs, off):
    for name, start, size in secs:
        if start <= off < start + size:
            return "%s+0x%x" % (name, off - start)
    return "raw+0x%x" % off


def find_all(blob, needle):
    hits, i = [], blob.find(needle)
    while i != -1:
        hits.append(i)
        i = blob.find(needle, i + 1)
    return hits


# Symbol names shim's lib/guid.c gives the GUIDs above, for --xref.
XREF_SYMS = {
    "SHIM_LOCK": "SHIM_LOCK_GUID",
    "SHIM_IMAGE_LOADER": "SHIM_IMAGE_LOADER_GUID",
    "SHIM_LOADED_IMAGE": "SHIM_LOADED_IMAGE_GUID",
    "EFI_SECURITY_ARCH": "SECURITY_PROTOCOL_GUID",
    "EFI_SECURITY2_ARCH": "SECURITY2_PROTOCOL_GUID",
}

# Function symbols that distinguish the Section 7.6 branches.
BRANCH_SYMS = [
    "security_policy_install",  # branch A: OVERRIDE_SECURITY_POLICY built in
    "security_policy_uninstall",
    "init_image_loader",  # branch B: image-loader protocol published
    "shim_load_image",
    "shim_start_image",
    "hook_system_services",  # the gBS->LoadImage patch
    "verify_buffer",  # branch C: what shim_lock->Verify calls
]


def xrefs(path):
    """symbol name -> number of .text instructions taking its address."""
    counts = {}
    try:
        text = subprocess.run(
            ["objdump", "-d", path], capture_output=True, text=True, check=True
        ).stdout
    except Exception:
        return counts
    for line in text.splitlines():
        tail = line.rsplit("<", 1)
        if len(tail) == 2 and tail[1].endswith(">") and "#" in line:
            counts[tail[1][:-1]] = counts.get(tail[1][:-1], 0) + 1
    return counts


def defined_syms(path):
    out = set()
    try:
        text = subprocess.run(
            ["nm", path], capture_output=True, text=True, check=True
        ).stdout
    except Exception:
        return out
    for line in text.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in ("T", "t"):
            out.add(f[2])
    return out


def main(argv):
    argv = list(argv)
    do_xref = "--xref" in argv
    if do_xref:
        argv.remove("--xref")
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    rc = 0
    for path in argv[1:]:
        try:
            with open(path, "rb") as fh:
                blob = fh.read()
        except OSError as exc:
            sys.stderr.write("ERROR: %s\n" % exc)
            rc = 1
            continue
        secs = sections(path)
        print("===== %s (%d bytes)" % (path, len(blob)))
        for name, guid in sorted(GUIDS.items()):
            hits = find_all(blob, guid_bytes(guid))
            if hits:
                locs = ", ".join(where(secs, h) for h in hits[:4])
                if len(hits) > 4:
                    locs += ", ..."
                print("  PRESENT %-22s %s  x%d [%s]" % (name, guid, len(hits), locs))
            else:
                print("  absent  %-22s %s" % (name, guid))
        if do_xref:
            xr = xrefs(path)
            syms = defined_syms(path)
            print("  -- .text references to GUID symbols:")
            for name in sorted(XREF_SYMS):
                sym = XREF_SYMS[name]
                print("     %-24s %d" % (sym, xr.get(sym, 0)))
            print("  -- branch-discriminating function symbols:")
            for sym in BRANCH_SYMS:
                print("     %-26s %s" % (sym, "defined" if sym in syms else "-"))
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
