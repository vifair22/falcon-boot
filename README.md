# falcon-boot

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

falcon-boot never weakens the machine's Secure Boot posture. It behaves
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
src/efi/      EFI entry point, .sbat, our own minimal UEFI headers
src/loader/   shim-verified load path, synthetic credential filesystem
src/console/  text menu and masked passphrase prompt
src/disk/     GPT parsing and the one device-discovery pass
src/fs/       filesystem registry and read-only exFAT / ext4 drivers
src/luks/     LUKS2 unlock
src/crypto/   SHA-2, HMAC, PBKDF2, AES-XTS
src/secboot/  Secure Boot state, read for display only
src/iso/      ISO path: virtual media, family detection, injection
src/pos/      personal-OS path
src/bless/    blessed path and its manifest
src/lib/      freestanding support routines (mem*, UCS-2 helpers)
src/cli/      host tools (mkstick, sign); never linked into the loader
src/winjump/  WinPE jump binary
tools/        host scripts: QEMU + OVMF rig, test-fixture builders
docs/         DESIGN.md and (later) Manual.md
```

## Building

Requires `x86_64-w64-mingw32-gcc`, and for testing `qemu-system-x86_64` with
OVMF. Host tooling additionally needs `cryptsetup`, `sbsign`, `mkfs.exfat`,
`mkfs.ext4`.

```sh
make check-tools     # verify the toolchain is present
make                 # build build/bin/falcon-boot.efi (+ compile_commands.json)
make test-image      # build the exFAT fixture the rig boots against
make run             # boot it under QEMU/OVMF with a display window
```

See `docs/DESIGN.md` for architecture and the MVP build order.

The personal OS this loader boots is a separate project:
[archusb](https://git.airies.net/vifair22/archusb) holds the Arch install's
build runbook, its installed configuration, and the OS-side half of the
passphrase handoff described in DESIGN Section 5.1.
