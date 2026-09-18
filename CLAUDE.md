# falcon-boot

A single-purpose, auditable multi-boot loader for UEFI x64. A distilled Ventoy:
drag-and-drop ISO booting plus a personal LUKS-backed OS booted via a UKI.

## Read this first

**`docs/DESIGN.md` is the authoritative design and the source of truth.** Read it
before proposing or changing any architecture. It argues its decisions and
records the alternatives that were considered and rejected, so do not reintroduce
a rejected approach without re-reading why it was rejected. `README.md` is the
short public summary of the same model.

Status: pre-1.0 MVP. Build-order steps 1-7 are done (see docs/DESIGN.md
Section 11 status header); host tools falcon-boot-mkstick and
falcon-boot-sign exist and are proven end to end (a mkstick-created image
boots Secure Boot ON with its own generated MOK and delivers the p4
credential). Next: step 8 (signing appliance), step 9 (permissive-mode UX),
step 10 (real hardware), plus the loader-side blessed-boot path consuming
src/bless/manifest.c (Section 8.1). Older status text follows for context: Steps 1-6 of the build order in `docs/DESIGN.md` Section
11 are done, with one carve-out named below. The mingw PE toolchain and the
QEMU/OVMF rig are proven; the shim question is settled (Fedora shim-x64 16.1,
`SHIM_IMAGE_LOADER`, `docs/notes/shim-spike.md`) and the loader carries the
`.sbat` section that needs. The loader enumerates `*.iso` from exFAT into a
text menu labelled with family and signature presence, boots an entry through
the platform's verifying `LoadImage` of the ISO's `\EFI\BOOT\BOOTX64.EFI`,
and shows the Secure Boot state banner. The personal-OS entry is pinned at the
top of the menu when a pbkdf2-capable LUKS2 volume is in sight, and it works
end to end: masked passphrase prompt, LUKS2 unlock of p3, read-only ext4
inside, `/uki/*.efi` discovery (`/EFI/Linux` as fallback), and `LoadImage` of
the image through the shim-verified path.

The passphrase handoff (step 6) is in: `src/loader/credfs.c` publishes a synthetic
read-only filesystem carrying the UKI plus
`<uki>.efi.extra.d/cryptsetup.passphrase.cred`, the UKI is loaded BY DEVICE
PATH off it, and sd-stub packs the credential into the initrd where
`systemd-cryptsetup` will find it. Proven with a real sd-stub UKI under both
Secure Boot states.

Proven headless under OVMF against `build/test-stick.img`, a real
four-partition GPT disk (`make test-stick`, `make run-stick` /
`run-stick-secboot`), with Secure Boot both off and on. Under Secure Boot on,
shim verifies the MOK-signed image read out of the encrypted partition and
refuses an unsigned one with `EFI_SECURITY_VIOLATION` and an actionable
message; the menu survives both. p4's argon2id keyslot is recognised and
skipped without a second prompt.

Next: **step 7, the host tooling** (`src/cli`, DESIGN Section 8): mkstick, the
MOK keypair, the UKI build, and the `sign` bless tool. The carve-out still
outstanding from step 4 is the injection framework's runtime half (extent map
and cpio append exist; per-family injectors do not). The end of the handoff
chain is also still proof-by-reading rather than proof-by-running: the fixture
UKI shows the credential arriving in the initramfs, but nothing in the rig runs
a systemd initramfs that actually unlocks p4 with it.

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

One flat top-level Makefile builds everything; there are no per-directory
Makefiles.

`src/` is grouped by layer and by boot path, one level deep, headers next to
their `.c` files. Everything outside `src/cli/` and `src/winjump/` links into
the loader.

- `src/efi/` the EFI application entry point (`main.c`), the `.sbat` section,
  and our own minimal UEFI headers: freestanding C11, MS x64 ABI, built to
  PE32+ with `x86_64-w64-mingw32-gcc`. No gnu-efi, no EDK2.
- `src/loader/` the shim-verified load path (`loadops.c`, DESIGN Section 7.6)
  and `credfs.c`, the synthetic read-only filesystem a UKI is published on so
  it can be loaded by device path and sd-stub finds the passphrase credential
  beside it (DESIGN Section 5.1). `credfs_efi.h` carries the File Protocol's
  full provider-side surface, which `src/efi/sfs.h` deliberately stubs out on
  the consumer side.
- `src/console/` text menu over Simple Text Output/Input, plus the masked
  passphrase prompt (`prompt.c`, its own TU so "what has held the secret" has
  a one-file answer).
- `src/disk/` GPT parsing (`part.c`, EFI-free; `partefi.c`, its firmware
  half) and the single device-discovery pass (`discover.c`): enumerates the
  Block I/O handles once, classifies each (media, GPT role, LUKS2 magic,
  filesystem identification), and hands every consumer the same inventory.
- `src/fs/` the filesystem driver registry (`fs.c`, DESIGN Section 9: one
  `fs_ops` vtable, opaque caller-allocated storage, unified dirents and status
  codes) and the read-only drivers only it calls, `exfat.c` and `ext4.c`.
  Consumers select a driver through the registry and never name one. EFI-free
  (device access through a read callback), so testable off the firmware.
- `src/luks/` LUKS2 header parse, keyslot unlock, AF merge. EFI-free.
- `src/crypto/` SHA-256/512, HMAC, PBKDF2, AES-XTS (AES-NI with a portable
  fallback). EFI-free, the only crypto in EFI-land.
- `src/pe/` PE/COFF header layout read from a file's bytes: optional-header
  fields, data directories, sections. The one PE walker; the ISO survey, the
  UKI labeller and the boot predictions all use it. EFI-free.
- `src/secboot/` everything that reads platform trust state, for UX only:
  `sb.c` (SecureBoot / SetupMode / MokSBStateRT) and the boot predictions
  (DESIGN Section 7.8): `der.c`/`x509.c`/`authenticode.c` read an image's
  signature block and Authenticode hash, `siglist.c` the db/dbx/MokList
  stores and SBAT levels, `trust.c` the verdict, `probe.c` the glue over a
  read callback, all EFI-free; `trustefi.c` reads the variables. A
  prediction is a label and no boot path takes one.
- `src/iso/` the ISO path (DESIGN Section 6): virtual media over a p2 file
  (`iso.c`, `overlay.h`), family detection (`family.c`), and the initrd cpio
  append of the injection framework (`cpio.c`).
- `src/pos/` the personal-OS path (DESIGN Section 5): LUKS2 volume discovery,
  unlock, the ext4 inside, UKI discovery and naming (`osrel.c`), and the
  shim-verified load. The only module that ever holds the passphrase. Glue
  over `src/luks` and `src/fs`; it writes no crypto and no filesystem code.
- `src/bless/` the blessed path (DESIGN Section 8.1): the manifest parser and
  writer (`manifest.c`, `manifest_write.c`, shared with the host tool), the
  record cache (`blesscore.c`, EFI-free) and the launch (`bless.c`).
- `src/lib/` freestanding support routines: `mem*` and the UCS-2 helpers.
- `src/cli/` host tooling (Linux userland C, never linked into the loader):
  `mkstick`, `sign`, and their shared `common`. May shell out to
  `cryptsetup`, `sbsign`, `mkfs.*`, `ukify`. The `sign` tool is a portable
  static binary shared between the host and the on-stick signing appliance.
- `src/winjump/` the WinPE jump binary (DESIGN Section 6.3), a separate
  Windows PE target.
- `docs/` DESIGN.md (authoritative) and later Manual.md.
- `tools/` host scripts: the QEMU + OVMF rig (`run-qemu.sh`, with `--secboot`,
  `--stick` and `--headless`), the exFAT fixture builder (`mk-test-exfat.sh`),
  the El Torito payload ISO (`mk-test-iso.sh`), the throwaway test MOK
  (`mk-test-mok.sh`), the sd-stub fixture UKI the credential handoff is tested
  against (`mk-test-uki.sh`, `make test-uki`, with its `/init` in
  `tools/uki-init/`), and the four-partition GPT test stick
  (`mk-test-stick.sh`, `make test-stick`) that the personal-OS path needs.
  OVMF (plain and secboot) is present on this machine.

`make check-tools` verifies the toolchain. Objects land in
`build/<variant>/<module>/`, the linked loader in `build/bin/falcon-boot.efi`,
the staged ESP in `build/esp/`; `compile_commands.json` is regenerated at the
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
