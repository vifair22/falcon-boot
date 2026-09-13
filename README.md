# airies-boot

A single-purpose, auditable multi-boot loader for UEFI x64. A distilled Ventoy.

- **Drag-and-drop ISO booting.** Copy an `.iso` onto the stick's exFAT
  partition from any OS; it appears in the boot menu and boots. No registration
  step.
- **Personal encrypted OS.** Boots a Unified Kernel Image off a LUKS2+ext4
  partition; its initramfs unlocks the real root/home and pivots in, all from a
  single passphrase prompt.
- **Secure Boot upheld.** Chains through Fedora's MS-signed shim and never
  weakens the machine's Secure Boot posture. See the security note below.

Scope is deliberately narrow: UEFI x64 only. No BIOS and no 32-bit UEFI,
ever.

## Security note

airies-boot never weakens the machine's Secure Boot posture. It behaves
according to the firmware's Secure Boot state:

- **Secure Boot ON:** it upholds it. Everything is chainloaded through the normal
  verifying path (via shim); only payloads that verify against the trust chain
  boot, and unsigned media is refused, exactly as if booted directly. It does not
  install any override or disable verification.
- **Secure Boot OFF:** there is nothing to uphold, so any ISO boots.

Under Secure Boot ON, distro ISOs that ship their own MS-signed shim (Ubuntu,
Fedora, Debian, recent Arch, ...) drag-and-drop and launch fully verified,
though whether the live environment finds its media afterwards is up to that
distro's own initramfs. The reliable route is blessing media with your own
key: it boots under full Secure Boot with media-finding included, either from
an external host or from the stick's own minimal signing appliance, so the key
never leaves encrypted storage and never enters the loader. If you
want the classic "boot anything" experience, you opt in through the platform's
own physical-presence mechanism (`mokutil --disable-validation`, confirmed at the
MokManager screen) or by turning Secure Boot off; the loader itself never relaxes
enforcement and never holds a signing key. The personal-OS path is signed by us
and verified end to end.

Two invariants hold everywhere: the loader verifies but never signs, and every
relaxation of enforcement is a platform choice the user makes with physical
presence, never something baked into our binary. See `docs/DESIGN.md` Section 7
for the full model.

## Layout

```
src/efi/     EFI application glue plus our own minimal UEFI headers
src/exfat/   read-only exFAT driver
src/menu/    text menu over Simple Text Output/Input
src/lib/     freestanding support routines (mem*, UCS-2 helpers)
tools/       host scripts: QEMU + OVMF rig, test-fixture builders
docs/        DESIGN.md and (later) Manual.md
```

Host configuration tooling lands under `src/cli/` at step 7 of the build order.

## Building

Requires `x86_64-w64-mingw32-gcc`, and for testing `qemu-system-x86_64` with
OVMF. Host tooling additionally needs `cryptsetup`, `sbsign`, `mkfs.exfat`,
`mkfs.ext4`.

```sh
make check-tools     # verify the toolchain is present
make                 # build build/bin/airies-boot.efi (+ compile_commands.json)
make test-image      # build the exFAT fixture the rig boots against
make run             # boot it under QEMU/OVMF with a display window
```

See `docs/DESIGN.md` for architecture and the MVP build order.
