# falcon-boot

A single-purpose, auditable multi-boot loader for x86-64 UEFI, with an opt-in
BIOS loader that is i586 and so runs on 32-bit machines too. A distilled Ventoy: drag-and-drop ISO booting plus a personal
LUKS-backed OS booted via a UKI.

## Read this first

**`docs/DESIGN.md` is the authoritative design and the source of truth.** Read it
before proposing or changing any architecture. It argues its decisions and
records the alternatives that were considered and rejected, so do not reintroduce
a rejected approach without re-reading why it was rejected. `README.md` is the
short public summary of the same model.

DESIGN Section 12 draws every boot route of both loaders, traced from the
code, from power-on to the booted system standing on its own. A change to a
route (its order, a fallthrough, what it hands over, what stays resident)
updates its diagram in the same commit. Section 12 is where a route's shape
is looked up before its code is read.

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
Measured on real hardware (2026-09-20), UEFI with Secure Boot on and off:
the personal OS end to end, blessed Linux ISOs (blessed from the menu, with
`BootNext` back to the stick), a stock Windows ISO, and with Secure Boot on
D3a (an unblessed Fedora) and D3b (an unblessed Kubuntu). Measured on real
hardware (2026-09-23), BIOS through CSM: the personal OS end to end, p4
unlocked from the credential with no second prompt. Measured on real
hardware (2026-09-24), BIOS: the since-retired 32-bit stick to its Arch
Linux 32 personal OS on a Windows 7 era machine. Measured on real
hardware (2026-09-26 and 27), BIOS on a Pentium III with no 64-bit mode
(the ThinkPad A21m): falcon jump's five routes, and station-legacy
installed on its internal disk with every row (the chains to Windows 2000,
memtest, DevLink recovery's UKI, jump's CD, USB and CF routes), updated in
place from a station bundle (DESIGN Section 13.14). Measured on real
hardware (2026-09-29), UEFI with Secure Boot on (the ThinkPad T430:
coreboot and edk2, its own key as PK, KEK and db, no shim): the UEFI
station installed with `--no-shim`, and its TOTP code released by a TPM
1.2, enrolled from the two event logs (DESIGN Section 13.15). Still
unmeasured on hardware: the stick's other four BIOS routes (direct boot, the virtual CD,
Windows media, BSD).

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

The repository is the loaders in `apps/`, and the components they and the
host programs are built from, each component with its own `src/`, `tests/`
and `tools/`. One non-recursive Makefile builds all of it: the root
`Makefile` holds the shared toolchains, flags, test runner and compile
database, and `include`s a `build.mk` from each component and an `app.mk`
from each app. There is one make invocation and one
dependency graph; nothing recurses. Each translation unit, tests included,
sees only its own component's `src/` and the ones it builds against (the UEFI
loader efi+core, falcon-bios bios+core, the appliance appliance+cli+core). A
module directory name is unique within every such include set, so
`#include <fs/exfat.h>` means one file wherever it is written, while
falcon-efi and falcon-bios, which never share a set, may both have a `pos/`.
`tools/analyze.sh` fails on a name that appears twice within a set.

- `apps/` the loaders, one directory per build: `stick/` (every route, the
  loader on a falcon stick; `all` builds its UEFI image, `make bios` its
  BIOS one), `station/` (DESIGN Section 13, falcon as a machine's own
  boot loader; `make station`; its UEFI build alone has the TPM-backed
  TOTP check of Section 13.15), `station-legacy/` (Section 13.10, the
  BIOS station plus jump's CD, USB and CF routes and DevLink recovery's
  UKI; `make station-legacy`, `make devlink-uki`, and `make station-bundle`,
  the update a station in the field takes, Section 13.14) and `jump/` (DESIGN Section 14, a boot
  floppy for an old BIOS machine; `make jump` builds the whole 1.44 MB
  image with mtools, its core in the FAT12 volume's reserved sectors). An app is its entry points (`efi_main.c`,
  `efi_sbat.c`, `bios_main.c`) and an `app.mk` that lists, one line per
  module with its reason, what the build links. falcon-efi and falcon-bios
  hold the modules and say how an app is linked (`efi_app`, `bios_app`);
  they carry no entry point. What an app does not list, it cannot run.
- `falcon-core/` the EFI-free libraries every other component links. Nothing
  here knows about firmware: device access goes through a read callback and
  every buffer is the caller's, which is what lets the loader, the host tools
  and the appliance run one implementation and the host tests exercise it.
  - `crypto/` SHA-1 (WIM stream names, and HMAC-SHA1 for the TOTP code and the
    TPM 1.2 session), SHA-256/512, HMAC, PBKDF2, BLAKE2b and Argon2id (the
    memory block the caller's, lanes filled in turn), AES-XTS (AES-NI with a
    portable fallback): the only crypto in the stick's loader. The UEFI
    station also links `CORE_CRYPTO_TPM` (`cfb`, `bn`, `p256`, `kdf`, `rsa`,
    `otp`: AES-CFB, P-256 ECDH, the TPM 2.0 KDFs, RSAES-OAEP and the RFC 6238
    code) for its TOTP check (DESIGN Section 13.15); nothing there signs or
    verifies an image.
  - `tpm/` the TPM command layer, 2.0 and 1.2, over one transport
    callback and one randomness callback (DESIGN Section 13.15): the
    seed's NV index sealed to PCRs, the salted, encrypted session that
    reads it (ECDH P-256 and AES-CFB on 2.0, an MGF1 transport to the SRK
    on 1.2), the pinned key checks, and `evlog.c`, the firmware's event
    log replayed to the station's own measurement, which is what
    enrollment seals to. The UEFI station reads the seed with it through
    the firmware, mkstick enrolls through `/dev/tpmrm0` or `/dev/tpm0`, and
    `make test-tpm` runs it against swtpm directly.
  - `fs/` the filesystem driver registry (`fs.c`, DESIGN Section 9: one
    `fs_ops` vtable, opaque caller-allocated storage, unified dirents and
    status codes) and the read-only drivers only it calls, `exfat.c`,
    `ext4.c`, `fat.c` (FAT12/16/32 with long names; one driver, since the
    width is derived from the cluster count and only the FAT entry read
    differs), `iso9660.c` (Rock Ridge, Joliet) and `udf.c`. `jbd2.c` is
    ext4's journal replayed in memory and never written, for a shelf its OS
    left mounted (hibernated, or the power went; DESIGN Section 5.4):
    `fs_mount_journal` reads such a volume through a table of the journal's
    newer copies, its memory the caller's (`fs_jmem`), where `fs_mount`
    still refuses it. `crc32c.c` is the checksum both share. Each driver's
    adapter to the vtable is its own file (`fs_<driver>.c`), and the table
    is one a build picks: `fs_reg_all.c` everywhere, `fs_reg_ext4.c` in the
    UEFI station (`CORE_FS_EXT4`) and `fs_reg_station.c`, ext4 and FAT, in
    the BIOS one (`CORE_FS_STATION`), which is how those builds link only
    what they mount. Consumers select a driver through the registry and never name
    one; `fs_lookup` resolves a path. `udf.c` also carries the few record edits the Windows patch needs
    (a File Entry's extents, a Partition Descriptor's length, an anchor),
    applied to caller buffers, never to media.
  - `luks/` LUKS2 header parse, keyslot unlock (argon2id and pbkdf2; the
    caller supplies argon2id's memory through `luks_kdf_mem`, and a slot it
    cannot supply is `LUKS_ERR_KDF_MEMORY`, which both loaders turn into a
    page giving what the slot needs and what is free), AF merge, the falcon
    token.
  - `part/` MBR and GPT read as one partition table (`table.c`).
  - `ident/` the identity record (DESIGN Section 3.1): its three encodings
    (`ident.c`) and the `/.falcon` reader over the fs registry
    (`identfs.c`). What a partition is, and whose, comes from here.
  - `pe/` PE/COFF header layout read from a file's bytes; the one PE walker.
  - `trust/` the boot predictions (DESIGN Section 7.8): `der`, `x509` and
    `authenticode` read an image's signature block and Authenticode hash,
    `siglist` the db/dbx/MokList stores and SBAT levels, `trust` the verdict,
    `probe` the glue over a read callback. A prediction is a label and no boot
    path takes one.
  - `media/` what an image is: family detection (`family.c`), the initrd cpio
    writer (`cpio.c`), `.osrel`/`.uname` reading (`osrel.c`), an ISO's own
    boot configuration (`bootcfg.c`, read through `bootfs.c` over a mounted
    image), the extent map handed to falcon-hook (`extmap.c`), which disk
    holds the library a map names (`extfind.c`, for the hook, winjump and
    falcon-vdisk) and the hooked
    boot's cmdline (`hookcmd.c`), the grub.cfg D3b serves in place of
    an ISO's own (`grubcfg.c`) and the two edits that make a BSD medium boot
    itself as a memory disk (`bsdmd.c`). `uefiboot.c` and `biosboot.c`
    are what a row's platform tags are read from (DESIGN Section 7.8):
    whether a UEFI machine can boot the medium at all (`{BIOS ONLY}`), and
    whether falcon-bios can (`{EFI ONLY}`) and needs long mode for it
    (`{64-BIT ONLY}`), each from the medium's own bytes, UNKNOWN unless
    proved. `survey.c` is what a menu is built
    from: the directory walk over a mounted volume, the extension rule, the
    sort and the folder view. Both loaders use it, so neither can have its
    own idea of what is on a stick. `shelf.c` is the personal-OS chooser's
    rows: the UKIs in p3's `/uki` (or `/EFI/Linux`), their `.osrel` names,
    order and qualifiers, again for both loaders. `strows.c` is the
    station's list as data, both stations' (DESIGN Section 13.7): the
    generated names, what a STATION.INI section changes or adds (a
    firmware entry or an ESP on UEFI, a chain on BIOS), the order and the
    default, and the rows' words (`STROWS_LABEL_*`), which `station
    install` and `status` use too; `vbr.c` what a partition's boot sector
    starts, for the BIOS station and the installer; `stationini.c` parses
    and writes the file. `nextboot.c` is the
    next-boot marker's value rules (decode, path match, the `:` reserved
    names) for both loaders, and the BIOS marker sector's format and dating.
    `eltorito.c` reads a medium's boot
    catalog and `medium.c` assembles what a medium holds into a set of
    mounted volumes (UDF, then ISO9660, then the El Torito boot image) over
    one read callback, which is the job the firmware's ConnectController does
    today and the per-platform boundary a BIOS build would need
    (docs/notes/iso-reader-route.md).
  - `gz/` inflate and the gzip member around it (RFCs 1951, 1952): every
    BIOS stage 3 is stored as a gzip member, which stage 2 inflates and
    checks, and so is every falcon jump module; no allocation and no .bss.
  - `wim/` LZX decompression and the WIM reader, plus the boot.wim patch
    (`wim_patch_build`): a new header and a tail appended after the
    original's end, the original bytes untouched.
  - `manifest/` the blessed-artifact manifest parser and writer (DESIGN
    Section 8.1), shared by the loader and the sign tool.
  - `tui/` how a loader screen is laid out: the centred content block, a
    menu's logo (left out when the list would not fit), banner, status
    lines under it (the UEFI station's TOTP code), list and
    key hint, a page started a third of the way
    down, the route code's place, row composition and the colours. Geometry
    only, no drawing; both loaders draw from it, which is what keeps their
    screens the same cell for cell.
  - `lib/` the freestanding `mem*` (`string.c`, linked into the loader
    flavours only, since every hosted build has a libc) and the UCS-2
    helpers (`ucs2.c`, linked into every flavour including the hosted
    ones, which is what lets host-tested core code handle a filename).
  - `tests/` the core test suite, plus `tests/support/` (`tests.h`, `synth`,
    and `hostshim/`, which answers `<lib/string.h>` with libc) that every
    component's tests link. `tools/` builds the fixtures: exFAT, ext4, LUKS2,
    trust.
- `falcon-efi/` the loader: freestanding C11, MS x64 ABI, built to PE32+ with
  `x86_64-w64-mingw32-gcc`. No gnu-efi, no EDK2.
  - `efi/` our own minimal UEFI headers. The entry points and `.sbat`
    sections are the apps' (`apps/stick/`, `apps/station/`).
  - `nxt/` the stationary build's route to the firmware's other boot
    entries (NXT, BootNext and a reset): `nxtcore.c` parses BootOrder and
    Boot#### (the description, and the partition and file its device path
    names) and is host-tested, `nxt.c` reads the variables.
  - `esp/` the station's other way into a system (DESIGN Sections 13.4,
    13.6; routes ESP and MEM): a file on a firmware-mounted volume
    (`bootmgfw.efi`, `memtest.efi`) loaded by device path through
    `loadops`, and the reads STATION.INI and the ESP rows need.
  - `loader/` the shim-verified load path (`loadops.c`, DESIGN Section 7.6)
    and `credfs.c`, the synthetic read-only filesystem a UKI is published on
    so it can be loaded by device path and sd-stub finds the passphrase
    credential beside it (DESIGN Section 5.1). `credfs_efi.h` carries the
    File Protocol's full provider-side surface, which `efi/sfs.h` deliberately
    stubs out on the consumer side.
  - `console/` the text menu over Simple Text Output/Input, drawn from core's
    `tui`, plus the masked passphrase prompt (`prompt.c`, its own TU so
    "what has held the secret" has a one-file answer).
  - `discover/` the single device-discovery pass (`discover.c`) and the
    device-path half (`partefi.c`: same disk, start LBA): reads the boot
    partition's record for the install ID, enumerates the Block I/O handles
    once (the boot disk's first), reads each one's record, and hands every
    consumer the rows that carry the install ID. `disks.c` (the station
    only) reads each whole disk's table so a partition handle or a firmware
    entry's HARDDRIVE node can be named as STATION.INI names it.
  - `secboot/` Secure Boot state for UX (`sb.c`) and the variable reads the
    boot predictions judge against (`trustefi.c`).
  - `tcg/` and `totp/` the UEFI station's TOTP check (DESIGN Section
    13.15), linked by that app alone: `tcg.c` is core's `tpm/` transport
    through `EFI_TCG2_PROTOCOL` SubmitCommand or `EFI_TCG_PROTOCOL`
    PassThroughToTpm (no TPM driver) and its randomness (EFI_RNG, RDRAND,
    the TPM, the counter, hashed together); `totp.c` reads the seed once
    before anything is loaded, keeps it until the next handover, and
    composes the code line the menu draws under its banner and pos.c's
    hook draws above the passphrase prompt. A label: it enforces nothing.
  - `bsd/` BSD media served as its own memory disk (Section 6.4; M7):
    the two byte edits core computes, laid over the virtual CD, and the
    memory check FreeBSD's staging area needs.
  - `mem/jnlmem.c` the pages a shelf's journal table lives in (DESIGN
    Section 5.4), shared by `pos/` and `discover/`.
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
    `/init` in `uki-init/`, the four-partition test stick, and
    `mk-test-station.sh`, a disk `mkstick station install` puts the station
    on, `make test-station`) and the two spikes
    whose findings are in `docs/notes/`.
- `falcon-cli/` host tooling, Linux userland C never linked into the loader:
  `src/mkstick`, `src/sign`, and their shared `src/common`. May shell out to
  `cryptsetup`, `sbsign`, `mkfs.*`; it builds UKIs itself (`sign/uki.c`,
  byte-identical to ukify). The sign tool is also linked
  static for the appliance. `mkstick station totp enroll|reseal|remove`
  (`mkstick/totp.c`, its pure half `totpcore.c`) enrolls the UEFI
  station's TOTP seed from the machine's own OS (DESIGN Section 13.15).
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
  console program, built a second time for i686 (`winjump32.exe`,
  `i686-w64-mingw32-gcc`) for 32-bit Windows media. The UEFI loader embeds
  the x64 build as a generated C array; falcon-bios embeds both and puts in
  the one the boot image's `winpeshl.exe` is built for. It links no C
  runtime at all (`-nostdlib`, its own entry point and the few string
  routines it needs): a missing import kills it before its first log line, so
  it imports only kernel32, advapi32 and user32, which every WinPE from
  Vista's on carries, and loads VirtDisk.dll at run time.
- `falcon-vdisk/` the WinPE virtual-CD kernel driver (DESIGN Section 6.3),
  for Windows older than 8, whose WinPE cannot attach an ISO (and 2000/XP,
  which have no WinPE). A native-subsystem NT driver (`.sys`) built for both
  widths from `src/vdisk.c` with mingw's DDK headers and `ntoskrnl`/`hal`
  import libraries (no separate WDK). It presents the ISO as a read-only
  CD-ROM CDFS mounts, reading the ISO's raw disk extents (the same map the
  BIOS virtual CD and falcon-hook use) since WinPE cannot read the exFAT the
  ISO lives on. winjump loads it (CreateService/StartService) and hands it
  the extents through `src/vdiskio.h`. 32-bit Windows loads it unsigned;
  64-bit needs the served boot config put into test-signing mode and the
  driver signed with a throwaway cert, which Secure Boot on refuses (the
  platform opt-out, DESIGN Section 7). Runs only in the installer's WinPE;
  the installed Windows keeps signing enforcement.
- `falcon-hook/` the program the loader appends to an ISO's initrd (DESIGN
  Section 6.4): freestanding static Linux, no libc, built twice, x86-64
  (`falcon-hook`) and i586 (`falcon-hook32`) from the same source: `src/sys.c`
  is its system calls and startup, and the only file that knows the width. It runs as `rdinit=`, reads the extent map from
  the initrd (a direct boot, or the credential sd-stub packs beside a
  blessed wrap) or efivarfs (an older wrap), finds the ISO library it names
  by the `/.falcon` record at the map's LBA (core's `media/extfind.c`, with
  exFAT alone behind the registry), maps the ISO
  read-only with dm-linear as `/dev/mapper/falcon-iso` under the ISO's label
  (a loop device when the kernel has no dm and the ISO is contiguous;
  `src/dimod.c` brings loop in from a Debian installer ISO's own pool), makes
  the per-distro nudges and pre-mounts in `src/hookcore.c`'s tables, and
  execs the distro's init. `src/hookcore.c` is its host-tested half; the
  loaders embed the stripped binaries as generated C arrays (the UEFI loader
  the x86-64 one, falcon-bios both, appending whichever matches the kernel).
  `tools/run-hook-rig.sh` boots it under QEMU without the loader.
- `falcon-bios/` the BIOS loader (docs/notes/bios-route.md). The image is
  opt-in and not built by `all`, and so is putting it on a stick: `mkstick
  bios <target>` installs it, `mkstick create` does not. A BIOS machine has
  no Secure Boot, no shim and no db, so this route has no blessed path and
  installing it changes what a stick can reach (bios-route.md Section 7.3).
  One layout for every BIOS install (DESIGN Section 3.3): stage 1 in the
  440 bytes of boot code at the head of LBA 0, with stage 2's place patched
  in by the installer (LBA and length, read by AH=42h or by CHS); stage 2
  before the first partition (LBA 34 on GPT, 1 on MBR); stage 3 and the
  next-boot marker in the reserved sectors of the FAT `boot` partition (a
  stick's ESP), at the sectors its identity record names. Stage 2 finds that
  partition by record (`src/disk/bootpart.c`), and stage 3 takes only
  partitions carrying the boot record's install ID, on any disk
  (`src/disk/install.c`). Its host-testable C is in
  `make test` and in `compile_commands.json` like everything else: the three
  halves with no BIOS in them are `vcdcore.c` (the virtual CD's run and
  overlay arithmetic), `linuxcore.c` (the addresses the x86 boot protocol is
  handed, and the four-byte padding between concatenated initrds) and
  `e820.c`'s arithmetic (`e820_place_in` picks an initrd's address out of a
  memory map the test writes; `e820_carve` is the map a chainloaded loader
  is handed, with what the loader leaves behind taken out). All three fail
  silently on a real machine, which is why they are separate from the code
  that does the writing. `make bios` builds it, `make test-bios-img` makes
  a disk the rig boots with `falcon-efi/tools/run-qemu.sh --bios`, and
  `make test-bios-probe` makes the medium that checks the resident's INT 15h
  half from real mode (`tools/memprobe.S`). `make test-jump` boots every
  falcon jump route under SeaBIOS and judges it from serial
  (`tools/run-jump-rig.sh`); its CF case runs on a QEMU that
  `tools/qemu-pcc/build-qemu.sh` builds with `falcon-pcc.c`, a model of the
  A21m's PC Card bridge, since QEMU has none (its `lba48=on` card is the
  `cf48` case, a sparse card past 128 GiB that `hw/ata/atadisk.c` reads
  with 48-bit commands). `make station` links the
  station's stage 3 (DESIGN Section 13) from `src/station/station.c`,
  which `apps/station/bios_main.c` and `apps/station-legacy/bios_main.c`
  both run (`station_main`, with a hook for station-legacy's own rows):
  the UKIs off the shelves carrying its install ID, memtest86+, and
  `src/mbr/chain.c`, a primary partition's boot sector (the MBR copy at
  0000:0600, DS:SI at its entry, the BPB made to agree) or another disk's
  MBR, with `src/mbr/swapres.S` the one resident, an 84-byte INT 13h hook
  that swaps a chained disk with 0x80; `src/mbr/vbrcore.c` (the BPB fix,
  the entry) is its host-tested half, core's `media/vbr.c` what a boot
  sector is (the loader signatures and their words, shared with the
  installer), and core's `media/strows.c` the list both stations and
  `station status` share. `make
  test-station-bios` boots every chain under SeaBIOS
  (`tools/run-station-rig.sh`, with `tools/rigboot.S`, a boot sector that
  reports what it was handed), and `make test-station-fixtures` real
  systems behind the station, FreeDOS and Windows 98 SE from the fixture
  disks of `tools/fixtures/`, put on by the installer through qemu-nbd
  (`tools/run-station-fixture-rig.sh`). `make test-station-legacy` puts
  station-legacy on a copy of the A21m's disk with the installer through
  qemu-nbd (`tools/mk-a21m-station.sh`, a qcow2 overlay, the original never
  written) and boots every row of its menu (`tools/run-legacy-rig.sh`).
  The menu's countdown is timed by the tick count and the RTC together
  (`src/console/cdclock.c`, host-tested), since a machine can reach it
  with IRQ0 never delivered; the station rig's `no-ticks` case boots
  through `tools/tickstop.S`, which masks IRQ0 first. `make test-core` boots the core matrix cells (the stick
  under both firmwares alone, beside a second stick, an MBR disk and another
  install's records; a 4Kn stick on NVMe; the station, alone and with a
  stick plugged in) and judges each from
  serial (`falcon-efi/tools/run-core-rig.sh`); `make test-station-uefi`
  does the same for the UEFI station's own cases
  (`falcon-efi/tools/run-station-rig.sh`), and `make
  test-station-installed` for stations the installer put on a disk, 4Kn
  GPT and MBR, an ESP on MBR and Windows on a second disk included
  (`falcon-efi/tools/run-installed-rig.sh`). `make test-tpm-rig`
  (`falcon-efi/tools/run-tpm-rig.sh`, swtpm through `swtpm.sh`) boots the
  UEFI station with a TPM 2.0 and 1.2 in both Secure Boot states; its
  `totp` cases enroll with mkstick between boots, from the event log the
  probe (`tools/tpm-probe/`) saved, and judge the code, the wire, the wipe
  at handover, the prompt page and the warning after a firmware change.

  `src/` is grouped by job, and a job the UEFI loader also does carries
  that module's name, so the two halves of a route sit side by side:
  `start/` (the MBR, stage 2, the mode switch, the thunk, the IDT and
  `biosdefs.h`), `console/` (serial, the screen, the prompt), `iso/` (the
  virtual CD and the ISO scan), `vfd/`, `mem/` (E820 and the heap),
  `disk/` (the read callback and the marker), `linux/`, `pos/`, `bsd/`,
  `win/` and `mbr/`; `jump/` (falcon jump's JUMP.INI parser, the module
  format and loader `jmod.c`/`jmodload.c`, the reverse thunk:
  `rthunk.S`, a real-mode INT 13h entry that calls a module's protected-mode
  `int13`, installed by `jres.c`, and `routes.c`, the CD, USB and CF routes
  jump and station-legacy both boot); `station/` (the BIOS station's list
  and routes, both station apps'); and `hw/`, jump's drivers, each built as
  a module linked at its slot (`jmod.ld.S`, `jump_module`): `hw/ata/` is
  ATA.MOD, ATAPI over the legacy IDE channels, with `atacore.c` its
  host-tested half; `hw/usb/` is the USB modules, `uhci.c`, `ohci.c` and
  `ehci.c` host controllers behind `usbhc.h` and `msc.c` the storage stack every
  controller's module links, with `usbcore.c` (descriptors, Bulk-Only)
  host-tested; `hw/pcc/` is PCC.MOD, a CompactFlash card in a PC Card
  slot (a CardBus bridge's ExCA registers, the card's CIS in `pcccore.c`
  host-tested) as an ATA disk through `hw/ata/atadisk.c`;
  `hw/scsi/` the SCSI blocks both speak; `hw/blk13.c` the
  INT 13h answers every module's resident gives; `hw/pci.h` and
  `hw/modrt.h` the port I/O a module does itself. `mem/lowmem.c` is the one place a resident's block is
  taken off the top of conventional memory, and `mem/jnlmem.c` hands a
  shelf's journal table the fixed region between stage 3's stack and
  jump's module slots (`JNL_REGION`, DESIGN Section 5.4). The
  entry points are the apps' (`apps/<app>/bios_main.c`). `start/boot_fd.S`
  is jump's stage 1, a FAT12 boot sector that reads the stick's stage 2 by
  CHS; `disk.c` reads by CHS too (AH=02h, `vfdcore.c`'s arithmetic) when a
  drive answers neither AH=48h nor AH=41h, which is a floppy.

  It is i586 code in flat 32-bit protected mode, paging off, and one image
  runs on every processor from the Pentium on (docs/notes/bios-route.md
  Section 6h; DESIGN Section 14.8 for the floor, which
  `tools/check-floor.sh` holds every linked image to): what a newer processor adds is taken by asking CPUID at run
  time, not by a second build. `pmode.S` switches the SSE state on only when
  CPUID reports SSE, falcon-core's AES path picks AES-NI by CPUID, and a
  64-bit kernel is refused with a sentence on a processor without long mode.

  Three stages, because stage 2 is linked where real mode and protected mode
  agree on what an address means and so cannot exceed about 33 KB.
  `src/start/boot.S` is the 512-byte MBR; `src/start/stage2.S`, `src/start/pmode.S` and
  `src/start/thunk.S` reach protected mode and keep what has to stay reachable
  from real mode (the thunk, and the gate at the head of stage 2 that
  publishes it at a fixed address);
  `src/start/load3.c` reads stage 3 in and jumps. Stage 3 links at a megabyte and
  holds the loader proper. `src/start/biosdefs.h` is the one place every address is
  written, which is why the assembly is preprocessed gas `.S` rather than
  NASM, and why the assembly rule tracks headers.

  All five of Section 4's boot routes work: `src/linux/linux.c` takes the x86 boot
  protocol's 32-bit entry, which boots 64-bit and 32-bit kernels alike
  (falcon-hook of the kernel's width and its extent map appended as the
  UEFI loader appends them); `src/iso/vcdres.S` is the resident INT 13h handler
  that serves an ISO as a CD, and the INT 15h half that keeps the memory it
  leaves behind out of the map the medium is handed, with `src/iso/vcdcore.c`
  and `e820_carve` its host-tested halves and `src/iso/vcd.c` its installation; `src/pos/pos.c` unwraps a UKI out of p3 rather
  than chainloading it, rebuilding sd-stub's credential cpio itself;
  `src/bsd/bsd.c` lays `media/bsdmd.c`'s two edits over the served medium; and
  `src/win/win.c` patches boot.wim with core's `wim`, serving the appended tail
  from extended memory through INT 15h AH=87h. `src/vfd/vfd.c` serves a
  medium whose El Torito boot image is a floppy emulation (memtest86+,
  Haiku) as a memory disk instead, its resident `src/vfd/vfdres.S` and its
  geometry `src/vfd/vfdcore.c`. `src/mem/heap.c` is the one
  allocator, for that tail and that image. `src/disk/marker.c` takes the next-boot marker
  (DESIGN Section 6.3.1): the sector of the boot partition its record
  names, written from Linux by `mkstick next-boot`, cleared before it is
  believed and honoured only within ten minutes by the RTC. It is the only code in falcon-bios that writes a disk (one sector,
  INT 13h AH=43h), and the stick's and the station's stage 3 link it.

  No failure ends in a halt, which on a machine with no serial cable is a
  hang: it ends in `bios_fatal` (`fatal_rm` before there is C), the reason
  on screen and serial, a key, then a reset. Stage 2 asks for CPUID
  (the i586 floor) and the BDA whether the EBDA reaches memory it writes
  before it enters protected mode, and `src/start/idt.S` gives protected mode an
  IDT, so a processor exception is reported through the same path
  (`src/start/fault.c`) rather than triple faulting.

  Stage 3 draws every screen through `src/console/screen.c`, the UEFI console's
  calls one for one (`screen_page` is `menu_screen`, `screen_menu` is
  `menu_run_keys`, `screen_menu_tagged` is `menu_run_probed`: tag columns,
  filled by the background probe on its own task, `src/task/`) over core's
  `tui`: cells and colours go straight into
  VGA text memory, and serial gets the ANSI sequences OVMF's terminal sends
  for the same calls, so the coverage harness reads both loaders one way.
  Diagnostics (volumes, config, addresses, cmdline) go to serial only, as
  `falcon-bios:` lines. Routes report back in a `boot_result` (`src/result.h`)
  and `bios_main.c` says it as the UEFI loader does, "<route> failed while
  <stage>" and the advice.

  It shares falcon-core unchanged: the read callback is the per-platform
  boundary, and core compiles clean as i586 because this build links all of
  it at that width. What a BIOS build had to provide that firmware used to
  is the thing to remember here - `pmode.S` sets CR4.OSFXSR because
  falcon-core's AES-NI path needs it, and nothing is placed at or above
  4 GiB (`ADDR_END`), since paging is off. The coverage harness judges it in
  two columns, `bios` (the host CPU) and `bios32` (a Pentium III).
- `docs/` DESIGN.md (authoritative) and Manual.md (the operator's reference:
  the route codes so far). `tools/` at the root
  holds only what spans the repo: `analyze.sh`; `arch-iso.sh`, sourced by
  the three scripts that take a kernel out of the rig's Arch ISO, which
  default to the copy on this stick's p2 (mounted read-only for the run
  when it is not already; `ARCH_ISO` overrides); and `coverage/`, the corpus
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

## One screen, two loaders

The UEFI loader and falcon-bios show the same screens: the same layout,
colours, key hints and sentences. The layout is one implementation
(`falcon-core/src/tui`), and so are the menu's rows (`media/survey.c`) and
the chooser's (`media/shelf.c`). The sentences are written twice, once per
loader, so a screen or a line changed on one loader is changed on the other
in the same commit. Where the platforms really differ (no trust tags, no
firmware-setup row, no status code on BIOS) the screen leaves the part out
rather than saying something else. The platform tags are each loader's own
(`{BIOS ONLY}` on UEFI, `{EFI ONLY}` and `{64-BIT ONLY}` on BIOS), in the
same column and colours. Both sticks probe every entry in the background
for as long as a menu is up, on a task of their own (`task/`, a coroutine
that yields at the read layer), `...` in a row's tag column until it is
filled and a spinner in the banner's last cell while there is work; every
loader says each startup step on the page's first row (`menu_busy`,
`screen_busy`) until its menu is up (DESIGN Section 7.8).

The pattern every route follows: its code, a page, `booting: <label>`; the
route's own lines up to the handover; and when it comes back, a new page
with the label, `<route> failed while <stage>`, the advice, and "press any
key to return to the menu." Diagnostics a person at the machine cannot act
on go to serial only.

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
