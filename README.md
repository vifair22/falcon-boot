# falcon-boot

A single-purpose, auditable multi-boot loader for x86-64 UEFI, with an
opt-in BIOS loader alongside that also runs on 32-bit machines. A distilled
Ventoy.

- **Drag-and-drop ISO booting.** Copy an `.iso` onto the stick's exFAT
  partition from any OS; it appears in the boot menu and boots. No registration
  step. Linux ISOs boot their own kernel with a small hook that maps the ISO
  back into place for the distro to find; Windows install ISOs boot stock,
  patched in memory as they are served; FreeBSD boots from a memory disk of
  its own ISO.
- **Personal encrypted OS.** Boots a Unified Kernel Image off a LUKS2+ext4
  partition; its initramfs unlocks the real root/home and pivots in, all from a
  single passphrase prompt.
- **Secure Boot upheld.** Chains through Fedora's MS-signed shim and never
  weakens the machine's Secure Boot posture. See the security note below.

Scope is deliberately narrow: the UEFI loader is x86-64 only, with no
32-bit UEFI. The BIOS loader is opt-in (`make bios`, `falcon-boot-mkstick
bios`): a BIOS machine has no Secure Boot, so on that route nothing is
verified and nothing needs blessing. It is i586 code and runs on any
processor from the Pentium on, booting 32-bit and 64-bit kernels, and
takes what a newer processor adds (SSE, AES-NI) by asking CPUID at run
time.

## Security note

falcon-boot never weakens the machine's Secure Boot posture. It behaves
according to the firmware's Secure Boot state:

- **Secure Boot ON:** it upholds it. Everything is loaded through the normal
  verifying path (via shim); only payloads that verify against the trust chain
  boot, and an unsigned kernel is refused, exactly as if booted directly. It
  does not install any override or disable verification.
- **Secure Boot OFF:** there is nothing to uphold, so any ISO boots.

Under Secure Boot ON, on a machine whose db trusts Microsoft, distro ISOs
that ship their own MS-signed shim boot fully verified through that shim or
through their own signed GRUB, with the hook appended so the live system
finds its media; stock Windows ISOs boot too. The route that works on any
machine is blessing media with your own key: it boots under full Secure Boot with media-finding included, either from
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
falcon-core/       firmware-free libraries all the others link: crypto, the
                   exFAT, ext4, FAT, ISO9660 and UDF drivers, LUKS2, GPT, the
                   PE walker, boot predictions, WIM, the manifest, media logic
falcon-efi/        the UEFI loader (x64 PE32+) and its QEMU/OVMF rig
falcon-bios/       the BIOS loader: a three-stage MBR chain into i586 protected mode
falcon-cli/        host tools: falcon-boot-mkstick, falcon-boot-sign
falcon-appliance/  the on-stick signing appliance: PID 1 and its image recipe
falcon-winjump/    the WinPE program that attaches a Windows ISO for Setup
falcon-hook/       the initrd program that maps an ISO for a Linux distro
docs/              DESIGN.md (the design; Section 12 draws every boot route),
                   Manual.md (the operator's reference), notes/
```

Each component has `src/`, `tests/` and `tools/` as it needs them, and a
`build.mk` the root Makefile includes; it is one build, not seven.

## Building

Requires `x86_64-w64-mingw32-gcc`, and for testing `qemu-system-x86_64` with
OVMF. Host tooling additionally needs `cryptsetup`, `sbsign`, `mkfs.exfat`,
`mkfs.ext4`. `make bios` and falcon-hook's 32-bit build need the host gcc's
32-bit libgcc (`gcc-multilib` on Debian), and the BIOS rig needs SeaBIOS.

```sh
make check-tools     # verify the toolchain is present
make                 # the loader, host tools and winjump into build/bin/
make test            # every component's host tests
make test-fixtures   # the images the image-backed tests read (needs sudo)
make coverage        # the tests, strictly, with one coverage report
make test-image      # build the exFAT fixture the rig boots against
make run             # boot it under QEMU/OVMF with a display window
make bios            # the BIOS loader's stages
make test-bios-img   # a disk the rig boots with run-qemu.sh --bios
make station         # the stationary build, UEFI and BIOS (DESIGN 13)
make jump            # the boot floppy, build/bin/falcon-jump.img (DESIGN 14)
make test-jump       # every jump route under QEMU (CF needs build-qemu.sh)
```

`make install` copies the two host tools, `falcon-boot-mkstick` and
`falcon-boot-sign`, into `/usr/local/bin` (mode 0755), building them first;
`PREFIX` changes the prefix and `DESTDIR` stages the install elsewhere.
`make uninstall` removes them.

See `docs/DESIGN.md` for architecture and the MVP build order.

The personal OS this loader boots is a separate project:
[archusb](https://git.airies.net/vifair22/archusb) holds the Arch install's
build runbook, its installed configuration, and the OS-side half of the
passphrase handoff described in DESIGN Section 5.1.

## Licence

GPL-3.0-or-later. The full text is in `LICENSE`.
