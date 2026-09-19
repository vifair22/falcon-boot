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

## The route taken: the ISO as a memory disk

FreeBSD's loader can load an arbitrary file as a memory disk and boot from it.
No kernel module, nothing per-version, and it works on every derivative that
still has FreeBSD's loader:

    md_image_load="YES"
    md_image_type="md_image"
    md_image_name="<device>:/falcon-iso.img"
    vfs.root.mountfrom="cd9660:/dev/md0"

**Measured end to end on 2026-09-19** with FreeBSD 15.1's bootonly ISO under
OVMF, Secure Boot off: with those four lines patched into the ISO's
`/boot/defaults/loader.conf` and the ISO itself present as a file on a second
disk, the loader printed `Loading configured modules... disk0p1:/iso.img` and
the installer came up on its Welcome screen, root mounted from `/dev/md0`.

What the loader needs from us, then:

1. **A device it can read the ISO from.** It reads msdosfs, and p2 is exFAT, so
   the loader publishes a second synthetic Block I/O device: a GPT with one
   FAT32 partition holding one file, whose data is the ISO's extents on p2,
   served the way the virtual CD serves them. Measured: a whole-disk FAT is NOT
   enough, the loader probes filesystems only inside partitions (`lsdev` showed
   `disk0` with nothing under it); with a GPT it showed `disk0p1: EFI` and read
   the file.
2. **The four settings above, patched into the served ISO.** The same in-place
   byte patch D3b uses for `grub.cfg`: `/boot/defaults/loader.conf` is ~10 KB
   and opens with ~440 bytes of comments, which is room enough.
   `/boot/loader.conf` is only 116 bytes, so it is not the one to patch.
3. **A memory guard.** The image is copied into RAM by the loader, so the boot
   needs free memory of the ISO's size plus the kernel's own needs: about
   550 MB for FreeBSD bootonly, 1.4 GB for pfSense, 2.2 GB for OPNsense. The
   loader checks the memory map before it offers this route and says so plainly
   when there is not enough, rather than letting the loader die mid-copy.

## Open question: naming the device

`md_image_name` needs a loader device name (`disk0p1`), and that index is the
loader's enumeration order, which we cannot predict: the real stick is a disk
too, and a machine may have several. Candidates, in preference order:

1. Patch a Lua file (FreeBSD 13+ ships `/boot/lua`) to try `disk0p1` through
   `disk7p1` until one has `/falcon-iso.img`, then set `md_image_name`. Costs
   one more in-place patch and keeps the config declarative.
2. Add a file to `/boot/loader.conf.d/` instead of patching defaults: the
   directory exists and is empty, so a directory record fits in its extent's
   spare bytes. Same naming problem, but a cleaner place to write.
3. Have the loader publish the FAT device so it sorts first. Not controllable:
   the order is the firmware's.

Option 1 is the plan.

## Secure Boot: blessing a BSD ISO (part 2 of M7)

FreeBSD's `loader.efi` is unsigned, so under Secure Boot the platform refuses
it exactly as it refuses Arch's. Blessing today signs a Linux kernel into a
UKI wrap, which has nothing to wrap here. The BSD form of the same act is:
the sign tool takes the ISO's own `loader.efi`, signs it with the user's MOK,
and the loader launches that signed copy with the patched ISO served beside
it. The ceremony is the one every blessing uses and it batches the same way:
`b` blesses the row, `a` blesses every unblessed ISO on the stick, the
appliance signs the whole list in one run, and the machine reboots once.
Signing happens there, never in the loader, because the key lives on p3.

What it needs:

- **The sign tool**: read `/boot/loader.efi` out of the ISO with falcon-core's
  ISO9660 reader, sign it with `sbsign` the way a bare payload is signed
  (falcon-cli/src/sign/payload.c already signs PE images in place), and write
  it to `p2:/falcon/blessed/<iso>.efi` with a manifest record naming it. The
  record's existing shape fits: the artifact to launch is a field the blessed
  path already has.
- **The loader**: on a blessed BSD entry, publish the virtual CD and the FAT
  device as M7's part 1 does, then start the signed loader instead of the
  ISO's own.
- **A measurement first**: FreeBSD's loader works out its own device
  (`currdev`) from the device it was loaded from. Started off the synthetic
  filesystem of Section 5.1, which has no Block I/O, it may not find one and
  may need the virtual CD named to it instead. The alternative is patching the
  signed bytes into the El Torito image the ISO already carries (2 MB FAT,
  ~650 KB used, so a signature's worth of growth fits, at the cost of a FAT
  directory and chain edit in the served bytes). Measure before choosing.

**What blessing a BSD ISO means, stated plainly**: the user's key vouches for
the ISO's loader, and that loader then loads a kernel and modules that nothing
verifies. That is FreeBSD's own design, not something this adds, but it is a
weaker chain than a blessed Linux wrap, where the signature covers the kernel
and its initramfs together. DESIGN Section 7.7 gets a row for it when it is
built.

## Corpus

`freebsd-bootonly-15.1`, `opnsense-25.1` and `pfsense-plus-24.06b7` are pinned
(Tier C). The first is the development target because it is the smallest; the
other two are what the route has to prove on, since their kernels are their
own.
