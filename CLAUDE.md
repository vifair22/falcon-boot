# airies-boot

A single-purpose, auditable multi-boot loader for UEFI x64. A distilled Ventoy:
drag-and-drop ISO booting plus a personal LUKS-backed OS booted via a UKI.

## Read this first

**`docs/DESIGN.md` is the authoritative design and the source of truth.** Read it
before proposing or changing any architecture. It argues its decisions and
records the alternatives that were considered and rejected, so do not reintroduce
a rejected approach without re-reading why it was rejected. `README.md` is the
short public summary of the same model.

Status: pre-1.0 MVP. Steps 1-6 of the build order in `docs/DESIGN.md` Section
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

The passphrase handoff (step 6) is in: `src/credfs` publishes a synthetic
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

- `src/efi/` EFI application entry point and glue: freestanding C11, MS x64 ABI,
  built to PE32+ with `x86_64-w64-mingw32-gcc`. No gnu-efi, no EDK2. Our own
  minimal UEFI headers live here too.
- `src/exfat/` read-only exFAT driver, EFI-free (device access through a read
  callback) so it is testable off the firmware.
- `src/menu/` text menu over Simple Text Output/Input, plus the masked
  passphrase prompt (`prompt.c`, its own TU so "what has held the secret" has
  a one-file answer).
- `src/pos/` the personal-OS path (DESIGN Section 5): LUKS2 volume discovery,
  unlock, the ext4 inside, UKI discovery, and the shim-verified load. The only
  module that ever holds the passphrase. Glue over `src/luks` and `src/ext4`;
  it writes no crypto and no filesystem code.
- `src/credfs/` the synthetic read-only filesystem the personal-OS path
  publishes so the UKI can be loaded by device path and sd-stub can find the
  passphrase credential beside it (DESIGN Section 5.1). Serves a caller-built
  in-RAM tree; `credfs_efi.h` carries the File Protocol's full provider-side
  surface, which `src/efi/sfs.h` deliberately stubs out on the consumer side.
- `src/lib/` freestanding support routines: `mem*` and the UCS-2 helpers.
- `src/cli/` host tooling (Linux userland C), arriving at step 7 of the build
  order. May shell out to `cryptsetup`, `sbsign`, `mkfs.*`, `ukify`. The `sign`
  tool is a portable static binary shared between the host and the on-stick
  signing appliance.
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
`build/<variant>/<module>/`, the linked loader in `build/bin/airies-boot.efi`,
the staged ESP in `build/esp/`; `compile_commands.json` is regenerated at the
repo root by the default target.
