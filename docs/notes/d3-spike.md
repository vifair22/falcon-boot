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

## D3b: the ISO's GRUB as shim's second stage, with a configuration we write

Not built. It is the route that reaches the distros D3a cannot: publish the
ISO's shim, its signed `grubx64.efi`, the kernel, our combined initrd and a
`grub.cfg` of ours on the synthetic filesystem; shim verifies GRUB, GRUB
verifies the kernel through shim's protocol, and our configuration names our
cmdline and initrd. Neither a configuration file nor an initrd is verified
under Secure Boot on any distro today, so nothing it does is outside what the
platform already permits, and it needs no ISO rewriting (the synthetic
filesystem is the loader's own).

It stays unbuilt because DESIGN 7.2 parked configuration injection into
another vendor's trust chain for discussion, and that discussion has not
happened. Two questions for it:

1. Is serving our own `grub.cfg` to a distro's signed GRUB acceptable, given
   that it is what GRUB's own design expects a configuration to be (unsigned,
   editable at the prompt), or is it the kind of relaxation Section 7.3 rules
   out? It changes what the kernel is told, never what may run.
2. Each distro's signed GRUB finds its configuration differently (an embedded
   prefix, a `search --file` for a marker, a memdisk). The loader would need
   one small table of where each looks, measured per distro, the same way the
   hook's nudges are.

Measured cost if agreed: one more credfs layout in `direct.c` and a GRUB
configuration writer in falcon-core (the inverse of `bootcfg.c`'s reader).
