# BIOS mode: the scope, measured against the tree (2026-09-19)

Status (2026-09-24): decided, built and in the scorecard. falcon-bios boots
all five routes, is i686 in 32-bit protected mode since Section 6h, and is
judged in two coverage columns, `bios` and `bios32`. DESIGN Section 12.3 is
the current design; this note is how it got there, and its sections are
dated because the early ones describe a loader that ran in long mode.
Where a passage has since stopped being true it says so in place.

This began as the scoping study behind "what would a BIOS-capable
falcon-boot actually cost", written down because the answer that had been
assumed is wrong and the right answer is cheap enough to be asked again.

Since then it is built (2026-09-20): `falcon-bios/` boots all five of the
routes Section 4 describes - a Linux ISO directly with falcon-hook, an ISO's
own bootloader through a resident INT 13h handler, the personal OS unwrapped
out of its UKI, BSD media as their own memory disk, and Windows with its
`boot.wim` patched in memory. It boots under SeaBIOS and reads its own boot sector
back through the BIOS from 64-bit code (it ran in long mode until Section
6h):

    falcon-bios: entering long mode
    falcon-bios: running in 64-bit long mode
    falcon-bios: reading LBA 0 through the BIOS from long mode
    falcon-bios: thunk ok, boot sector signature 0xAA55

And then, booted against a real four-partition falcon stick:

    falcon-bios:   GPT with 4 partition(s)
    falcon-bios:     p2  LBA 133120..264191  [falcon p2]
    falcon-bios:     p3  LBA 264192..395263  [falcon p3]
    falcon-bios:   p2 mounted, 15872 clusters of 4096 bytes
    falcon-bios:     archlinux-fake.iso  1048576 bytes
    falcon-bios:     systemrescue-fake.iso  1048576 bytes
    falcon-bios:     payload-test.iso  8765440 bytes

That is `part.c` and `exfat.c` from falcon-core, unmodified, over a read
callback whose other end is INT 13h, identifying p2 and p3 by DESIGN Section
3's own rules. It is the menu's data.

All five of Section 5.3's assembly jobs are done: the MBR, the mode switch,
the thunk, the jump into a loaded kernel, and the resident's prologue. So are
all five of Section 4's boot routes. The thunk is the one that mattered most, because everything
above it is C. Section 6a records what the ISO reader work had already built,
and Section 6b the third stage that took the size ceiling off.

Reproduce it with:

    make test-stick ARCH_ISO=<any Linux ISO>
    make test-bios-img
    falcon-efi/tools/run-qemu.sh --bios --headless \
        -drive format=raw,file=build/test-bios.img \
        -drive format=raw,file=build/test-stick.img

There is a menu, too, driven by real keystrokes through the QEMU monitor the
way the UEFI rig's tests are:

    falcon-boot (BIOS)

        archlinux-fake.iso   1 MiB
      > systemrescue-fake.iso   1 MiB
        payload-test.iso   8 MiB

      up and down to move, enter to select

Screen and keyboard are INT 10h and INT 16h through the thunk, one call per
character, which is slow and is the right trade: the alternative is a VGA
driver and a PS/2 driver of our own, and neither would work on a machine
whose firmware is doing something unusual. (Superseded for the screen by
Section 6g: stage 3 writes VGA text memory, which needs no driver.)

*The next three paragraphs are the state on 2026-09-20, the day the
first stage booted. All of it has been done since, the route was decided
once 5.4 turned out wrong, and `make test` runs falcon-bios's three
host-testable halves (`vcdcore.c`, `linuxcore.c`, `e820.c`).*

What is left in S1 is the shared menu model (open question 2, and the moment
to move it is when this build boots something, not before), the ISO scan's
depth and filter rules
(`falcon-efi/src/efi/main.c` has them and they are not EFI-shaped), the boot
paths, and the resident INT 13h hook of Section 7.1, which is still a spike
rather than an estimate. The ceiling stage 2 is now up against is its own:
about 33 KB, because it is linked where real mode and long mode agree on what
an address means. Past that the answer is a third stage above a megabyte,
entered after paging, not a bigger stage 2.

The route is still undecided, and Section 7.3 is why: BIOS boot from a GPT
stick needs a BIOS boot partition, which remakes every stick already written.
Nothing in `falcon-bios/` is on the path of `all` or of `make test`.

Read `coverage-roadmap.md` Section 8 for the shape this note is in: a
decision on the shelf, costed, so it can be taken whenever somebody wants it
without rediscovering any of this.

## 1. The premise that failed

BIOS support had been set aside on the belief that it meant two codebases:
one loader for BIOS, one for UEFI, with the high-level logic written twice.
The way out considered was a micro-OS. falcon-boot would exit boot services
as early as possible, take over the machine, and run everything inside its
own environment, so only a hardware abstraction layer differed per platform
and the menu, the parsers and the boot paths stayed single-sourced.

Both halves of that are wrong, and the tree says so.

**The high-level code is already single-sourced.** Measured 2026-09-19:

| component | lines of `src/` |
| --- | --- |
| `falcon-core` | 18,259 |
| `falcon-efi` | 11,809 (of which `efi/` is 2,880, mostly our own UEFI headers) |
| `falcon-cli` | 10,713 |
| `falcon-appliance` | 2,440 |
| `falcon-hook` | 1,931 |
| `falcon-winjump` | 691 |

(`falcon-core` at master bd97eb8; the in-flight `media/bsdmd.c` adds another
416, which is why a count taken in a dirty tree reads 18,675.)

falcon-core is EFI-free by construction and holds every parser, every
filesystem driver, LUKS2, GPT, PE, the trust predictions, WIM, the manifest
and the media logic. A BIOS loader links it unchanged.

**The HAL already exists, and it is not a micro-OS.** It is the read callback
every core module takes,

    int (*read)(void *ctx, uint64_t off, uint64_t len, void *buf);

plus the `fs_ops` vtable of DESIGN Section 9. Device access is a function
pointer and a context, and every buffer is the caller's. That is exactly the
"write the transport twice, share everything above it" boundary the micro-OS
was supposed to provide, and it is already drawn and already proven by three
consumers that are not the loader: the host test suite, the host CLI tools
and the signing appliance. A BIOS loader is the fourth, supplying `INT 13h`
in place of Block I/O.

## 2. The micro-OS, and why it is rejected

Recorded so it is not proposed again.

A loader that exits boot services early and boots payloads from its own
environment cannot use `gBS->LoadImage`, and `LoadImage` is the only place in
the chain where anything is verified: shim hooks it, and the shim spike
measured the acceptance table that DESIGN Section 7.6 rests on
(`shim-spike.md` Section 4.2). Booting a payload by any other route means
invariant 2 is not merely bent but absent, and on shim 15.8 the attempt is
caught anyway, because 15.8 hooks `gBS->ExitBootServices` and halts with
"Bootloader has not verified loaded image. System is compromised."
(`d3-spike.md`, limit 3, where we tripped that hook from the other
direction).

It also costs more than it saves. After `ExitBootServices` there is no
storage, no allocator, no console, no timer and no protocol database; only
runtime services survive, and they load no code. Every payload would then
have to be entered through a boot protocol we implement ourselves: the Linux
zero page, FreeBSD's loader metadata, and for Windows a UEFI implementation,
since `bootmgfw.efi` and `winload.efi` are EFI applications with no
firmware-independent entry point.

So the micro-OS trades the security model for a code-sharing win the read
callback already delivers. It is the right architecture for a hypervisor and
the wrong one for a loader whose value is that the platform does the
enforcing.

## 3. What BIOS mode is, and is not

It is: power on, a menu, and a payload booted.

It is not a smaller falcon-boot. On BIOS there is no Secure Boot, no shim, no
db or dbx, no MOK and no Authenticode verification, so the four invariants of
DESIGN Section 7 are vacuous rather than upheld. Nothing verifies, therefore
nothing needs blessing, therefore the signing appliance has no job and the
trust predictions have nothing to predict. `secboot/`, `bless/`, the trust
column and the whole appliance are absent from a BIOS build, not ported.

State that plainly wherever it is user-visible. A BIOS falcon-boot is a
different, smaller product that shares a core library and a name, and the
security story it tells is "nothing here is verified", not a weaker version
of the UEFI one.

The personal OS survives, unverified: see Section 4.4.

## 4. The chain, per payload

### 4.1 Linux, chainloaded (the minimum)

BIOS reads the El Torito boot catalog and loads the no-emulation boot image
at `0x7C00`, which for a Linux ISO is isolinux or an equivalent. It then
reads the medium over `INT 13h` for the rest of its work.

So the loader has to be the CD. A resident `INT 13h` handler publishes the
virtual medium as an optical drive, serving the same VIRTUAL address space
`falcon-efi/src/iso/overlay.h` already defines, off the p2 extents
`media/extmap.c` already computes. Syslinux's MEMDISK is this program, and
Ventoy's BIOS mode is this program; both are references worth reading before
writing a line of it.

The hook dies when the kernel leaves real mode, which is the exact analogue
of the virtual CD dying at `ExitBootServices`. falcon-hook answers it
unchanged: it runs in the initramfs, reads its extent map out of its own
cpio, and builds the dm-linear device over the real partition. It has no
opinion about firmware. All 1,931 lines and every per-distro nudge port for
free, which is the expensive half of coverage.

### 4.2 Linux, direct

Easier than under UEFI, not harder. No shim, no `LoadFile2`, no `.sbat`, no
SHIM_LOCK dance. Load the bzImage, fill `struct boot_params`, place the
initrds respecting `initrd_addr_max`, jump. `media/bootcfg.c` already finds
the kernel, initrd and cmdline in the ISO's own isolinux configuration, and
`media/cpio.c` already appends falcon-hook. The e820 map comes from
`INT 15h` AX=E820 instead of the EFI memory map.

Because the direct route is cheaper here, it is likely the primary BIOS path
with the El Torito chainload as the fallback, inverted from the UEFI menu's
defaults.

### 4.3 Windows

The easiest payload to port, which inverts the expectation. The entire
Windows strategy is already "serve a virtual CD and substitute byte ranges in
it", and none of that machinery is EFI-shaped. Only the transport is.

    BIOS -> El Torito catalog -> boot/etfsboot.com   (no emulation, real mode, 0x7C00)
         -> reads the medium over INT 13h
         -> bootmgr   (the BIOS variant, not bootmgfw.efi)
         -> protected mode, thunking back to real mode for INT 13h
         -> boot/bcd, boot/boot.sdi, sources/boot.wim -> SDI RAM disk
         -> winload.exe   (not winload.efi)
         -> WinPE kernel -> winpeshl.exe -> Setup

`etfsboot.com` takes its drive number from `DL`, so the loader sets that at
the jump and everything downstream follows it. Both boot images read the same
filesystem: a Windows 11 ISO is UDF, and `etfsboot.com` reads UDF as much as
`cdboot.efi` does, so the UDF record edits of DESIGN Section 6.4 (boot.wim's
File Entry gaining an extent past the ISO's end, the Partition Descriptors
growing to cover it, the anchor copy at the new last block) apply verbatim.

There is no media problem before Setup, for the same reason there is none
under UEFI: `bootmgr` has already loaded boot.wim into the `boot.sdi` RAM
disk, so WinPE runs from memory and does not need the hook to survive.

Setup's media problem is identical and is solved identically, with no
changes. falcon-winjump is pure Win32 userland: `enable_manage_volume()` for
the privilege, a `GetLogicalDrives()` poll until WinPE's asynchronous storage
stack surfaces the exFAT volume, verification by path and exact byte length,
then `AttachVirtualDisk`. It does not know or care how it was booted. All 691
lines port untouched, as does `windows.cfg` with its bypass and unattend
handling.

The one thing that gets simpler: nothing verifies anything, so serving
`bootmgr` and `winload` byte for byte is no longer load-bearing. Under UEFI
it is, because the platform checks Microsoft's signatures as it would from a
DVD. We would not want to patch them anyway, but the constraint is gone.

### 4.4 The personal OS

Possible, but the verb is not "chainload". A UKI is PE32+ with no BIOS entry
point. It is unwrapped instead.

1. **Unlock p3.** `luks/` and `fs/ext4.c` over the `INT 13h` read callback.
   p3's keyslot is pinned to pbkdf2 at 200,000 iterations with
   aes-xts-plain64 (`falcon-cli/src/mkstick/mkstick.h`, `create.c`) precisely
   so core's EFI-free unlock can open it. Identical code, identical prompt.
2. **Find the UKI.** As now.
3. **Split it.** `pe_find_section` returns FILE offsets, which is what this
   needs: pull `.linux`, `.initrd` and `.cmdline`. The `.linux` section is a
   plain bzImage with a perfectly good real-mode entry, so it boots by 4.2's
   route and sd-stub never runs.
4. **Rebuild the credential handoff.** sd-stub's contribution is to take
   `<uki>.efi.extra.d/cryptsetup.passphrase.cred` and synthesize a cpio
   placing it at `/.extra/credentials/cryptsetup.passphrase`, concatenated
   onto `.initrd`. That is a cpio write, and `media/cpio.c` is already the
   initrd cpio writer used to append falcon-hook. Same writer, different path
   and payload, concatenated the same way; systemd in the initrd only ever
   sees the finished cpio. So the p4 unlock with no second prompt survives
   the port.

Three things do not survive, and all three are worth stating in the note
rather than discovering on hardware:

- **Nothing verifies the UKI.** Its Authenticode signature becomes
  decorative. The gate degrades from "signed and shim-verified" to
  "encrypted and passphrase-gated", and LUKS2 with plain aes-xts carries no
  integrity, so that is confidentiality plus a passphrase, not authenticity.
- **`LoaderDevicePartUUID` does not exist**, because sd-stub sets it. If the
  personal OS's initrd leans on `systemd-gpt-auto-generator` to find root on
  the same disk as the ESP, it breaks. The fix is a `root=` on the cmdline we
  synthesize, which we control anyway since we are reading `.cmdline` and can
  append to it. This is the only one of the three that stops a boot.
- **No TPM measurement.** sd-stub measures the sections into PCR 11. Doing
  that under BIOS needs the TCG `INT 1Ah` interface, which is not worth it;
  anything sealed to PCR 11 stops unsealing.

### 4.5 BSD

Nearly free once 4.1 exists. The two byte edits of `media/bsdmd.c` (the
directory record naming the ISO as a file inside itself, and the `md_image`
settings written over `/boot/defaults/loader.conf`'s comment block) are laid
over the served medium exactly as they are under UEFI, and FreeBSD's own
loader is chainloaded through the El Torito boot image. See `bsd-route.md`.

## 5. Decisions taken in this study

**5.1 Long mode and a thunk, not a 32-bit falcon-core.** Measured
2026-09-19: falcon-core contains zero uses of `unsigned long`, `uintptr_t`,
`sizeof(void *)` or pointer-to-integer casts, and the only intrinsics are in
`crypto/aesni.c`, which already carries a portable fallback. So a 32-bit
flavour would work. It is still the wrong call: a second flavour doubles the
build matrix, the test matrix and the coverage gate permanently, in exchange
for avoiding a long-to-real-mode thunk that is perhaps 200 lines nastier than
a protected-to-real one. Pay the 200 lines once. One core build, AES-NI kept.

*Reversed 2026-09-24 (Section 6h).* The premise was that every machine the
route would meet has long mode. The goal it now serves is XP-era hardware,
and much of that has none.

**5.2 gas `.S`, not NASM.** `.S` is preprocessed, so one header can carry the
GDT selector values, struct offsets and memory-layout constants for both the
C and the assembly side instead of duplicating them. It also rides the
existing gcc invocation, adds nothing to `make check-tools` and keeps the
repository on one assembler syntax. NASM's real-mode syntax is nicer and that
does not pay for a second toolchain dependency under the minimal-dependency
rule.

**5.3 Assembly is confined to five jobs, and everything decidable stays C.**
The five:

1. MBR stage 1. 512 bytes, real mode, entry at `0x7C00`, partition table at
   `0x1BE`, `0x55AA` at the end. gcc has no usable 16-bit backend (`-m16` is
   a `.code16gcc` prefix hack).
2. The mode switch. GDT load, `CR0.PE`, the far jump to flush, then page
   tables, `CR4.PAE`, `EFER.LME`, `CR0.PG`, another far jump.
3. The real-mode thunk, long to compatibility to protected to real and back,
   for every `INT 13h`, `INT 10h` and `INT 16h`: segment save and restore,
   IVT reload, a stack below 1 MB.
4. The `INT 13h` hook's prologue and epilogue. It is an interrupt handler:
   exact register preservation, and `retf 2` versus `iret` for the flag
   semantics.
5. The jump to a loaded kernel, with registers set per the boot protocol,
   interrupts off and the protocol's GDT loaded.

*Items 2 and 3 as built until 2026-09-24. Since Section 6h there is no
paging: the mode switch stops at the first far jump, and the thunk is
protected to real and back.*

Everything else is C in falcon-core or falcon-bios and host-tested: extent
walking, 2048-to-512 sector translation, the overlay arithmetic, El Torito
parsing, zero-page construction, GPT identification, menu logic.

**5.4 The stick layout changes.** BIOS boot from a GPT stick needs a BIOS
boot partition (type EF02) holding stage 2, plus a boot stub in the
protective MBR. That is an mkstick change and a breaking layout change:
existing sticks would have to be remade. It is a migration decision, not a
line count, and it is the reason this cannot be added purely loader-side the
way Section 8 of the roadmap can.

**5.4 is wrong about the migration (2026-09-20).** The second sentence does
not follow from the first, and the number that settles it was never looked
up. A GPT declares its first usable sector at 34 (one protective MBR, one
header, 32 blocks of a 128-entry array) and mkstick aligns every partition to
2048. The 2014 sectors in between - 1007 KiB - are unallocated on every
falcon stick that has ever been written, and stage 2 at 32 KiB and stage 3 at
512 KiB both fit inside them with room to spare. So there is no migration:
the sectors are already free, and adding the BIOS route to an existing stick
is a write to space nobody owns. Checked against the author's own stick,
which additionally still carried 444 KiB of a previous loader's core image in
exactly that gap, put there by exactly this mechanism.

What the EF02 partition is actually for, then, is bookkeeping and not
reachability: nothing reads it, because stage 1 goes to a fixed LBA and never
parses a table. It stops a later gdisk or parted seeing the gap as free. It
is therefore optional (`mkstick bios --reserve`), and declining it means an
install never writes the partition table at all, which is the only step with
any risk in it. Slot 5, so p1..p4 keep the numbers DESIGN Section 3 and the
install manifest use.

The one real code change was in `falcon-bios/src/start/biosdefs.h`: `STAGE2_LBA`
was 1, which is correct for the rig's raw image and is the GPT header on a
stick. It is 34 now, and the rig image moved with it so that what the
coverage harness boots sits at the offsets real media uses.

See `falcon-cli/src/mkstick/bioscore.h` for the arithmetic and `bios.c` for
the install.

## 6. Scope

Staged, so the first stage is the product as described in Section 3 and
everything after is additive.

| stage | new | existing touched |
| --- | --- | --- |
| S1 boot, menu, chainload | ~3,500 + 700 tests | ~1,000 |
| S2 Linux direct + hook | ~700 + 400 tests | ~50 |
| S3 Windows | ~250 + 150 tests | 0 |
| S4 personal OS unwrap | ~300 + 200 tests | 0 |
| S5 BSD memory disk | ~100 | 0 |
| **total** | **~5,300 + ~1,450 tests** | **~1,050** |

S1 breaks down as MBR stage 1 (~200 assembly), stage 2 with A20 and the mode
switch (~500), the real-mode thunk (~350 assembly), `INT 13h`/`10h`/`16h`
behind the read callback and two console primitives (~300), GPT discovery
over the boot drive (~250), the menu (~350), El Torito catalog parsing (~200,
in falcon-core, host-tested), the resident `INT 13h` virtual-CD hook
(~1,000), and orchestration (~350).

The ~1,000 touched in S1: the root Makefile gains a `bios` flavour, an
assembly rule and a second link script (~120); the menu model comes out of
`falcon-efi/src/console` into a shared home, leaving the EFI draw and read
behind (~350 of its 919); `discover` splits its inventory model from the
Block I/O enumeration (~200 of its 668); mkstick gains the BIOS boot
partition and the protective-MBR stub, with the layout constants and their
tests rippling (~250); `falcon-efi/tools/run-qemu.sh` gains a SeaBIOS mode
(~60).

Unchanged entirely: falcon-hook, falcon-winjump, `wim/`, `udf/`, `iso9660/`,
`luks/`, `ext4/`, `exfat/`, `pe/`, `manifest/`, `overlay.h`, `bsdmd`,
`extmap`, `bootcfg`, `cpio`. That is 18.3k of core plus 2.6k of hook and
winjump earning their keep.

## 6a. What the ISO reader work already built (2026-09-20)

The table above was written on 2026-09-19. The next day's work
(`iso-reader-route.md`) built three of the things it counts as net-new, for
UEFI reasons, and they are in falcon-core and corpus-proven:

| what S1 needed | state |
| --- | --- |
| El Torito boot catalog parsing (~200) | `media/eltorito.c`, 14 tests, agrees with an independent implementation on all 72 entries across 37 corpus ISOs |
| a FAT12/16/32 driver (uncosted above, and needed for p1) | `fs/fat.c`, 11 tests per width, mounts the boot image of all 37 |
| the read callback as the per-platform boundary | `media/medium.c` assembles a medium's volumes over one callback and walks a GPT; the UEFI survey already runs on it |

That last one was Section 1's central claim, and the part of it that was not
yet true. It is now: under UEFI the callback is an `fs_file` on p2, and
nothing below `medium_open` can tell what is on the other end of it.

So S1's falcon-core half is done, and about 1,000 lines of the estimate with
it. What remains in S1 is genuinely BIOS-only and mostly the part the note
was already least sure of: the MBR stage 1, the mode switch, the real-mode
thunk, `INT 13h`/`10h`/`16h` behind the callback, the menu's shared half, and
the resident virtual-CD hook that Section 7.1 calls a spike rather than an
estimate.

Section 7.2's risk is unchanged and is now the dominant one: the assembly is
outside every analyzer the project gates on, and the repository still has
none. Section 7.3, the breaking stick-layout change, is still the decision
that has to be taken by a person rather than measured.

## 6b. The third stage (2026-09-20)

Stage 2 reached 28.3 KB of a ceiling it does not get to choose. Linked at
`STAGE2_LOAD` (0x7E00), it has to be addressable in both real mode and long
mode (protected mode since 6h; the ceiling is the same), which means it has to fit below the end of the first 64 KB segment:
about 33 KB, with the menu and one filesystem driver in it and the ISO reader
and a boot path still to come. Raising `STAGE2_SECTORS` does not help. The
answer the note predicted is the one taken.

There are now three:

| stage | where | what |
| --- | --- | --- |
| 1 | 0x7C00, one sector | the MBR: bring COM1 up, read stage 2 in with `INT 13h` AH=42h |
| 2 | 0x7E00, up to 32 KB | A20, the mode switch, the thunk, and C that reads stage 3 in and jumps to it. 5.2 KB |
| 3 | 0x100000, up to 512 KB | the loader: falcon-core, the scan, the menu. 26.5 KB |

Stage 3 is self-describing: it begins with a sixteen-byte header holding a
magic and its own length, so stage 2 reads one sector, learns the length and
reads exactly the rest. The alternative is a build step that patches a sector
count into stage 2, which is a second place the size is written and a second
place it can be wrong.

**The gate.** Stage 3 lives above a megabyte and the thunk cannot follow it
there, because the thunk drops to real mode and real mode has twenty address
bits. So the thunk stays in stage 2, and stage 2 publishes a way into it at
an address both halves can write down: the head of stage 2 itself, which is
fixed already. `stage2_start` is a jump over the gate; the gate is a jump
into the thunk plus the register block the thunk reads. `biosdefs.h` names
the four offsets, `stage2.S` places them with `.org`, and the build checks
the linked addresses against the header, so a field that moved without the
header moving fails the build rather than the boot.

Two bugs in this, both of the kind Section 7.2 is about:

- The gate's `jmp thunk_bios` sits at the top of `stage2.S`, which opens in
  `.code16`. Assembled as a 16-bit relative jump and executed in long mode,
  the same opcode is a 32-bit one: the processor took two bytes of `.org`
  padding as the top half of the displacement. A reboot loop with no output
  past stage 3's first line.
- `disk.c`'s disk address packet and drive parameter block were statics. That
  was reachable while the file only linked into stage 2; in stage 3 they are
  above a megabyte, and the thunk hands the BIOS a pointer as DS:SI with DS
  zero, so the address was truncated to sixteen bits and the BIOS read
  somewhere else. Every read returned carry, and the loader reported a disk
  that would not read. They are now at a fixed address in the low 64 KB
  (`BIOS_SCRATCH`), which is the same rule as the bounce buffer and was
  always the rule; the statics only happened to satisfy it.

Neither is a mistake a C compiler makes, and neither would have been found by
anything the repository gates on. Both were found by printing characters at
the failure and reading the QEMU monitor.

What the third stage buys is that size stops being a design input. The
filesystem registry, the ISO reader, `medium.c` and a boot path all fit, and
`main.c`'s note that it calls `exfat.c` directly because the registry would
not fit is no longer true.

## 6c. A kernel boots (2026-09-20)

falcon-bios boots a stock Arch ISO's kernel and initramfs under SeaBIOS, from
a real falcon stick, over INT 13h. The whole chain, and every layer of it is
falcon-core except the first:

    INT 13h  ->  part.c: the GPT, p2 identified by DESIGN Section 3's rules
             ->  fs.c: p2 mounted by fs_identify, not named
             ->  fs_file_read_cb: the .iso as a stream of bytes
             ->  medium.c: ISO9660 and the El Torito boot image
             ->  bootfs/bootcfg: the ISO's own systemd-boot entry
             ->  linux.c: the 64-bit entry of the x86 boot protocol

and it arrives at archiso's rescue shell, having unpacked a 234 MB
initramfs, because nothing has told it where its medium is. That is the next
piece and it is already written: falcon-hook and the extent map (DESIGN
Section 6.4), which the UEFI loader appends to the same initramfs for the
same reason.

*Superseded 2026-09-24 (Section 6h): the loader now takes the 32-bit
entry, which starts kernels of both widths, from protected mode with paging
off, so there is no identity map and the ceiling is 4 GiB (`ADDR_END`). The
handoff GDT, code 0x10 and data 0x18, is unchanged. The next two paragraphs
are the long-mode loader.*

**Why the 64-bit entry.** The loader is already in long mode with paging on,
which is the state the protocol asks for; the 16-bit entry would mean going
back down through the thunk and handing over there. The cost is the four
things the protocol requires at the jump, each of which fails silently if it
is wrong: everything inside the identity map (a gigabyte here, which bounds
where the kernel and initrd may go), interrupts off, %rsi holding the zero
page, and a GDT whose code selector is 0x10 and data 0x18. falcon-bios's own
GDT has code64 at 0x08, because nothing had an opinion until now, so
`linux.c` builds a second GDT for the handoff.

**Where things go.** The zero page at 0x10000 and the command line at
0x20000, the addresses every BIOS loader has used since LILO, which is the
useful property: a kernel's decompressor is known not to land on them. The
kernel at 16 MB, above everything the loader occupies and aligned as a
relocatable kernel requires. The initrd is not fixed: it is placed from the
E820 map, as high as `initrd_addr_max` and the identity map allow, because it
is the one object whose size is not known until it is read.

**One bug worth recording**, because it reads as a wrong file rather than a
wrong offset: the first read was one sector. `setup_sects` is at 0x1F1,
inside the boot sector, but the "HdrS" magic that says the header is a header
is at 0x202, which is past it. The loader reported a stock Arch kernel as
"not a bzImage".

**A decision taken here.** The loader appends
`console=ttyS0,115200 console=tty0` to every cmdline. This build is
serial-first from its first instruction - stage 1 brings COM1 up before it
reads anything, because a machine with no usable video is the machine a BIOS
route exists to reach - and a kernel printing only to a screen that is not
there would break that where it matters most. tty0 is last so the kernel's
log goes to serial while the interactive console stays on the screen. The
UEFI loader does not do this and should not; whether the BIOS one should is
open.

## 6d. The chainload, and the resident (2026-09-20)

Section 7.1 called the resident INT 13h handler a spike rather than an
estimate and said to time-box one before believing any other number. It is
written, and it works: falcon-bios chainloads a stock Arch ISO's own
ISOLINUX from a real falcon stick under SeaBIOS. ISOLINUX draws its menu,
counts down, and loads the kernel and initramfs, and every one of those reads
is served out of the ISO file on exFAT on p2 by a handler in conventional
memory.

    falcon-bios:   serving the medium on drive 0xe0 from 0x9ec00
    falcon-bios:   boot image at CD sector 0x73, 1 run(s)
    falcon-bios:   handing over

    ISOLINUX 6.04 6.04-pre3-3-g05ac953c  Copyright (C) 1994-2015 H. Peter Anvin
    Loading /arch/boot/x86_64/vmlinuz-linux... ok
    Loading /arch/boot/x86_64/initramfs-linux.img...

**What made it tractable** was refusing to write it in assembly. Section 5.3
says the assembly is transport and every decision is C, and that is what
kept this to 250 lines of `.S`: `vcdcore.c` turns the ISO's extents on p2
into a table of runs - stretches that are contiguous on the physical disk -
and the handler does a linear scan, one multiply, one add, and re-issues the
read to the physical drive through the vector it replaced. `vcdcore.c` is
ordinary host C with eight tests over it, and it is the first thing in
`falcon-bios/` that `make test` covers.

**Where the resident lives.** A chainloaded bootloader believes it owns
memory below a megabyte: ISOLINUX relocates itself, GRUB's core image
decompresses into it, `bootmgr` uses most of it. The only region any of them
will leave alone is one the BIOS says is not there, so the word at 0x413 -
how many kilobytes of conventional memory the machine has - is lowered by
four and the handler lives in the four kilobytes that just stopped existing.
MEMDISK has done this since 2001 and nothing else works.

**The bug that shaped the design.** El Torito puts a no-emulation boot image
at 0x7C00, and stage 2 starts 512 bytes above that: the gate, the thunk, and
the code doing the copying are all inside what the image lands on. The first
attempt rebooted the machine. So stage 3 stages the image at 0x20000 and the
last thing this loader runs is a stub inside the resident, which is in memory
the BIOS no longer admits to having and so cannot be overwritten by what it
copies.

**And one that was a build bug, not a code bug.** Every address in this
component is a macro in `biosdefs.h`, and the assembly rule had no `-MMD`.
An object that kept an old `GATE_DRIVE` assembled, linked, and read the wrong
byte of memory; the symptom was a drive number that came back as the last
interrupt vector raised. The rule tracks headers now.

**The overlay, and BSD (S5).** The handler scans a short table of CD sector
ranges before every pass: sectors in one are served from conventional memory
rather than from the disk, and a read that would run into one stops at its
edge, because the handler issues one BIOS call per pass. That is what
`iso/overlay.h` is under UEFI, and it is what makes a medium bootable that is
not bootable as it lies on the stick. `media/bsdmd.c`'s two edits - the
directory record naming the ISO as a file inside itself, and the `md_image`
settings over `loader.conf`'s comment block - are a few hundred bytes and fit
it directly, so S5 is the same two edits over the same core code as the UEFI
route, laid over a different medium.

It works: FreeBSD 15.1's loader reads both edits, loads the whole 521 MB
medium off the virtual CD as a memory disk in about a minute, and the
installer comes up.

**The medium has to be padded, and that took two measurements.** A BIOS
caller reads in fixed-size chunks and the last one runs past the end of an
image whose length is not a multiple of that chunk:

    /FALCONBS.IMG;1 c0: Read 8 sector(s) from 266920 to 0x6b63c (0x7563c): 0x4
    error reading '/FALCONBS.IMG;1': input/output error

The medium is 266,927 CD sectors and the loader asked for 266,920 through
266,927. A pressed CD is padded to a whole number of blocks and answers, so
this one is too: the declared size rounds up to 64 sectors and the padding
reads as zeros.

With that in, the same loader read the sector AT the declared size, 266,944
of 266,944, and the second refusal cost the boot as the first had. So the
medium answers for one padding block past what it says it has. Being
permissive there costs nothing and a caller that has run off the end gets
zeros rather than an error it was not written to handle.

Both were found by making the handler print the sector it refused to COM1,
after two rounds of screenshotting a loader that scrolls its error away in
under a second. A handler that cannot be single-stepped can still be made to
say what it did.

**Windows (S3), and the far side of a megabyte.** Its patched `boot.wim`
appends a tail past the ISO's end measured in megabytes, which is more
conventional memory than a machine has to give. So an override can also name
a physical address instead of a segment, and the handler asks the BIOS to
move those bytes with `INT 15h` AH=87h - the one service that has always been
able to reach memory real mode cannot address. It reads six descriptors; the
handler writes two of them and the BIOS fills the rest.

The computation is `falcon-efi/src/win/winpatch.c`'s, over the same
falcon-core: the Partition Descriptors and `boot.wim`'s File Entry grow to
reach the appended blocks, the WIM's first 208 bytes are replaced, and the
tail carries the jump binary, its configuration, the new metadata and the new
lookup table. Four of those are a CD sector each and fit the handler's own
pool; the tail does not, and goes above 64 MB.

It works. Windows 11 25H2 Setup comes up, and the handler served the appended
region 217 times on the way - which is the part that matters, because Setup's
first screen looks the same whether the patch took or not. Those 217 passes
are `bootmgr` and `winload` reading metadata that exists only in the patched
WIM.

**One bug, and it is the same shape as the CR4 one below.** The heap that
holds the patch takes the highest usable run it can find, and the first
attempt took one near 3 GB on a 4 GB machine - outside the gigabyte
`lmode.S` maps. The first store triple faulted. Under UEFI the firmware maps
what it allocates; here the loader maps a gigabyte and everything it writes
has to be inside it. (Since 6h nothing is mapped: the heap's ceiling is
4 GiB, `ADDR_END`, in `heap.c`.)

## 6e. The personal OS, and one bit nobody sets (2026-09-20)

Section 4.4's route works: the passphrase opens p3 with falcon-core's LUKS2,
ext4 mounts inside it, the shelf's images list, and the one chosen is
unwrapped rather than chainloaded - `.linux` is a plain bzImage and boots by
4.2's route, `.initrd` and `.cmdline` come out of the same PE. The fixture
UKI's own init reports the credential arriving:

    FALCON-CRED-FILE cryptsetup.passphrase.cred 11 bytes
    FALCON-CRED-HEAD cryptsetup.passphrase.cred [falcon-test]
    FALCON-CRED-OK

which is the p4 unlock with no second prompt, surviving the port. sd-stub's
contribution is a cpio placing the passphrase at `/.extra/credentials/`, and
`media/cpio.c` writes it here directly.

**CR4.OSFXSR, which firmware has always set for us.** The first attempt reset
the machine the instant the passphrase opened the volume, with nothing on the
serial line but the menu drawing a second time. falcon-core's AES-NI path is
the only translation unit in this build compiled with SSE at all, and long
mode had been entered with CR4 holding PAE and nothing else: the first XMM
instruction raised #UD, and with no IDT that is a triple fault. Two bits in
`lmode.S` fix it. (Since 6h they are set in `pmode.S`, and only when CPUID
reports FXSR and SSE, because the CR4 write itself faults on a processor
without them. `idt.S` now gives protected mode an IDT, so an exception is
reported rather than triple faulting.)

It is worth naming because of what it says about the port generally. The same
falcon-core has run under UEFI for months and never needed this, because
firmware sets those bits before a loader ever runs. A BIOS build inherits
nothing; everything the environment used to provide has to be provided.

**And one that was the same shape.** An ext4 directory entry carries no size -
`fs.c` says so where it builds one, and the consumer is meant to open the file
and ask - so taking the size from the entry read zero bytes of every image and
reported that none of them was a UKI.

## 6f. First real hardware (2026-09-23)

An ASUS VivoBook 14 (8th-gen Intel, CSM legacy boot) booted a stick written
with `sgdisk -Z -n 1:2048:0 -t 1:0700` and `mkstick bios`, and falcon-bios
reached its menu. That is stages 1 to 3 outside SeaBIOS: the firmware booted
a protective MBR whose `0xEE` entry has no active flag, stage 1 and stage 2
read through AH=42h, long mode came up, the thunk served stage 3's reads, and
the console drew. The stick carried no falcon partitions and no ISOs, so none
of Section 4's routes ran.

The same day, Section 4.4 ran on hardware: an ASUS TP410UAR (VivoBook Flip
14, i5-8250U) booted the full falcon stick through CSM, with the internal
SATA disk as the other drive. The passphrase opened p3, the UKI was
unwrapped and its kernel entered by the 64-bit entry, and the booted system
has no `/sys/firmware/efi`. The initrd logged `Received untrusted
credentials: cryptsetup.passphrase` and p4 unlocked from it with no second
prompt. The other four routes are still unmeasured on hardware.

The machine also answered part of a question the study had not asked: a CSM
of this era does not require the active flag. Older BIOSes are the open case
(the Win2k/XP-era hardware that would need a 32-bit stage 3 as well, which
is Section 6h).

## 6g. One screen (2026-09-24)

falcon-bios drew its own screens: a hand-written menu with "  > " markers,
a sentence for a key hint, every route's progress on screen as it happened,
failures worded wherever they were found. It worked, and it did not look or
read like the UEFI loader, because the UEFI loader's conventions lived only
in its code. One of the differences was a defect: `linux.c` reported a
kernel it would not boot on serial only, so a machine with no serial cable
showed "starting the kernel" and then the menu, with no reason between.

Now both loaders draw from one layout, `falcon-core/src/tui` (the UEFI
console's frame, block and row composition, moved rather than copied), and
falcon-bios draws through `src/console/screen.c`, which is `console/menu.c`'s calls
one for one. Cells go into VGA text memory directly: teletype output cannot
colour a cell, and AH=09h can but costs a thunk round trip per character.
Serial gets the ANSI sequences OVMF's terminal sends for the same calls,
which is what let tools/coverage/run-case.sh drop both of its BIOS special
cases. The personal-OS chooser's rows moved to core too
(`media/shelf.c`), since falcon-bios had been showing bare file names.

Measured in the rig: the UEFI loader's menu, prompt and chooser are
byte-identical in screendumps before and after the move, and under SeaBIOS
the personal OS (FALCON-CRED-OK), a direct Arch boot and Windows 11 Setup
all pass behind the new screens.

## 6h. i686, and promotion at run time (2026-09-24)

The goal: a falcon stick that boots ISOs on Windows 2000 and XP era machines,
the ones that can boot from USB at all. A BIOS too old to offer USB boot is
out of scope; one that can is in. Much of that hardware is a Pentium III, a
Pentium M or an Athlon XP, none of which has long mode, and 5.1's loader
stopped at its CPUID check on all of them. The plan is two sticks, the
256 GB one for x86-64 under UEFI and BIOS and a 128 GB one for 32-bit ISOs
and a 32-bit personal OS, and one loader on both.

**One loader, not two.** The loader is i686 code in flat 32-bit protected
mode with paging off, and nothing else. What a newer processor adds is taken
by asking CPUID at run time, in three places:

- `pmode.S` turns on the SSE state (CR4.OSFXSR, OSXMMEXCPT) only when CPUID
  reports FXSR and SSE, because the CR4 write itself faults where they are
  missing;
- falcon-core's AES path already chose AES-NI by CPUID, so a Pentium III
  unlocks p3 on the portable path and a modern machine on AES-NI, from the
  same image;
- `linux.c` asks for long mode only when the kernel is 64-bit, and refuses a
  64-bit kernel on a processor without it with a sentence, where it would
  otherwise die in its own decompressor with "starting" still on screen.

Long mode itself buys a BIOS loader nothing. Every object the loader places
goes below 4 GiB, which is where the boot protocol, the BIOS's AH=87h and
the initrd ceilings want them anyway, and the boot protocol's 32-bit entry
starts a 64-bit kernel exactly as it starts a 32-bit one (the kernel enters
long mode itself). So the change made the loader smaller as well as wider:
the page tables, the identity map and the long-to-real half of the thunk
are gone, and the thunk that remains is the protected-to-real one 5.1 said
was 200 lines easier.

What 5.1 feared, a second falcon-core flavour doubling the build and test
matrix, did not arrive in that form. falcon-core compiles clean as
freestanding i686 under the full warning set after three changes, all of
them real: a WIM offset compared before it is narrowed (it would otherwise
wrap into the metadata on 32 bits), a lookup length proven to fit `size_t`,
and `aesni.c` no longer pulling in the hosted `<stdlib.h>` through the SSE
headers. The i686 BIOS build links all of core, so it is the check that
keeps core clean at that width. Running core's host tests as i686 as well
needs 32-bit glibc and cmocka on the build host, and is left for the
harness container.

**The edges that are width-specific**, and nothing else is:

- `falcon-hook/src/sys.c`: `int $0x80` system calls, O_LARGEFILE, pread64's
  split offset and an i386 `_start`, beside the x86-64 ones. The hook is
  built twice (`falcon-hook`, `falcon-hook32`) from otherwise the same
  source, and falcon-bios embeds both and appends the one the kernel's
  setup header says it needs (`linux_probe`, xloadflags bit 0).
- falcon-bios itself, which has only one width now.

The UEFI loader is untouched: it is x86-64 UEFI only, and 32-bit UEFI is
out of scope.

**Measured in the rig** (SeaBIOS; `bios32` is the new harness column, QEMU's
`pentium3` model: no long mode, no SSE2, no AES-NI):

| ISO | bios32 (Pentium III) | bios (host CPU) |
|---|---|---|
| Alpine 3.24.2 x86, direct | PASS | PASS |
| Alpine 3.24.2 x86, chain | FAIL, as 64-bit Alpine does in `bios` | FAIL |
| Arch Linux 32 2024.07.10, direct | PASS | PASS |
| Debian 12.15 i386 netinst, direct | PASS | PASS |
| Arch x86_64 2026.01.01, direct | refused: "the kernel is 64-bit and this processor has no 64-bit mode" | PASS |
| Windows 11 25H2 | | PASS |
| the personal OS fixture (a 64-bit UKI) | refused, as above, after the unlock and chooser | PASS, FALCON-CRED-OK |

The three 32-bit ISOs are corpus rows (corpus.tsv's `arch` column, i686),
and docs/coverage.md carries the bios32 column beside bios (run on
binhost-builder, 2026-09-24). Chain fails in both BIOS columns for most of
the corpus, 64-bit and 32-bit alike, so it is not a width problem.

**The 32-bit stick.** docs/notes/stick32.md is the second stick, built and
booted: Arch Linux 32 as its personal OS and 32-bit ISOs on p2, read-only in
QEMU on a Pentium III with 512 MB, to a login prompt each way. Building it
found the one thing the loader's side could not: `mkstick create`'s stock
argon2id on p4 is tuned on the building host and cannot run on an old
machine, hence `mkstick create --p4-kdf-memory`.

**On hardware** (2026-09-24): a Windows 7 era machine in BIOS mode booted
the 32-bit stick to its personal OS (stick32.md, which also records the
retry-prompt wrap it found). Still unmeasured: a processor without a
64-bit mode, and the 32-bit ISOs.

**Building it.** falcon-bios and `falcon-hook32` link against the host
compiler's 32-bit libgcc (64-bit division and shifts in i686 code are calls
into it). Without multilib, `gcc -m32 -print-libgcc-file-name` answers with
the 64-bit libgcc and the link fails on `__udivdi3`; `make check-tools`
warns, and the coverage container installs `gcc-multilib` for it.

## 6i. The next-boot marker (2026-09-24)

archusb's reboot chooser (`archusb-boot-next`) queues "boot this next" for
the UEFI loader in the `FalconNextBoot` variable, and brings the reboot back
to the stick with `BootNext`. Booted through falcon-bios it had neither:
legacy BIOS has no variable store, the BIOS Boot Specification gives an
operating system no way to set the boot order, and a CSM machine's UEFI
`BootNext` is out of reach of a system booted in legacy mode. Vendor tools
and IPMI can set a one-time boot on some machines; none of that generalises.

So the marker lives on the stick: LBA 2047, the last sector of the gap in
front of p1, as a short text record Linux writes with `printf` and `dd`
(DESIGN Section 6.3.1 has the format and the reasoning; `media/nextboot.c`
the parse, `falcon-bios/src/disk/marker.c` the reading). `lilo -R` and GRUB's
`grubenv` are the same idea: a one-shot kept in raw blocks outside any
filesystem, because the loader has no filesystem writer.

**Zeroed, not left to expire.** Two ways to make it one-shot were weighed.
The loader clearing it costs the BIOS loader its first disk write. Leaving
it to a time window alone costs the one-shot: a target that fails, or a
reset inside the window, boots the same thing again. Clearing won, so the
BIOS loader now writes one sector: only LBA 2047, only when it carries the
magic, only ever to zero, and checked by reading it back. `marker.c` is the
only file that can, and only the stick's stage 3 links it.

**Dated as well.** What cannot be replaced is `BootNext`'s half: the reboot
follows the machine's boot order, and a stick picked from a boot menu is not
in it. The marker would then wait for the stick's next boot and start
something unasked, so it is honoured only within ten minutes of being set.
Both sides read the same RTC the same way (its digits taken as UTC), which is
what keeps a machine that holds local time in its RTC from skewing it.

**Moved, not copied.** The value's rules (decode, the path match, the `:`
reserved namespace) were the UEFI loader's, in
`falcon-efi/src/bless/blesscore.c`. They are falcon-core's now, so the two
loaders cannot disagree about what a marker names.

**Measured in the rig** (SeaBIOS; the marker written by `printf | dd` onto
the stages image, the rig stick as the second disk): a fresh marker naming
`payload-test.iso` boots it by `DIR` without the menu, and so does
`PAYLOAD-TEST.ISO`; `:personal` opens at the passphrase prompt; a marker an
hour old, one with a tab in its value, one naming a missing ISO and one with
an unknown reserved value each end at a note and the menu. The sector reads
zero after every one of them, and a sector of 0xA5 bytes, which is not a
marker, is left byte for byte as it was.

End to end with archusb's writer: `archusb-boot-next set arch` (its BIOS
branch, pointed at an image of the rig stick that `mkstick bios` had given
stages) wrote the marker, SeaBIOS booted that image as its only disk, and
falcon-bios cleared the marker and opened at the passphrase prompt. That run
is also why the window allows a clock that went back: the laptop's RTC ran
four seconds ahead of the system time QEMU's RTC follows, so the marker was
stamped four seconds "after" the boot read it, and the first build refused
it. Anything that corrects the RTC between the two readings does the same,
so up to two minutes backwards counts as fresh.

## 6j. 32-bit Windows, and a wider 32-bit corpus (2026-09-25)

The Windows route puts falcon-winjump in place of `winpeshl.exe`, and winjump
was built for x64 only, so a 32-bit WinPE could never start it. It is built
twice now, from one source (`winjump32.exe`, `i686-w64-mingw32-gcc`, with
exactly the x64 build's imports), and falcon-bios reads the machine type out
of the boot image's own `winpeshl.exe` headers to choose (DESIGN 12.3.9). In
the harness, Windows 10 22H2 x86 passes the BIOS drop: Setup read 330 MiB of
its install image through the ISO the i686 winjump attached.

On the Pentium III it cannot: Windows 8 and later need PAE, NX and SSE2, and
bootmgr reset the machine silently. falcon-bios now asks CPUID first and
names what is missing ("this processor has no SSE2"). Windows 7 is a separate
wall and is not addressed: its WinPE cannot attach an ISO at all
(`AttachVirtualDisk` gained ISO support in Windows 8), 32-bit or 64-bit.

Thirteen 32-bit rows went into the corpus with it. What the harness said,
read against the screenshots, because several of these systems write nothing
to serial and take no DHCP lease:

| media | BIOS | what the screen shows |
|---|---|---|
| Windows 10 22H2 x86 | drop PASS | Setup, reading install.wim through the attached ISO |
| Clonezilla, GParted, antiX 26 (i686) | drop PASS in both columns | live systems up |
| Tiny Core 17.1 | PASS in every BIOS cell | |
| Slackware 15.0 | PASS on the host CPU | on the Pentium III its `huge.s` kernel prints nothing: probably its own processor floor |
| OpenBSD 7.9 | chain FAIL | the installer is up; it uses the VGA console only, so the judge cannot see it |
| FreeDOS 1.4 | chain FAIL | DOS boots off the virtual CD, then its CD driver (UDVD2) drives the ATAPI hardware directly, cannot see an INT 13h CD, and asks Abort, Retry, Fail |
| ReactOS 0.4.16 | chain FAIL | FreeLoader and the kernel load, then `IO1_INITIALIZATION_FAILED`: the protected-mode kernel loses the INT 13h CD, the wall chained Linux meets |
| FreeBSD 14.5 i386 | drop FAIL | the memory-disk route: BTX halts while the i386 loader pulls a 1.1 GB image into its 32-bit space |
| NetBSD 10.2 | chain FAIL | its own boot code, chained off the virtual CD, resets the machine right after the handoff (the harness's Enter nudges then reach the menu and report a DIR failure) |
| Haiku R1b6, memtest86+ 8.10 | chain refused | their El Torito boot images ask for floppy or hard disk emulation, which the resident does not do |

The drop FAILs for FreeDOS, ReactOS, the BSDs, Haiku and memtest86+ are
"direct boot failed" within seconds: Enter is the direct route, these are not
Linux, and on BIOS a direct boot with no entry it can take stops instead of
falling through to the chainload (12.3.5 of DESIGN; the UEFI loader does
fall through). Decided 2026-09-25 that it should, and it does now: a direct
boot that stops before committing goes on to the medium's own loader, so
Enter on those rows now means what `c` did.

What the table asks for, in the order it would pay: floppy and hard disk
emulation in the resident (Haiku, memtest86+, and every DOS-era image); a
look at why NetBSD's boot code resets; a judge that reads the screen for
systems with no serial console. FreeDOS and ReactOS need a driver of their
own inside the booted system, which is the Ventoy question again.

## 6k. Floppy-emulation El Torito (2026-09-25)

Some media boot a BIOS through an El Torito image that emulates a floppy
rather than a no-emulation CD: the catalog's media type is 1, 2 or 3 (1.2M,
1.44M, 2.88M), and the BIOS is meant to present the image as drive 0x00 and
boot its first sector. falcon-bios refused these at the boot catalog. Two
corpus rows need it: memtest86+ 8.10 (a 1.44M floppy) and Haiku R1b6 (2.88M).

It is served as a memory disk, not off the disk like the CD: the whole image
is small (2.88 MB at most), so `vfd.c` loads it into extended memory once and
the resident of `vfdres.S` serves reads with INT 15h AH=87h, the block move
the Windows tail already uses. The resident hooks INT 13h only and answers
for drive 0x00: AH=02h reads (CHS to a linear sector by `vfdcore.c`'s
arithmetic, one AH=87h move, since a flat memory disk has no real tracks to
cross), AH=08h the geometry, the reset and status calls, and it refuses
writes. The image sits in RAM the map does not hide, which is sound for media
that read the floppy once as they load and never again. The geometry and the
CHS-to-LBA arithmetic are `vfdcore.c`, host-tested; the assembly does a
multiply and a re-issued move.

Enter on such a medium takes the DIR route, which is not Linux and falls
through to CHN (6j), and `vcd_chainload` dispatches a floppy media type to
`vfd_serve` rather than to the CD.

Measured in the rig (SeaBIOS): **memtest86+ boots and runs** off the emulated
1.44M floppy - it loaded itself through the handler and reached its memory
test, Pass 23%, no errors. The harness scores it FAIL because memtest writes
only to the screen (no serial, no DHCP), the same blind spot OpenBSD's
installer has; the screenshot is the evidence.

**Haiku does not, and it is a real gap.** Its 2.88M floppy boots
`haiku_loader`, which then loads the rest of Haiku - hundreds of megabytes -
from the medium's CD portion. falcon-bios serves the floppy but not a CD at
the same time, so the loader finds nothing and resets, and the boot loops.
Booting Haiku's anyboot needs floppy and CD emulation co-resident, which this
does not do. Hard-disk emulation (media type 4) is also not here; no corpus
medium needs it.

## 7. Risks, in the order they matter

**7.1 The `INT 13h` resident is a spike, not an estimate.** The ~1,000 above
is MEMDISK-shaped guesswork. The program has to serve 2048-byte CD sectors
off 512-byte LBAs through the p2 extent map, apply the overlay's override and
append regions from extended memory while running in real mode with 640 KB of
conventional memory, re-enter the saved original vector for reads that fall
through to the physical disk, and stay correct across `bootmgr`'s mode
switches. The two answers to the memory problem are both old: unreal mode
inside the hook (4 GB-limit descriptors on entry, segment state restored on
exit) or `INT 15h` AH=87h block moves. MEMDISK and Ventoy both prove it is
doable; neither makes it predictable. Time-box a spike on this alone before
believing any other number in Section 6.

**7.2 The assembly is outside every analyzer the project relies on.** The
repository has zero assembly today (checked 2026-09-19). The quality story is
`-Wconversion`, `-Wsign-conversion`, `-fanalyzer`, cppcheck, clang-tidy and
an 80% coverage gate, and none of them look at a `.S` file. S1 adds roughly
700 to 800 lines of the hardest code in the project to debug, in the one
place none of those tools reach. That is a larger change to the project's
character than the line count suggests, and it is why 5.3 draws the C/assembly
line where it does: the assembly should be transport and nothing else.

**7.3 The stick layout change is a migration.** Withdrawn 2026-09-20: there
is no migration. See 5.4, which was wrong, and `mkstick bios`, which installs
onto a stick that already exists without remaking anything on it.

What remains of this risk is smaller and is a reachability question rather
than a layout one. Installing stage 1 makes a stick bootable on machines that
ignore it today, and a BIOS machine has no Secure Boot, no shim and no db, so
the BIOS route has no blessed path and cannot have one. That is why the
install is opt-in and is not part of `mkstick create`: somebody asks for it.

**7.4 A second platform is a second coverage matrix.** `docs/coverage.md`
scores each corpus ISO in three Secure Boot states. BIOS adds a fourth cell
per ISO that shares none of the three. The harness work is small; the
recurring cost of keeping the cell green is not. (Since 6h there is a
fifth, `bios32`, the same machine as a Pentium III, where the 32-bit ISOs
are judged and the 64-bit ones are N/A.)

## 8. Open questions

1. **Is it wanted at all?** BIOS-only machines are pre-2012, and CSM is gone
   from new firmware. The study says the cost is ~5,300 lines rather than a
   second product; it does not say the reach is worth it. Nobody has asked
   for it yet. Answered 2026-09-20 by building it, and 2026-09-24 by the
   request that widened it: XP-era machines that boot from USB (6h).
2. **Where the shared menu model lives.** Answered 2026-09-20, and the
   question aimed slightly wrong: `falcon-efi/src/console` is presentation
   and belongs where it is. What had to move was the survey - the directory
   walk, the extension rule, the sort and the folder view - which sat in
   `falcon-efi/src/efi/main.c` and so was unreachable from here. It is
   `falcon-core/src/media/survey.c` now, beside `medium.c`, and both loaders
   build their menus from it. Found the hard way: falcon-bios had its own
   flat root-only scan, which showed an empty menu on a stick whose ISOs sit
   in `Linux/` and `Windows/`.
3. **Whether S1 alone is a coherent release.** Chainload-only means Tier B
   installers and anything needing the hook do not find their media, which is
   the wall `coverage-roadmap.md` W1 exists for. S1 plus S2 may be the real
   minimum. Moot: all five stages were built before any was released.
4. **What the menu says about verification.** Answered 2026-09-24 (Section
   6g): the banner's state segment, where the UEFI menu says "SB: off" or
   what the machine trusts, says "BIOS", and the rows carry no tag columns.
   Once, in the place the UEFI loader says it.
