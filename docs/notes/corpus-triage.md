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

- **netbsd-10.2-i386 (BIOS):** an open NetBSD-/boot failure, not a falcon-bios
  CD-path bug. Freezing the crash (`-no-reboot`) shows exactly cdboot's banner
  "NetBSD/x86 cd9660 Primary Bootstrap" and nothing more: cdboot runs fully
  (reads through the resident's INT 13h AH=42h, loads /BOOT - lba 340030,
  86444 bytes, its >64 KB load handled by the resident's segment advance - and
  far-calls it at 0x10000), and NetBSD's /boot then resets the machine at its
  entry before printing anything. Ruled out: a CPU-feature issue (identical
  crash under -cpu host and -cpu qemu64) and the CD path (cdboot works; OpenBSD
  and Linux-32 boot fine in the same setup). The pre-reset fault could not be
  read - KVM logs only a plain reset, and a TCG run with `-d int` is too slow
  to reach it in a bounded pass. Settling it (not done): a TCG run carried to
  the fault to read the exception vector and /boot's crashing EIP, or an ISO
  remastered with `consdev com0` so /boot's own crash message reaches serial.
  No falcon-bios fix identified; not proven a hard limitation either.
- **haiku-r1beta6-x86 (BIOS):** an open downstream Haiku-loader failure, not a
  falcon-bios serving bug. Its El Torito BIOS entry is a 2.88 MB floppy
  emulation, so it takes the vfd route, and the serial confirms falcon-bios
  serves it correctly ("serving a 2880 KiB floppy image on drive 0x00",
  "booting the medium's floppy image") - the same route memtest boots through.
  Haiku's loader then runs (a frozen `-no-reboot` capture shows it has cleared
  the screen to black, so it got past the floppy load into its own code) and
  resets. Like NetBSD's /boot, the crash is in the medium's own loader after
  falcon-bios hands off. Whether Haiku's loader is failing to find the Haiku
  BFS volume (it would have to scan the physical disks, since vfd serves only
  the floppy, not the CD) or crashing for another reason is unsettled; a frozen
  TCG run to read the fault would be the next step, as for NetBSD.
- **pfsense-plus-24.06b7 (BIOS):** a downstream failure past falcon-bios, like
  netbsd and haiku. amd64, 1369 MiB, fits the amd64 staging ceiling. The serial
  shows it all the way through pfSense's own loader menu ("Welcome to Netgate
  pfSense Plus", console set to Video), its autoboot, and "Loading kernel...
  /boot/kernel/kernel text=..." - so falcon-bios serves the memory disk and
  pfSense's loader loads the kernel. Then the machine resets (a live VGA
  screendump shows the falcon menu again). pfSense's kernel resets after load
  where freebsd-bootonly-15.1 (amd64, 546 MiB) boots to its installer through
  the same route, so the suspect is the larger (1.4 GB) memory disk served over
  INT 15h - possibly a falcon-bios limit for memory disks past ~1 GB, or a
  pfSense-kernel issue - but it is not a serving/staging failure. Left as an
  open downstream reset pending a frozen-fault run to read the cause.

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
