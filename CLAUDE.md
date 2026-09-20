# falcon-boot

A single-purpose, auditable multi-boot loader for UEFI x64. A distilled Ventoy:
drag-and-drop ISO booting plus a personal LUKS-backed OS booted via a UKI.

## Read this first

**`docs/DESIGN.md` is the authoritative design and the source of truth.** Read it
before proposing or changing any architecture. It argues its decisions and
records the alternatives that were considered and rejected, so do not reintroduce
a rejected approach without re-reading why it was rejected. `README.md` is the
short public summary of the same model.

Coverage (docs/notes/coverage-roadmap.md, 2026-09-20): M0-M7 are done. The
corpus harness and scorecard exist (`docs/coverage.md`), and both run in a
container on binhost-builder rather than on a workstation. falcon-core reads
ISO9660/Rock Ridge/Joliet, UDF, FAT12/16/32 and an ISO's own GRUB,
systemd-boot and isolinux configuration, and assembles a medium's volumes
itself (`media/medium.c`), which is what the menu's survey now uses instead
of publishing a block device and asking the firmware
(docs/notes/iso-reader-route.md); falcon-hook rebuilds the ISO as a device-mapper
device inside the distro's initramfs; and with Secure Boot off the loader
boots an unblessed Linux ISO's kernel directly (Enter; `c` chainloads its own
loader, `d` forces direct); with it on, through the ISO's own shim when the
kernel carries `.sbat` (Fedora, Rocky, Alma; D3a), else through the ISO's own
signed GRUB with a grub.cfg of ours served in its place and falcon-hook as a
blocklist initrd past the ISO's end (D3b; `docs/notes/d3-spike.md`). Blessed wraps
carry the hook too. `b` on a
row or `a` for everything new blesses from the menu through the appliance.
The Tier B installers (Debian netinst, Alpine, Proxmox) boot direct through
the hook's fallbacks. A stock Windows ISO boots in every state whose db
trusts Microsoft: the loader patches its boot.wim in memory as the virtual CD
serves it (falcon-winjump in place of winpeshl.exe), and Setup finds its
source through the ISO winjump attaches from Windows. BSD media (M7) boots by a third
route: the served ISO gains one directory record naming itself as a file and
the md_image settings in its loader.conf, so FreeBSD's own loader mounts root
from a memory disk. Blessing it signs that loader with the MOK and starts the
signed copy with the medium named to the platform as its device: FreeBSD
passes in every Secure Boot state, pfSense only with it off, since shim will
not parse pfSense's loader at all (docs/notes/bsd-route.md).

Status: pre-1.0 MVP. Build-order steps 1-9 are done and step 10 (real
hardware) is partly done; `docs/DESIGN.md` Section 11's status header is the
record. The loader, the personal-OS path with its passphrase handoff (the
UKI published on `credfs` with `<uki>.efi.extra.d/cryptsetup.passphrase.cred`
beside it, which sd-stub packs into the initrd), mkstick, the sign tool and
the signing appliance all exist and are proven in the QEMU/OVMF rig under
both Secure Boot states (`make test-stick`, `make run-stick` /
`run-stick-secboot`, `falcon-appliance/tools/`). Measured on real hardware
(2026-09-13): shim/MOK enrollment, the p3 unlock, and a blessed Arch ISO
booting under Secure Boot on a machine that trusts nothing from Microsoft.
Still unmeasured on hardware: the personal OS end to end, Windows, and
D3a/D3b.

## Non-negotiable security invariants (DESIGN.md Section 7)

These are hard rules. If a change would break one, it is a regression, stop and
flag it rather than implement it.

1. **The loader verifies; it never signs.** The loader never performs a signing
   operation and never holds a private key. Signing is a private-key operation
   that happens only in Linux userland (an external host, or the on-stick signing
   appliance). See Section 7.5.
2. **Enforcement is the platform's, never the loader's.** The loader never
   changes what is allowed to run. It launches everything through a
   shim-verified load path (see Section 7.6 for which entry point that is),
   which enforces when Secure Boot is on and permits when it is off. The
   loader reads Secure Boot state only for UX.
3. **No mechanism that relaxes signature enforcement exists in the loader.**
   There is no security-protocol override and no in-loader "disable enforcement"
   toggle. The user opts out of enforcement only through platform,
   physical-presence mechanisms (firmware Secure Boot off, or shim `MokSBState`
   via `mokutil --disable-validation`). See Sections 7.3 and 7.4.
4. **The private MOK key stays encrypted on p3 and never on the readable exFAT
   partition and never in the loader.** The public cert travels for enrollment;
   the private key does not travel in the clear. See Section 7.5.

Consequence, stated so nobody "fixes" it: drag-and-drop of an arbitrary *unsigned*
ISO with reliable media-finding while Secure Boot validation stays on is
intentionally impossible. The paths for that case are blessing (sign the kernel
with the MOK key) or the platform opt-out, not any loader-side relaxation. This
is by design; it is not a gap to close.

## Build and layout

See `docs/DESIGN.md` for full architecture. Quick orientation:

The repository is six components, each with its own `src/`, `tests/` and
`tools/`, built by one non-recursive Makefile: the root `Makefile` holds the
shared toolchains, flags, test runner and compile database, and `include`s a
`build.mk` from each component. There is one make invocation and one
dependency graph; nothing recurses. Module directory names are unique across
the tree, so `#include <fs/exfat.h>` means one file wherever it is written.

- `falcon-core/` the EFI-free libraries every other component links. Nothing
  here knows about firmware: device access goes through a read callback and
  every buffer is the caller's, which is what lets the loader, the host tools
  and the appliance run one implementation and the host tests exercise it.
  - `crypto/` SHA-1 (WIM stream names only), SHA-256/512, HMAC, PBKDF2,
    AES-XTS (AES-NI with a portable fallback); the only crypto in the loader.
  - `fs/` the filesystem driver registry (`fs.c`, DESIGN Section 9: one
    `fs_ops` vtable, opaque caller-allocated storage, unified dirents and
    status codes) and the read-only drivers only it calls, `exfat.c`,
    `ext4.c`, `fat.c` (FAT12/16/32 with long names; one driver, since the
    width is derived from the cluster count and only the FAT entry read
    differs), `iso9660.c` (Rock Ridge, Joliet) and `udf.c`. Consumers select
    a driver through the registry and never name one; `fs_lookup` resolves a
    path. `udf.c` also carries the few record edits the Windows patch needs
    (a File Entry's extents, a Partition Descriptor's length, an anchor),
    applied to caller buffers, never to media.
  - `luks/` LUKS2 header parse, keyslot unlock, AF merge.
  - `part/` GPT parsing and falcon partition identification.
  - `pe/` PE/COFF header layout read from a file's bytes; the one PE walker.
  - `trust/` the boot predictions (DESIGN Section 7.8): `der`, `x509` and
    `authenticode` read an image's signature block and Authenticode hash,
    `siglist` the db/dbx/MokList stores and SBAT levels, `trust` the verdict,
    `probe` the glue over a read callback. A prediction is a label and no boot
    path takes one.
  - `media/` what an image is: family detection (`family.c`), the initrd cpio
    writer (`cpio.c`), `.osrel`/`.uname` reading (`osrel.c`), an ISO's own
    boot configuration (`bootcfg.c`, read through `bootfs.c` over a mounted
    image), the extent map handed to falcon-hook (`extmap.c`) and the hooked
    boot's cmdline (`hookcmd.c`), the grub.cfg D3b serves in place of
    an ISO's own (`grubcfg.c`) and the two edits that make a BSD medium boot
    itself as a memory disk (`bsdmd.c`). `eltorito.c` reads a medium's boot
    catalog and `medium.c` assembles what a medium holds into a set of
    mounted volumes (UDF, then ISO9660, then the El Torito boot image) over
    one read callback, which is the job the firmware's ConnectController does
    today and the per-platform boundary a BIOS build would need
    (docs/notes/iso-reader-route.md).
  - `wim/` LZX decompression and the WIM reader, plus the boot.wim patch
    (`wim_patch_build`): a new header and a tail appended after the
    original's end, the original bytes untouched.
  - `manifest/` the blessed-artifact manifest parser and writer (DESIGN
    Section 8.1), shared by the loader and the sign tool.
  - `lib/` freestanding `mem*` and UCS-2 helpers; `string.c` is linked into
    the loader only, since every hosted build has a libc.
  - `tests/` the core test suite, plus `tests/support/` (`tests.h`, `synth`,
    and `hostshim/`, which answers `<lib/string.h>` with libc) that every
    component's tests link. `tools/` builds the fixtures: exFAT, ext4, LUKS2,
    trust.
- `falcon-efi/` the loader: freestanding C11, MS x64 ABI, built to PE32+ with
  `x86_64-w64-mingw32-gcc`. No gnu-efi, no EDK2.
  - `efi/` the entry point (`main.c`), the `.sbat` section, our own minimal
    UEFI headers.
  - `loader/` the shim-verified load path (`loadops.c`, DESIGN Section 7.6)
    and `credfs.c`, the synthetic read-only filesystem a UKI is published on
    so it can be loaded by device path and sd-stub finds the passphrase
    credential beside it (DESIGN Section 5.1). `credfs_efi.h` carries the
    File Protocol's full provider-side surface, which `efi/sfs.h` deliberately
    stubs out on the consumer side.
  - `console/` the text menu over Simple Text Output/Input, plus the masked
    passphrase prompt (`prompt.c`, its own TU so "what has held the secret"
    has a one-file answer).
  - `discover/` the single device-discovery pass (`discover.c`) and GPT
    identification's firmware half (`partefi.c`): enumerates the Block I/O
    handles once, classifies each, and hands every consumer one inventory.
  - `secboot/` Secure Boot state for UX (`sb.c`) and the variable reads the
    boot predictions judge against (`trustefi.c`).
  - `bsd/` BSD media served as its own memory disk (Section 6.4; M7):
    the two byte edits core computes, laid over the virtual CD, and the
    memory check FreeBSD's staging area needs.
  - `iso/`, `pos/`, `bless/` the three boot paths: virtual media over a p2
    file (DESIGN Section 6), the personal OS (Section 5; the only module that
    ever holds the passphrase, glue over core's `luks` and `fs`), and blessed
    media (Section 8.1; `blesscore.c` is its EFI-free, tested half).
  - `direct/` direct kernel boot of an ISO (DESIGN Section 6.4): kernel and
    initrds read with core's ISO drivers, falcon-hook appended, the extent
    map set, the initrd served over LoadFile2, the kernel loaded through
    `loadops`.
  - `win/` Windows install media patched at boot (`winpatch.c`, DESIGN
    Section 6.4): boot.wim read from the ISO's UDF tree, patched with core's
    `wim`, and served through the virtual CD's override layer
    (`iso/overlay.h`, which takes memory sources as well as p2 files).
  - `tools/` the QEMU + OVMF rig (`run-qemu.sh`, with `--secboot`, `--stick`
    and `--headless`; `ovmf.sh`; `rig-shot.sh`), its fixtures (the payload
    ISO and images, the throwaway test MOK, the sd-stub fixture UKI with its
    `/init` in `uki-init/`, the four-partition test stick) and the two spikes
    whose findings are in `docs/notes/`.
- `falcon-cli/` host tooling, Linux userland C never linked into the loader:
  `src/mkstick`, `src/sign`, and their shared `src/common`. May shell out to
  `cryptsetup`, `sbsign`, `mkfs.*`; it builds UKIs itself (`sign/uki.c`,
  byte-identical to ukify). The sign tool is also linked
  static for the appliance.
- `falcon-appliance/` the on-stick signing appliance. `src/` is PID 1, split
  by job (`appliance.h` has the map: `main`, `console`, `stick`, `run`,
  `survey`, `list`, `select`, `shelf` (p3 read in process with core's LUKS2
  and ext4, the keypair handed on as sealed memfds), `bless`, and `route`,
  the host-tested decision of what each file on p2 is owed). It blesses and nothing else: Windows
  media is reported and left alone, since the loader patches it at boot. The
  Makefile links it static as
  `build/bin/falcon-boot-appliance-init`. `image/` is the recipe for the UKI
  it runs in: `mk-appliance.sh` plus the image's contents as data,
  `programs.list` (`sbsign`, `sbverify`) and `modules.list`, each entry
  with its reason. The build fails if any program the image can exec lacks its interpreter inside
  it.
  `tools/` holds the QEMU proofs.
- `falcon-winjump/` the WinPE jump binary (DESIGN Section 6.3), a Win64
  console program. The loader embeds its stripped build as a generated C
  array. It links static (`-static`) because WinPE has no `libssp-0.dll`, and
  a missing import kills it before its first log line.
- `falcon-hook/` the program the loader appends to an ISO's initrd (DESIGN
  Section 6.4): freestanding static x86-64 Linux, no libc (`src/sys.c` is its
  system calls and startup). It runs as `rdinit=`, reads the extent map from
  the initrd (a direct boot, or the credential sd-stub packs beside a
  blessed wrap) or efivarfs (an older wrap), maps the ISO
  read-only with dm-linear as `/dev/mapper/falcon-iso` under the ISO's label
  (a loop device when the kernel has no dm and the ISO is contiguous;
  `src/dimod.c` brings loop in from a Debian installer ISO's own pool), makes
  the per-distro nudges and pre-mounts in `src/hookcore.c`'s tables, and
  execs the distro's init. `src/hookcore.c` is its host-tested half; the
  loader embeds the stripped binary as a generated C array.
  `tools/run-hook-rig.sh` boots it under QEMU without the loader.
- `falcon-bios/` the BIOS loader (docs/notes/bios-route.md). The image is
  opt-in and not built by `all`, because the route is still undecided: BIOS
  boot from a GPT stick needs a BIOS boot partition, which is a breaking
  stick-layout change nobody has taken (bios-route.md Section 7.3). Its
  host-testable C is in `make test` and in `compile_commands.json` like
  everything else. `make bios` builds it and `make test-bios-img` makes a
  disk the rig boots with `falcon-efi/tools/run-qemu.sh --bios`.

  Three stages, because stage 2 is linked where real mode and long mode agree
  on what an address means and so cannot exceed about 33 KB. `src/boot.S` is
  the 512-byte MBR; `src/stage2.S`, `src/lmode.S` and `src/thunk.S` reach
  long mode and keep what has to stay reachable from real mode (the thunk,
  and the gate at the head of stage 2 that publishes it at a fixed address);
  `src/load3.c` reads stage 3 in and jumps. Stage 3 links at a megabyte and
  holds the loader proper. `src/biosdefs.h` is the one place every address is
  written, which is why the assembly is preprocessed gas `.S` rather than
  NASM, and why the assembly rule tracks headers.

  All five of Section 4's boot routes work: `src/linux.c` takes the x86 boot
  protocol's 64-bit entry (falcon-hook and its extent map appended as the
  UEFI loader appends them); `src/vcdres.S` is the resident INT 13h handler
  that serves an ISO as a CD, with `src/vcdcore.c` its host-tested half and
  `src/vcd.c` its installation; `src/pos.c` unwraps a UKI out of p3 rather
  than chainloading it, rebuilding sd-stub's credential cpio itself;
  `src/bsd.c` lays `media/bsdmd.c`'s two edits over the served medium; and
  `src/win.c` patches boot.wim with core's `wim`, serving the appended tail
  from extended memory through INT 15h AH=87h. `src/heap.c` is the one
  allocator, for that tail alone.

  It shares falcon-core unchanged: the read callback is the per-platform
  boundary. What a BIOS build had to provide that firmware used to is the
  thing to remember here - `lmode.S` sets CR4.OSFXSR because falcon-core's
  AES-NI path needs it, and nothing may be written outside the gigabyte
  `lmode.S` identity maps.
- `docs/` DESIGN.md (authoritative) and later Manual.md. `tools/` at the root
  holds only what spans the repo: `analyze.sh`, and `coverage/`, the corpus
  harness of docs/notes/coverage-roadmap.md (a pinned ISO manifest, one rig
  stick per ISO, each booted under OVMF in three Secure Boot states and
  judged from serial, a DHCP capture and the media read after the kernel
  starts, rendered to `docs/coverage.md`). `coverage/Dockerfile` is the
  environment it runs in and `coverage/harness.sh` runs anything inside it
  (`harness.sh make test`, `harness.sh` alone for the corpus), which is how
  the measurement stops being tied to one machine's installed packages. It
  needs `--privileged` and the host's `/dev` because stick building calls
  `losetup -P`.

`make check-tools` verifies the toolchain. Objects land in
`build/obj/<flavour>/<source path>.o` (flavours: `efi-release`, `host`,
`winjump`, `hook`, `test`), binaries in `build/bin/` (the loader is
`build/bin/falcon-boot.efi`), fixtures and rig artifacts in `build/`.
`make test` runs every component's test binary, `make coverage` runs them
strictly and reports over all of them, `make test-fixtures` builds the images
the image-backed suites need. `compile_commands.json` is regenerated at the
repo root by the default target.

## Tone: no preaching

This applies to everything with a reader: strings the loader prints, CLI
output, docs, and the assistant's own replies in a session.

State the fact. Stop. A reader who wants the reasoning opens `docs/DESIGN.md`,
which is where the reasoning belongs.

Cut:

- Restating a security invariant at the point of use. "This changes nothing
  about what may run", "the loader cannot and must not do that itself". The
  invariants are in Section 7 and in this file. A prompt is not the place to
  re-argue them.
- Reassurance. "Nothing here decides what may run", "this is safe".
- Consequences the reader already knows. "Anything unsaved is lost" before a
  reboot.
- Justifying the design to the person using it. Confidence needs no defence.

Keep:

- What is about to happen.
- What the user must do.
- What went wrong, and what to do about it.

A confirmation is a question and its keys, not an essay:

    Reboot into firmware setup? y to reboot, any other key to go back.

Code comments are exempt. They argue, at length, for the next maintainer.
That is the one audience that wants the reasoning inline.
