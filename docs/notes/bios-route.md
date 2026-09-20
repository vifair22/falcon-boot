# BIOS mode: the scope, measured against the tree (2026-09-19)

Not decided. This began as the scoping study behind "what would a
BIOS-capable falcon-boot actually cost", written down because the answer that
had been assumed is wrong and the right answer is cheap enough to be asked
again.

Since then a first slice exists (2026-09-20): `falcon-bios/` with stage 1 and
stage 2, and `run-qemu.sh --bios`. It boots under SeaBIOS and prints the
drive it was handed. What that proves is the toolchain and nothing else,
which was the point of building it first: gas `.S` against a preprocessed
header, a link script placing each stage where it runs, a build flavour, and
a rig. Section 6a records what the ISO reader work had already built.

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

Everything else is C in falcon-core or falcon-bios and host-tested: extent
walking, 2048-to-512 sector translation, the overlay arithmetic, El Torito
parsing, zero-page construction, GPT identification, menu logic.

**5.4 The stick layout changes.** BIOS boot from a GPT stick needs a BIOS
boot partition (type EF02) holding stage 2, plus a boot stub in the
protective MBR. That is an mkstick change and a breaking layout change:
existing sticks would have to be remade. It is a migration decision, not a
line count, and it is the reason this cannot be added purely loader-side the
way Section 8 of the roadmap can.

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

**7.3 The stick layout change is a migration.** See 5.4.

**7.4 A second platform is a second coverage matrix.** `docs/coverage.md`
scores each corpus ISO in three Secure Boot states. BIOS adds a fourth cell
per ISO that shares none of the three. The harness work is small; the
recurring cost of keeping the cell green is not.

## 8. Open questions

1. **Is it wanted at all?** BIOS-only machines are pre-2012, and CSM is gone
   from new firmware. The study says the cost is ~5,300 lines rather than a
   second product; it does not say the reach is worth it. Nobody has asked
   for it yet.
2. **Where the shared menu model lives** once it comes out of
   `falcon-efi/src/console`. `falcon-core/src/ui/` is the obvious answer and
   would be the first core module that is not a parser.
3. **Whether S1 alone is a coherent release.** Chainload-only means Tier B
   installers and anything needing the hook do not find their media, which is
   the wall `coverage-roadmap.md` W1 exists for. S1 plus S2 may be the real
   minimum.
4. **What the menu says about verification.** A BIOS build has no trust
   column and no blessed state. It should say so once, plainly, rather than
   showing a UEFI menu with the security half missing.
