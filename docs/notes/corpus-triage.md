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

## Real, and fixable (loader work, not yet done)

- **freedos-1.4-live (BIOS):** its CD driver (UDVD2/SHSUCDX) initialises drive
  D: off the virtual CD, then a later read fails, "Error reading from drive D:
  data area: drive not ready". Some INT 13h read pattern FreeDOS issues is not
  served correctly by the resident. A resident bug to chase, not a limitation.
- **netbsd-10.2-i386 (BIOS):** the loader tries both the direct and the chain
  route (serial shows DIR then CHN) and both return to the menu. NetBSD's i386
  boot blocks are not handled by either path. Real; needs the NetBSD boot
  mechanism understood. (OpenBSD i386, below, boots - so it is NetBSD-specific.)

## Real, but likely an i386/architecture limitation

- **freebsd-disc1-14.5-i386 (BIOS):** the FreeBSD loader runs, loads the kernel,
  references the FALCONBS.IMG memory disk, then "BTX halted" with a register
  dump. The amd64 FreeBSD (freebsd-bootonly-15.1) passes; i386 BTX is a
  constrained 32-bit environment and the memory disk is ~1 GB. Likely a BTX
  limitation on i386 rather than a loader bug, but not yet proven either way.
- **opnsense-25.1, pfsense-plus-24.06b7 (BIOS):** FreeBSD-derived, same memory-
  disk route as freebsd-i386. opnsense also fails the EFI memory check
  (0x8000000000000009) with Secure Boot off. Grouped with freebsd-i386.

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
  non-verdict, not a boot failure; a harness that OCR'd the console, or an ISO
  served with the loader forcing com0, would score it PASS.

## Not yet inspected here

The Secure-Boot `own keys` REFUSED cells across the Linux media are the intended
technical-limitation outcome (an ISO whose shim is signed by Microsoft, not by
the stick owner's key, cannot run under Secure Boot with only the owner's key
enrolled). The `chain`-route and `bless`-route FAILs, and haiku, are not triaged
in this pass.
