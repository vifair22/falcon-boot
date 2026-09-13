# airies-boot

A single-purpose, auditable multi-boot loader for UEFI x64. A distilled Ventoy:
drag-and-drop ISO booting plus a personal LUKS-backed OS booted via a UKI.

## Read this first

**`docs/DESIGN.md` is the authoritative design and the source of truth.** Read it
before proposing or changing any architecture. It argues its decisions and
records the alternatives that were considered and rejected, so do not reintroduce
a rejected approach without re-reading why it was rejected. `README.md` is the
short public summary of the same model.

Status: pre-1.0 MVP. Steps 1-3 of the build order in `docs/DESIGN.md` Section
11 are done: the mingw PE toolchain and QEMU/OVMF rig are proven, the loader
enumerates `*.iso` from exFAT (own read-only driver) into a working text menu,
and selecting an entry boots it (virtual CD, `ConnectController`, then the
platform's verifying `LoadImage` of the ISO's `\EFI\BOOT\BOOTX64.EFI`),
proven end to end under OVMF with Secure Boot off and surviving a payload
exit back into the menu. Next is step 4 (SB-state UX plus the injection
framework: family detection, descriptor registry, extent map, cpio append).
Still outstanding: the shim binary to ship and the Section 7.6 load-path
classification that goes with it, which gates the Secure Boot ON half of
step 3.

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
- `src/menu/` text menu over Simple Text Output/Input.
- `src/lib/` freestanding support routines: `mem*` and the UCS-2 helpers.
- `src/cli/` host tooling (Linux userland C), arriving at step 7 of the build
  order. May shell out to `cryptsetup`, `sbsign`, `mkfs.*`, `ukify`. The `sign`
  tool is a portable static binary shared between the host and the on-stick
  signing appliance.
- `docs/` DESIGN.md (authoritative) and later Manual.md.
- `tools/` host scripts: the QEMU + OVMF rig (`run-qemu.sh`) and the exFAT test
  fixture builder (`mk-test-exfat.sh`). OVMF (plain and secboot) is present on
  this machine.

`make check-tools` verifies the toolchain. Objects land in
`build/<variant>/<module>/`, the linked loader in `build/bin/airies-boot.efi`,
the staged ESP in `build/esp/`; `compile_commands.json` is regenerated at the
repo root by the default target.
