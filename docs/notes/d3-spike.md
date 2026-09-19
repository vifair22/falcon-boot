# D3 spike: key-less Secure Boot for distros with a Microsoft-signed shim (2026-09-18)

Roadmap M5 (`docs/notes/coverage-roadmap.md`, W2 D3). The question: on a
machine whose db trusts the Microsoft UEFI CA, can a stock distro ISO boot
with our cmdline and our initrd (falcon-hook) through the ISO's own
Microsoft-signed shim, so that it needs no blessing at all?

## D3a: the ISO's shim, with the kernel as its second stage

Built and measured. The loader reads the ISO's `/EFI/BOOT/BOOTX64.EFI` and
the entry's kernel out of the ISO, publishes both on the synthetic
filesystem of DESIGN Section 5.1 (`\shimx64.efi`, `\vmlinuz`), loads the
shim by device path through `load_ops` (so the platform verifies it against
db exactly as it would any image), and starts it with the load options
`\vmlinuz <hooked cmdline>`. The initrd, with falcon-hook and the extent map
in it, is served over LoadFile2 as for a Secure Boot off direct boot
(`falcon-efi/src/direct/direct.c`, `DIRECT_VIA_ISO_SHIM`).

What shim does with that, measured under OVMF with the Microsoft keys
enrolled (the harness's `ms` state):

- It takes the first word of its load options as the second stage, reads it
  off the device it was loaded from, verifies it against its vendor
  certificate and starts it. The kernel received the cmdline (falcon-hook
  ran, and the live root was found through its device) and took the initrd
  from LoadFile2. Whether shim strips its own first word before the kernel
  sees the options was not isolated; a leading `\vmlinuz` would be an
  unknown word the kernel passes to init, which is harmless either way.
- **It refuses a second stage without a `.sbat` section** ("Verification
  failed: (0x1A) Security Violation", then an attempt to start MokManager).
  SBAT metadata became mandatory for everything shim loads itself, under
  Secure Boot, in shim 15.7.

Which kernels carry `.sbat`, across the corpus: **Fedora's, and no other.**
Debian, Ubuntu (and Kubuntu, Mint), openSUSE, Arch, Manjaro, Gentoo, Alpine,
Proxmox, SystemRescue and Clonezilla kernels have none, because on those
distros GRUB is shim's second stage and checks the kernel through shim's
verify protocol, which does not look at SBAT.

Result: Fedora Workstation 44 boots, drag and drop, with Secure Boot on and
no blessing (`fedora-ws-44 ms dnd PASS`). The loader checks the kernel for
`.sbat` before taking this route and chainloads the ISO's own loader when it
is missing, so a distro the route cannot serve sees no shim error screen.
Where db does not trust the Microsoft CA (the `own` state) the ISO's shim is
refused, as it is when chainloaded; blessing is the route there.

Decision taken with the result: this is Enter's route under Secure Boot
(Section 6.4 menu keys unchanged: `c` chainloads, `d` forces direct).

## D3b: the ISO's own chain, with a grub.cfg of ours (2026-09-19)

Built and measured on the `spike/d3b` branch, then merged the same day:
the discussion DESIGN 7.2 asked for decided that serving our own
configuration to another vendor's signed GRUB is acceptable (DESIGN 7.2
records it). What it does and what it measured:

**The shape the write-up proposed does not work.** It had the loader publish
the ISO's shim, its signed GRUB and the rest on the synthetic filesystem.
GRUB reads files through its own drivers over Block I/O, not through Simple
File System, so from a handle that only has the latter it finds no disk and
no configuration. The shape that works serves the ISO itself as the virtual
CD, as a chainload does, with two overrides and no ISO record changed:

1. The bytes of the grub.cfg the ISO's default entry came from
   (`bootcfg_result.source`) are replaced in place by one of ours, exactly
   as long, padded with a comment (`falcon-core/src/media/grubcfg.c`). It
   names the entry's own kernel and initrds and the hooked cmdline.
2. falcon-hook's archive, with the extent map in it, is served past the
   ISO's end, and our grub.cfg names it as one more initrd with a GRUB
   blocklist: `initrd <the ISO's initrds> ($root)SECTOR+COUNT`.

The platform verifies the ISO's shim against db, shim verifies GRUB, GRUB
verifies the kernel through shim's protocol. The configuration and the
initrds are verified by nobody, on any distro, which is GRUB's design.
Nothing that runs is changed: only what the kernel is told, and what is in
its initramfs.

Proven first with no loader code: a copy of Debian live 13.7 with its
`/boot/grub/grub.cfg` overwritten in place and a hook-only cpio appended,
booted as a plain CD under Secure Boot with the Microsoft keys, ran
falcon-hook as rdinit (`Run /falcon/hook as init process`) and reached a
login prompt.

Then in the loader (`direct_grub_boot` in `falcon-efi/src/direct/direct.c`),
as Enter's route under Secure Boot when D3a cannot run the kernel (no
`.sbat`). Measured, harness `ms` state, drop route, stock unblessed ISOs:

| ISO | before | with D3b |
|---|---|---|
| debian-live-13.7 | FAIL | PASS (dhcp; auto-login console on screen) |
| debian-netinst-13.7 | FAIL | PASS (dhcp) |
| ubuntu-26.04.1 | FAIL | PASS (dhcp) |
| kubuntu-26.04 | FAIL | PASS (dhcp) |
| mint-22.3 | FAIL | PASS (dhcp) |
| opensuse-tw-20260916 | FAIL | PASS (dhcp) |
| proxmox-ve-9.2-1 | FAIL | PASS (dhcp) |
| clonezilla-3.3.3-15 | FAIL | boots to its own UI; the harness has no signal for it |
| kali-2026.2 | REFUSED | REFUSED (its kernel is not signed for its shim) |
| fedora-ws-44 | PASS (D3a) | PASS (D3a, unchanged) |

Arch, Manjaro, Gentoo, SystemRescue and Alpine carry no Microsoft-signed
shim, so no route through their own chain exists; blessing stays theirs.

Known limits:

1. The grub.cfg replaced is the file bootcfg read the default entry from.
   A distro whose signed GRUB reads a different file first (an embedded
   prefix pointing elsewhere) would ignore ours and boot as a chainload
   does; none in the corpus did.
2. The replacement must fit in the original's length. Every corpus
   grub.cfg is kilobytes; ours is a few hundred bytes plus the cmdline.
3. The coverage harness counts media read after a "starting" line as the
   kernel's. D3b's line says "handing ... to the ISO's own GRUB" instead,
   because GRUB reads the kernel and initrds after it; D3b cases are judged
   by DHCP, serial or the screen.

