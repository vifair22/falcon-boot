# BSD media: the route, measured (2026-09-19)

Decided 2026-09-19: BSD media is first class, by the memory-disk route below,
with the GEOM module kept as the fallback if the memory cost proves to matter.
Roadmap milestone M7.

## The problem, which is Section 6.1's

A FreeBSD ISO chainloads fine: the ISO's `loader.efi` runs, reads the kernel
off the virtual CD and starts it. Then `ExitBootServices` takes the virtual CD
away, and the kernel has no cd9660 device to mount root from:

    mountroot: waiting for device /dev/iso9660/15_1_RELEASE_AMD64_BO...
    Mounting from cd9660:/dev/iso9660/15_1_RELEASE_AMD64_BO failed with error 19.

falcon-hook cannot help: it is a Linux initramfs and device-mapper mechanism.
This is what FreeBSD, GhostBSD, pfSense, OPNsense, TrueNAS CORE and the rest of
the family all do, so one answer covers the family.

## What Ventoy does, and why it is the fallback here

Ventoy ships a per-version `geom_ventoy.ko`, appends `ventoy_load="YES"` and
the ISO's extents to `loader.conf`, and the module builds a GEOM provider over
those extents (`docs/notes/ventoy-survey.md`, the FreeBSD row). No RAM copy,
but a kernel module per FreeBSD major, built in a FreeBSD VM, and the
derivatives (pfSense, OPNsense) ship their own kernels whose module ABI does
not match stock FreeBSD's. That is a build pipeline and a per-release
treadmill, which is what this project's coverage model exists to avoid.

## The route taken: the ISO as a memory disk, read from itself

FreeBSD's loader can load an arbitrary file as a memory disk and boot from it.
No kernel module, nothing per-version, and it works on every derivative that
still has FreeBSD's loader:

    md_image_load="YES"
    md_image_type="md_image"
    md_image_name="<path>"
    vfs.root.mountfrom="cd9660:/dev/md0"

The file it loads is the ISO, and the path it loads it from is **inside the
ISO**: one directory record is added to the medium naming a file whose extent
is LBA 0 and whose length is the whole image, so the ISO contains itself, and
the loader reads it off the same virtual CD it booted from. Two byte edits,
both served through the overlay the virtual CD already has
(falcon-efi/src/iso/overlay.h), so nothing on p2 is written:

1. **The record**, appended into the free bytes at the end of the root
   directory's extent, or /boot's if the root is full. No record moves, no
   length changes. `falcon-core/src/media/bsdmd.c`, `bsdmd_record`.
2. **The four settings**, written over the opening comment block of the ISO's
   `/boot/defaults/loader.conf` and padded back to exactly the length they
   replaced, the same in-place trick D3b uses for `grub.cfg`. That block is
   442 bytes on FreeBSD, pfSense and OPNsense alike and the settings are 114.
   `/boot/loader.conf` is the wrong file: a few hundred bytes with no comment
   block to spend. `bsdmd_config`.

The loader's half is `falcon-efi/src/bsd/bsd.c`, reached from the menu for a
`FAMILY_BSD` medium on Enter.

### The route first designed, and why it was dropped

The first version published a **second Block I/O device**: a synthetic GPT
with one FAT32 partition whose single file was the ISO, served from p2 the way
the virtual CD serves it. Measured working on 2026-09-19, and dropped the same
day. `md_image_name` needs a loader device name (`disk0p1`), that index is the
loader's enumeration order, and nothing here can predict it: the real stick is
a disk too and a machine may have several. Escaping that meant patching the
ISO's Lua to search `disk0p1`..`disk7p1`, which is a second in-place patch of
a file that differs across FreeBSD 13, 14, 15 and every derivative. Reading
the file off `currdev` needs no device name at all, and no second device.

Also measured on the way: FreeBSD's loader probes filesystems only inside
partitions, so a bare FAT volume with no partition table is invisible to it
(`lsdev` showed `disk0` with nothing under it; with a GPT, `disk0p1: EFI`).
That is why the dropped route needed a whole GPT rather than a superfloppy.

### The name carries its ISO9660 version

`ls /` at the loader prompt shows `FALCONBS.IMG;1` beside the Rock Ridge names
of everything else on the medium: the added record has no Rock Ridge name, so
the loader matches its ISO name verbatim, `;1` included. `md_image_name`
without the `;1` does not resolve ("can't find '/FALCONBS.IMG'"). The name in
`bsdmd.h` carries it.

## Measured

Under OVMF through the coverage harness (`tools/coverage/run-corpus.sh`),
2026-09-19, against loader 0.1.0_20260919.2102:

| medium | size | SB off | SB on, MS CA | SB on, own keys |
|---|---|---|---|---|
| FreeBSD 15.1 bootonly | 521 MB | PASS 24 s | REFUSED | REFUSED |
| FreeBSD 15.1, blessed | | PASS 25 s | PASS 22 s | PASS 22 s |
| pfSense Plus 24.06-BETA7 | 1369 MB | PASS 98 s | FAIL | FAIL |
| pfSense Plus, blessed | | PASS 96 s | FAIL | FAIL |
| OPNsense 25.1 dvd | 2114 MB | refused by the guard, 5 s | same | same |

REFUSED is the designed answer for an unblessed BSD medium with Secure Boot
on: its loader is unsigned and the platform declines it
(`EFI_SECURITY_VIOLATION`). Blessing is the route for that state, and it
works.

pfSense puts its installer on the serial console and leaves an error about
the daemon it cannot reach on the VGA one. That is pfSense, not this route: a
control run of the stock ISO as a plain CD, with no falcon-boot in the picture
at all, reaches the same two consoles saying the same two things. It is why
`run-case.sh` reads a BSD installer's prompt as proof of userland, the way it
already reads a getty's: these installers never bring up the network by
themselves, so the DHCP signal that judges a Linux live image never fires.

## pfSense under Secure Boot: shim will not parse its loader

pfSense fails in both Secure Boot states, drop-and-boot and blessed alike,
with `EFI_UNSUPPORTED` from `LoadImage`. It is not a signature problem and
blessing does not help, because shim never gets as far as the signature.

Its `/boot/loader.efi` carries a section named `set_Xfic` with
`SizeOfRawData` 0 and `PointerToRawData` 0, while its characteristics
(`0xc0000040`) claim initialized data. shim's PE verifier refuses an image
like that outright. FreeBSD 15.1's own loader has no such section, which is
why it parses and gets as far as being refused on its signature.

With Secure Boot off the firmware's loader accepts the same bytes, which is
what the firmware-first path in `bless_boot_as` and in the chainload is for
(`falcon-efi/src/iso/iso.c`), and both pfSense cases PASS there. With Secure
Boot on, shim is the only path and there is nothing to do about it here:
rewriting a vendor's PE headers to get past a verifier is not a thing this
loader does. The doors are the platform ones, Secure Boot off or
`mokutil --disable-validation`.

## The memory ceiling, which is FreeBSD's

The image is copied into the loader's staging area, and that area has to live
under 4 GB because that is as far as the amd64 kernel's first page tables
reach (`stand/efi/loader/copy.c`, `get_staging_max`: 1 GB with `copy_staging`
enabled, 4 GB without). The staging area also holds the kernel and its
modules, and ends with 8 MB of slop for the kernel's earliest allocations.

OPNsense's 2.2 GB DVD does not fit and cannot be made to: it failed with 4 GB
and with 8 GB of guest RAM, with QEMU's below-4G window raised from its
default 2.75 GB to 3.5 GB, and with `exec="copy_staging disable"` added to the
patched settings. The image is simply too large for what has to be one
contiguous run below 4 GB once the firmware has taken its share.

So the loader checks before it serves anything, by doing what FreeBSD's loader
will do: `AllocatePages(AllocateMaxAddress, ...)` for the image's size plus
192 MB below 4 GB, and an immediate `FreePages`. An allocation IS the
question; an estimate from the memory map would have to model the firmware's
own fragmentation. When it fails the user is told, in those words, and told to
press `c` for the ISO's own loader, rather than watching the boot die halfway
through a copy with a message only FreeBSD's source explains.

## Secure Boot: blessing a BSD ISO (part 2 of M7, built 2026-09-19)

FreeBSD's `loader.efi` is unsigned, so under Secure Boot the platform refuses
it exactly as it refuses Arch's. Blessing today signs a Linux kernel into a
UKI wrap, which has nothing to wrap here: a UKI carries a kernel and its
initramfs, and a FreeBSD medium has neither to give. The BSD form of the same
act is to sign the medium's own loader:

- **The sign tool** reads `/boot/loader.efi` out of the ISO with falcon-core's
  ISO9660 reader, signs it with `sbsign` and the user's MOK, and writes it to
  `p2:/falcon/blessed/<iso>.efi` with an ordinary manifest record naming it
  (`family=bsd`). No sd-stub, no cmdline, no initramfs: `--cmdline-extra` is
  ignored and said to be, because nothing here chooses kernel arguments.
- **The loader** publishes the medium with M7 part 1's two edits and starts
  the signed copy in place of the medium's own loader. The ceremony is the one
  every blessing uses and it batches the same way: `b` blesses the row, `a`
  blesses every unblessed ISO on the stick, the appliance signs the whole list
  in one run, and the machine reboots once. Signing happens there, never in
  the loader, because the key lives on p3.

### The measurement that decided how it is launched

A blessed artifact is normally published on the synthetic filesystem of
Section 5.1 and loaded from there by device path. Measured 2026-09-19, that
does not work here: the signed loader starts, the platform having verified
it, and then stops at

    Failed to find bootable partition
    ERROR: cannot open /boot/lua/loader.lua: invalid argument.

FreeBSD's `find_currdev` (stand/efi/loader/main.c) works out the device the
loader came from, and it has no global scan of block devices: it tries
`rootdev`, the boot manager's own `Boot####` device path, the partitions of
the boot device, and then the handle its image was loaded from and that
handle's ancestors. The synthetic filesystem has no Block I/O behind it and
no ancestors, so every one of those comes up empty. It is also why the
unblessed route works without any of this: there the image IS the medium's
own loader, loaded off the medium.

So the blessed loader is named to the platform as the medium's own
`\EFI\BOOT\BOOTX64.EFI` (`iso_boot_path`) while its BYTES are handed over
as a buffer, the signed copy read off p2 (`bless_boot_as`). The spec allows
both a FilePath and a SourceBuffer; the firmware resolves the path to a
DeviceHandle without reading it. What the platform verifies is the buffer,
which is the signed image; the path only says which device the started image
should treat as its own. Measured: `Load Device: VenHw(...)/CDROM(0x1)`,
`Setting currdev to cd0`, the memory disk loads and the installer comes up.

**Measured, blessed**, FreeBSD 15.1 through the harness: PASS in every
Secure Boot state, 22 s with only the user's MOK trusted and 22 s with the
Microsoft CA present as well. pfSense is blessed by the same code and PASSes
with Secure Boot off; under Secure Boot on it is stopped by shim before its
signature is looked at, for the reason above.

**What blessing a BSD ISO means, stated plainly**: the user's key vouches for
the ISO's loader, and that loader then loads a kernel and modules that nothing
verifies. That is FreeBSD's own design, not something this adds, but it is a
weaker chain than a blessed Linux wrap, where the signature covers the kernel
and its initramfs together.

## Corpus

`freebsd-bootonly-15.1`, `opnsense-25.1` and `pfsense-plus-24.06b7` are pinned
(Tier C). The first is the development target because it is the smallest; the
other two are what the route has to prove on, since their kernels are their
own.
