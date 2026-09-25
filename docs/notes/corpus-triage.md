# Corpus non-boot triage (2026-09-25)

The exit-goal bar: every corpus ISO boots in the configurations it can, and the
only allowed non-boot is a technical limitation - Secure Boot refusing an
unsigned image in the mode being tested, or the ISO's architecture not matching
the machine. This note sorts the remaining `drop`-route non-boots into: fixed,
real-and-fixable, hard (its own build), technical limitation, and measurement
limit (the medium boots, the harness cannot see it). Verdicts are read off the
saved screenshots and serial, not the scorecard's coarse "no userland".

## Fixed this session

- **windows-vista-sp1-x64 (BIOS):** ISO 9660-only Windows media. boot.wim is
  relocated past the ISO's end and served by a virtual-CD redirect; see
  windows-route.md Section 7.2. PASS.
- **windows-vista-x86, windows-7-sp1-x86 (bios32, Pentium III):** the CPU check
  wrongly demanded NX and SSE2 of Vista and 7, which need neither; gated to
  Windows 8+ by winpeshl's PE OS version. Both PASS on a real P3.

## Under investigation

- **netbsd-10.2-i386 (BIOS):** disassembly settled that cdboot's first stage
  works - it relocates itself, reads through the resident's INT 13h AH=42h
  (same as OpenBSD's cdboot), finds the PVD, loads /boot into 0x10000 and
  far-calls it; its "Can't read CD"/"Can't find /boot" errors never fire. The
  crash is downstream, inside NetBSD's /boot secondary bootstrap (its kernel
  load above 1 MB, or its reading of the carved E820), not in falcon-bios's
  CD-read path. Not the AH=02h issue first suspected. Settling it needs a rig
  run with fast (~0.5 s) screendumps right after the chain to catch /boot's
  own output.
- **haiku-r1beta6-x86 (BIOS):** its El Torito BIOS entry is a 2.88 MB floppy
  emulation (the UEFI entry is separate), so it takes the vfd route (a floppy
  served as a memory disk, not the CD chainload). It returns to the menu.
  Suspect: vfd.c serves the floppy but not the virtual CD alongside, and
  Haiku's loader on that floppy reads the Haiku system off the CD's ISO9660,
  which is then not there. memtest (also a floppy image) passes because it is
  self-contained. Needs a rig run to see Haiku's loader output and confirm
  whether serving the CD alongside the floppy is the fix.
- **pfsense-plus-24.06b7 (BIOS):** amd64, 1369 MiB, fits the amd64 staging
  ceiling - the recent serial shows falcon-bios does serve it as a memory disk
  and hand off, so it is past staging. The failure is downstream in pfSense's
  own loader/installer (no serial "Welcome to pfSense!" within 300s), distinct
  from the i386 BTX limit. Needs a fresh rig run to characterise; the saved
  screenshot is from an older loader.

## Technical limitation - the medium cannot boot this way

- **freedos-1.4-live (BIOS):** confirmed a limitation, not a loader bug. Its
  CDROM.BAT tries only hardware ATAPI/AHCI/IDE drivers (AHCICD, VIDE-CDD,
  OAKCDROM, GSCDROM, UDVD2) - none a BIOS/INT 13h El Torito driver. The virtual
  CD is an INT 13h construct (drive 0xE0) with no ATAPI hardware behind it, so
  UDVD2 assigns D: but its first real read fails. Everything before userland
  loads fine through 0xE0, and the resident cannot even return "drive not
  ready" (AH=0xAA), so the error is FreeDOS's, not falcon-bios's. The ISO ships
  no driver a drag-and-drop virtual CD can serve.
- **freebsd-disc1-14.5-i386 (BIOS):** confirmed an i386/BTX limitation. The ISO
  is 1044 MiB; FreeBSD's i386 loader runs under BTX, which maps only the low
  ~1 GB, and copies the whole ISO into RAM as the md_image, running off the end
  of that window - "BTX halted" is BTX's exception handler. The amd64 FreeBSD
  stages below 4 GB and passes. A ~1 GB i386 memory disk cannot fit; a smaller
  i386 BSD ISO would boot. falcon-bios now gates staging_fits by the loader's
  ELF machine (i386 -> 1 GB ceiling, amd64 -> 4 GB), so this ISO is refused at
  "checking memory for the image" with that reason rather than served and left
  to halt BTX.
- **opnsense-25.1 (BIOS):** amd64, 2.2 GB, correctly refused by staging_fits as
  too big even for the amd64 ceiling (its EFI failure is a separate shim/PE
  matter). Not the i386 case. (pfsense-plus, also amd64 but 1.4 GB, fits the
  ceiling and is under investigation above, not here.)

## Hard - its own build (NT text-mode / driver injection)

- **windows-2000-pro-x86, windows-xp-pro-sp3-x86, windows-xp-pro-x64 (BIOS):**
  text-mode setup, bugcheck 0x7B. See windows-route.md Section 7.4 - a
  firadisk-shaped multi-session build. Piece 1 (an XP-loadable driver) is
  settled: the existing falcon-vdisk-x86.sys imports only NT 5.0+ symbols.
- **reactos-0.4.16 (BIOS and bios32):** bugcheck IO1_INITIALIZATION_FAILED
  (STOP 0x69), a boot driver failing to initialise - the same NT-kernel
  boot-device shape as XP/2000 (ReactOS is a Windows clone). The XP text-mode
  route, when built, is the likely fix here too.

## Measurement limit - the medium boots

- **openbsd-7.9-i386 (BIOS):** reaches "Welcome to the OpenBSD/i386 7.9
  installation program. (I)nstall, (U)pgrade, (A)utoinstall or (S)hell?" on the
  VGA console (screenshot). It boots. The harness scores it FAIL only because
  OpenBSD's installer speaks to VGA, not the serial console the harness reads,
  and screen_kind cannot read the text off a console screen. This is an allowed
  non-verdict, not a boot failure. Reading the console out of VGA text memory
  (pmemsave 0xB8000) was tried in run-case.sh and reverted: the harness QEMU's
  HMP would not write the file, though its screendump over the same monitor
  worked - so the console text is only in the rendered PPM, which would need
  OCR. Left as a documented measurement limit; an ISO served with the loader
  forcing com0, or an OCR pass over the screendump, would score it PASS.

## Not yet inspected here

The Secure-Boot `own keys` REFUSED cells across the Linux media are the intended
technical-limitation outcome (an ISO whose shim is signed by Microsoft, not by
the stick owner's key, cannot run under Secure Boot with only the owner's key
enrolled). The `chain`-route and `bless`-route FAILs, and haiku, are not triaged
in this pass.
