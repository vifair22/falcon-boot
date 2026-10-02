# falcon-boot

A single-purpose, auditable multi-boot loader for x86-64 UEFI, with an opt-in
BIOS loader that is i586 and so runs on 32-bit machines too. A distilled Ventoy: drag-and-drop ISO booting plus a personal
LUKS-backed OS booted via a UKI.

## Read this first

**`docs/DESIGN.md` is the design and the source of truth.** Read it before
proposing or changing any architecture. It describes how falcon-boot works,
in the present tense; it is not a decision log and records no history (git
does). `README.md` is the short public summary of the same model.

DESIGN Section 11 draws every boot route of both loaders, traced from the
code, from power-on to the booted system standing on its own. A change to a
route (its order, a fallthrough, what it hands over, what stays resident)
updates its diagram in the same commit. Section 11 is where a route's shape
is looked up before its code is read.

What exists, proven in the QEMU/OVMF and SeaBIOS rigs and on real hardware:
the stick's UEFI and BIOS loaders with every route (the personal OS with its
passphrase handoff, the virtual CD, direct kernel boot with falcon-hook, D3a
and D3b under Secure Boot, blessed media, the Windows boot.wim patch with
winjump and falcon-vdisk, the BSD memory disk); mkstick, the sign tool and
the on-stick signing appliance; the UEFI and BIOS stations with the
installer, STATION.INI, the station bundle update and the TPM-backed TOTP
check; falcon jump and station-legacy for machines without a 64-bit mode;
and the coverage harness (`docs/coverage.md`), which runs in a container on
binhost-builder. Status: pre-1.0.

## Non-negotiable security invariants (DESIGN.md Section 7)

A change that breaks one of these is a regression: stop and flag it.

1. **The loader verifies; it never signs.** No signing operation and no
   private key in the loader. Signing happens in Linux userland only (a host,
   or the on-stick appliance). Section 7.5.
2. **Enforcement is the platform's.** Everything is launched through the
   shim-verified load path (Section 7.6). The loader reads Secure Boot state
   for UX only.
3. **Nothing in the loader relaxes signature enforcement.** The opt-outs are
   the platform's: firmware Secure Boot off, or shim's `MokSBState`.
   Sections 7.3 and 7.4.
4. **The private MOK key lives encrypted on p3.** Never on p2, never in the
   loader. Section 7.5.

An unsigned ISO with Secure Boot validation on is therefore blessed or not
booted; there is no third path to add.

## Layout

The repository is the loaders in `apps/` and the components they and the
host programs are built from, each with its own `src/`, `tests/` and
`tools/`. One non-recursive Makefile: the root holds the toolchains, flags,
test runner and compile database and `include`s a `build.mk` per component
and an `app.mk` per app. A translation unit sees only its own component's
`src/` and the ones it builds against (the UEFI loader efi+core, falcon-bios
bios+core, the appliance appliance+cli+core); a module directory name is
unique within each such set, so `#include <fs/exfat.h>` means one file, and
`tools/analyze.sh` fails on a name that appears twice.

- `apps/`: `stick/` (every route; `all` builds the UEFI image, `make bios`
  the BIOS one), `station/` (DESIGN Section 12; `make station`),
  `station-legacy/` (12.10; `make station-legacy`, `make devlink-uki`,
  `make station-bundle`), `jump/` (Section 13; `make jump`). An app is its
  entry points (`efi_main.c`, `efi_sbat.c`, `bios_main.c`) and an `app.mk`
  listing what it links, one line per module with its reason. What an app
  does not list, it cannot run.
- `falcon-core/`: the EFI-free libraries every component links. Device
  access goes through a read callback and every buffer is the caller's,
  which is what lets the loaders, the host tools and the appliance run one
  implementation and the host tests exercise it. Modules: `crypto`, `tpm`,
  `fs` (the registry and the exfat, ext4+jbd2, fat, iso9660 and udf drivers),
  `luks`, `part`, `ident`, `pe`, `trust`, `media`, `gz`, `wim`, `manifest`,
  `tui`, `blk`, `lib`. DESIGN Section 9 is the inventory. `tests/` is the
  suite, `tests/support/` what every component's tests link, `tools/` the
  fixture builders and the TPM rig.
- `falcon-efi/`: the UEFI loader. Freestanding C11, MS x64 ABI, built to
  PE32+ with `x86_64-w64-mingw32-gcc`, own headers in `efi/`; no gnu-efi, no
  EDK2. Modules per DESIGN Section 9. `tools/`: the QEMU and OVMF rig
  (`run-qemu.sh` with `--secboot`, `--stick`, `--headless`, `--bios`;
  `ovmf.sh`; `rig-shot.sh`), its fixtures (the payload ISO, the test MOK,
  the sd-stub fixture UKI, the test stick, `mk-test-station.sh`), and the
  rigs `run-core-rig.sh`, `run-station-rig.sh`, `run-installed-rig.sh`,
  `run-tpm-rig.sh` (with `swtpm.sh` and `tpm-probe/`).
- `falcon-bios/`: the BIOS loader, i586 code in flat 32-bit protected mode
  (DESIGN 3.3 for the layout, 11.3 for the routes, 12 for the station, 13
  for jump). `src/` is grouped by job: `start/` (the MBR, stage 2, the mode
  switch, the thunk, the IDT, and `biosdefs.h`, where every address is
  written), `console/`, `iso/`, `vfd/`, `mem/`, `disk/`, `linux/`, `pos/`,
  `bsd/`, `win/`, `mbr/`, `task/`, `power/`, `jump/`, `station/`, and `hw/`
  (jump's modules: `ata/`, `usb/`, `pcc/`, `scsi/`, `blk13.c`). Every piece
  with no BIOS in it (`vcdcore`, `linuxcore`, `e820`, `vfdcore`, `vbrcore`,
  `cdclock`, `atacore`, `usbcore`, `pcccore`) is host-tested in `make test`.
  `tools/`: the SeaBIOS rigs (`run-jump-rig.sh`, `run-station-rig.sh`,
  `run-station-fixture-rig.sh`, `run-legacy-rig.sh`, `run-cfstation-rig.sh`,
  `run-devlink-rig.sh`, with `rigboot.S`, `tickstop.S`, `memprobe.S`), the
  fixture disks, and `qemu-pcc/`, the PC Card bridge model the rig's QEMU is
  built with.
- `falcon-cli/`: host tooling, Linux userland C never linked into a loader:
  `mkstick`, `sign` (also linked static for the appliance), `common`.
- `falcon-appliance/`: the on-stick signing appliance. `src/` is PID 1
  (`appliance.h` has the map), `image/` the UKI recipe (`mk-appliance.sh`,
  `programs.list`, `modules.list`, each entry with its reason), `tools/` the
  QEMU proofs.
- `falcon-hook/`: the program the loaders append to an ISO's initrd (DESIGN
  6.4), x86-64 and i586 from one source, embedded as generated C arrays.
- `falcon-winjump/`, `falcon-vdisk/`: the WinPE jump binary and the
  virtual-CD NT driver (DESIGN 6.3), x64 and i686.
- `docs/`: `DESIGN.md`, `Manual.md`, `coverage.md` (generated by the
  harness). `tools/` at the root: `analyze.sh` with `analyze.md` and
  `analyze-baseline.txt`, `check-floor.sh`, `arch-iso.sh`, and `coverage/`,
  the harness.

## Building and testing

- `make check-tools` names what is missing. Objects land in
  `build/obj/<flavour>/<source path>.o`, binaries in `build/bin/` (the UEFI
  loader is `build/bin/falcon-boot.efi`), fixtures and rig artifacts under
  `build/`. `compile_commands.json` is regenerated by the default target, or
  on its own by `make compdb` (no toolchain needed).
- `make test` runs every component's suite. A fresh build directory needs
  `make test-fixtures` first (twelve image-backed tests read them; it calls
  `losetup`, so sudo). `make coverage` runs the suites strictly with a
  report; `make analyze` runs cppcheck, gcc's analyzer and clang-tidy
  against `tools/analyze-baseline.txt`.
- Rigs: `make test-stick`, `run-stick`, `run-stick-secboot`, `test-core`,
  `test-station`, `test-station-uefi`, `test-station-installed`,
  `test-tpm-rig`, `test-tpm`, `test-bios-img`, `test-bios-probe`,
  `test-jump`, `test-station-bios`, `test-station-fixtures`,
  `test-station-legacy`, `test-devlink`.
- A machine without the toolchain builds in the harness container, which
  carries all of it: `tools/coverage/harness.sh make test` (docker; `/dev/kvm`
  for the rigs; the repo's own `build/` when the binhost's `/srv/falcon` is
  not there; output chowned back to the caller). That is how the T430 builds.
  Heavy runs and the corpus go to binhost-builder: ship the tree with
  `git archive HEAD | ssh vifair@172.20.0.170 'mkdir -p /srv/falcon/repo-<sha>
  && tar -x -C /srv/falcon/repo-<sha>'`, then there
  `tools/coverage/harness.sh --build-dir /srv/falcon/build-<sha> make test`
  (or `make analyze`, or no command for the corpus). A fresh build
  directory needs `make test-fixtures`, the shim binaries from
  `/srv/falcon/build/shim/bins` and the DevLink UKI from
  `/srv/falcon/devlink-recovery.efi`; `/srv/falcon/build` is the corpus's
  own, and its case sticks are mounted from `/srv/build/cache/falcon/sticks`.
  Delete a `build-<x>` and `repo-<x>` pair once its branch has landed.
- `release_version` at the root is the version; everything tags off it
  (`FALCON_SEMVER` and the timestamped `FALCON_VERSION` in
  `build/version.h`, the SBAT rows, the banners).

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
output, docs, code comments, and the assistant's own replies in a session.

State the fact. Stop.

Each kind of writing has one job:

- `docs/DESIGN.md` says how it works. A rejected alternative is one sentence,
  what and why, not a re-argued case. A security invariant is stated once, in
  Section 7, and referenced by number everywhere else.
- `docs/Manual.md` says how to use it.
- The mermaid diagrams (DESIGN Section 11) show it.
- A code comment explains what the code does not make obvious: an invariant
  the code relies on, a hardware or format quirk, why the obvious approach
  was not taken. Design and policy live in DESIGN.md; point at the section
  (`DESIGN Section 7.6`), do not restate it. Any dev can read what the code
  does.

Cut:

- Restating a security invariant at the point of use. "This changes nothing
  about what may run", "the loader cannot and must not do that itself".
- Reassurance. "Nothing here decides what may run", "this is safe".
- Consequences the reader already knows. "Anything unsaved is lost" before a
  reboot.
- Justifying the design to the reader. Confidence needs no defence.

Keep:

- What is about to happen.
- What the user must do.
- What went wrong, and what to do about it.

A confirmation is a question and its keys, not an essay:

    Reboot into firmware setup? y to reboot, any other key to go back.
