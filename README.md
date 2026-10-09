# falcon-boot

A single-purpose, auditable multi-boot loader for x86-64 UEFI, with an
opt-in BIOS loader alongside that is i586 code and runs on 32-bit machines
too. A distilled Ventoy.

- **Drag-and-drop ISO booting.** Copy an `.iso` onto the stick's exFAT
  partition from any OS; it appears in the boot menu and boots. Linux ISOs
  boot their own kernel with a small hook that maps the ISO back into place
  for the distro to find; Windows install ISOs boot stock, patched in memory
  as they are served; FreeBSD boots from a memory disk of its own ISO.
- **Personal encrypted OS.** Boots a Unified Kernel Image off a LUKS2+ext4
  partition; its initramfs unlocks the real root and home and pivots in,
  from one passphrase prompt.
- **Secure Boot kept.** Everything the loader launches goes through shim's
  verification: with Secure Boot on, only what verifies against db or the
  enrolled MOK boots, and media is blessed (signed with your own key, on a
  host or on the stick's own signing appliance) to boot that way; with it
  off, anything boots. The loader never signs and never relaxes enforcement.
  `docs/DESIGN.md` Section 7 is the model.
- **A machine's own boot loader.** The same loader installed on a disk
  (`falcon-boot-mkstick station install`, DESIGN Section 12), with a boot
  list, chains to other systems, and on UEFI a TPM-backed TOTP code shown
  before the passphrase prompt.
- **falcon jump.** A boot floppy for a BIOS machine that cannot boot a USB
  stick or a PC Card (DESIGN Section 13): its own ATA, USB and PC Card
  drivers bring the stick up and hand over to it.

The UEFI loader is x86-64 only, with no 32-bit UEFI. The BIOS loader is
opt-in (`make bios`, `falcon-boot-mkstick bios`): a BIOS machine has no
Secure Boot, so on that route nothing is verified and nothing is blessed.

`docs/DESIGN.md` is the design, present tense, Section 11 drawing every boot
route of both loaders. `docs/Manual.md` is the operator's reference: the
menu, the route codes, Secure Boot states and enrollment, blessing, the
station and its `STATION.INI`, jump, every `falcon-boot-mkstick` command,
troubleshooting by failure page, and building and testing.

## Layout

```
apps/              the loaders: stick/, station/, station-legacy/, jump/
falcon-core/       firmware-free libraries every other component links:
                   crypto, TPM, the exFAT, ext4, FAT, ISO9660 and UDF drivers,
                   LUKS2, partition tables, the PE walker, boot predictions,
                   media logic, WIM, the manifest, the screen layout
falcon-efi/        the UEFI loader (x64 PE32+) and its QEMU/OVMF rigs
falcon-bios/       the BIOS loader (three stages into i586 protected mode),
                   jump's drivers, and the SeaBIOS rigs
falcon-cli/        host tools: falcon-boot-mkstick, falcon-boot-sign
falcon-appliance/  the on-stick signing appliance: PID 1 and its image recipe
falcon-hook/       the initrd program that maps an ISO for a Linux distro
falcon-winjump/    the WinPE program that attaches a Windows ISO for Setup
falcon-vdisk/      the WinPE virtual-CD driver for Windows older than 8
docs/              DESIGN.md, Manual.md, coverage.md (the corpus scorecard)
tools/             the static-analysis gate, the processor-floor check,
                   and coverage/, the harness that produces docs/coverage.md
```

Each component has `src/`, `tests/` and `tools/` as it needs them and a
`build.mk` the root Makefile includes; it is one build, not nine.

## Releases

Each release on GitLab carries mkstick and the sign tool as static x86-64
binaries with every unsigned loader they install
(`falcon-boot-X.Y.Z-x86_64.tar.gz`, unpacked under `/usr` it needs no path
options), the BIOS station bundle, the falcon jump floppy image, and their
`SHA256SUMS`. The Manual's Section 14 says how to check and install them.

## Building

Requires `x86_64-w64-mingw32-gcc`. The BIOS loader and falcon-hook's 32-bit
build need the host gcc's 32-bit libgcc (`gcc-multilib` on Debian) and
`i686-w64-mingw32-gcc` for the 32-bit winjump; the host tools need
`cryptsetup`, `sbsign` and `mkfs.exfat`/`mkfs.ext4`; the rigs need QEMU with
OVMF and SeaBIOS, and the floppy needs mtools. `make check-tools` names what
is missing. A machine without the toolchain builds in the harness container
instead: `tools/coverage/harness.sh make` (docker; everything is in the
container).

```sh
make check-tools     # what is missing, and what each tool is for
make                 # the UEFI loader, host tools and winjump into build/bin/
make test-fixtures   # the images the image-backed tests read (needs sudo)
make test            # every component's host tests
make coverage        # the tests, strictly, with one coverage report
make analyze         # cppcheck, gcc -fanalyzer and clang-tidy, gated
make bios            # the BIOS loader's stages
make station         # the stationary build, UEFI and BIOS
make jump            # the boot floppy, build/bin/falcon-jump.img
make run             # the test stick under QEMU/OVMF with a display window
```

The rigs (`make test-stick`, `test-core`, `test-station*`, `test-tpm-rig`,
`test-jump`, `test-bios-img` and the rest) are listed in the Manual's
Section 13. Everything, the corpus included, runs in a container with
`tools/coverage/harness.sh` (`harness.sh make test`), which is how the
measurements in `docs/coverage.md` are made.

The pipeline (`.gitlab-ci.yml`) runs on the binhost's `falcon` runner, each
job in the same container: every merge request gets the tests (with JUnit
reports), `make analyze`, the coverage gate and the build. A merge request
into `master`, which is a release branch shipping, also runs every rig and
the corpus under all six states, and fails if any scorecard cell that passes
on `master` no longer does (`tools/coverage/scorecard-diff.py`).

`make install` copies `falcon-boot-mkstick` and `falcon-boot-sign` into
`/usr/local/bin`, building them first; `PREFIX` and `DESTDIR` apply.
`make uninstall` removes them.

The personal OS this loader boots is a separate project:
[archusb](https://git.airies.net/vifair22/archusb) holds the Arch install's
build runbook, its configuration, and the OS-side half of the passphrase
handoff (DESIGN Section 5.1) as a mkinitcpio hook.
[thinkpad-t430](https://git.airies.net/vifair22/thinkpad-t430) holds the
same half for an installed Gentoo with systemd, as a dracut module.

## Licence

GPL-3.0-or-later. The full text is in `LICENSE`.
