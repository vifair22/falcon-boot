# falcon-boot Manual

Operating reference for the loader on a written stick. `docs/DESIGN.md` is
where the reasoning lives; this file says what the loader does and what the
operator sees.

## 1. Route codes

A boot does not start the instant a row is chosen. The kernel, loader or
image being handed the machine can take several seconds to put anything on
screen, and in that time the screen still shows the loader's last message.
The route code says which boot path is running during that wait.

The code is three characters, drawn in white at the top right of the text
block, one row above the block's first line. Both loaders draw every screen
from one layout (`falcon-core/src/tui`), so it sits in the same place on
either. It appears as soon as the route is chosen and
before the handoff message, stays through any screen the route draws (the
passphrase prompt, an error), and is removed on return to the menu.

When a route falls through to another one, the code changes before the new
route's handoff. The cases are listed in Section 1.3.

### 1.1 UEFI loader

| code | route | reached by |
|---|---|---|
| `POS` | personal OS: a UKI from p3, with the passphrase credential beside it | the pinned row |
| `APP` | the signing appliance, a UKI from p3 | `b` on a row, `a` anywhere |
| `BLS` | a blessed wrap: the MOK-signed UKI built for a Linux ISO | Enter on a blessed row |
| `BLB` | a blessed BSD medium: its loader, MOK-signed, started with the medium as its device | Enter on a blessed BSD row |
| `EFI` | a bare `.efi` file on p2, launched as itself | Enter on a `.efi` row |
| `DIR` | direct kernel boot: the ISO's kernel and initrds with falcon-hook appended | Enter or `d` on a Linux ISO, Secure Boot not enforcing (off, validation disabled, setup mode) |
| `D3A` | the ISO's own shim, which loads the distro's signed kernel | Enter or `d` on a Linux ISO whose kernel carries `.sbat`, Secure Boot on |
| `D3B` | the ISO's own signed GRUB, with our grub.cfg served in place of its own | Enter or `d` on a Linux ISO whose kernel has no `.sbat`, Secure Boot on |
| `WIN` | Windows install media, boot.wim patched in memory as it is served | Enter or `d` on a Windows ISO |
| `BSD` | a BSD medium served as its own memory disk | Enter or `d` on a BSD ISO |
| `CHN` | chainload: the ISO's own bootloader off the virtual CD | `c` on an ISO, or a fallback |
| `IMG` | a `.img` served as a disk, and its own bootloader started | Enter on a `.img` row |

`c` and `d` on a blessed row bypass the blessing and boot the ISO as if
unblessed, so the code is one of `DIR`, `D3A`, `D3B`, `WIN`, `BSD` or `CHN`
rather than `BLS`.

### 1.2 BIOS loader

A BIOS machine has no Secure Boot, no shim and no blessing, so the BIOS
loader has five of the routes.

| code | route | reached by |
|---|---|---|
| `POS` | personal OS: a UKI from p3, unwrapped and its kernel started | the Personal OS row |
| `DIR` | direct kernel boot, falcon-hook of the kernel's width appended | Enter or `d` on a Linux ISO |
| `WIN` | Windows install media, boot.wim patched, the tail served from extended memory | Enter or `d` on a Windows ISO |
| `BSD` | a BSD medium served as its own memory disk | Enter or `d` on a BSD ISO |
| `CHN` | the ISO's own bootloader off the resident virtual CD | `c` on an ISO, or a fallback |

The BIOS loader mirrors its screen to serial (COM1) as the ANSI sequences
OVMF's terminal sends for the UEFI loader, so the code is on serial for
either loader, drawn where it is on the screen.

The BIOS loader is one image for every processor from the Pentium Pro on,
and boots 32-bit and 64-bit kernels alike. On a processor with no 64-bit
mode, a 64-bit kernel is refused before it starts, on `DIR` or `POS`:

    direct boot failed while starting the kernel

    the kernel is 64-bit and this processor has no 64-bit mode

(`personal OS boot failed while starting the kernel` on `POS`). A key
returns to the menu. A processor older than a Pentium Pro stops before the
menu, on screen and serial:

    falcon-bios: this processor is older than a Pentium Pro, which this loader needs
    press any key to reboot.

### 1.3 Fallthrough

| loader | shown first | then | when |
|---|---|---|---|
| UEFI | `D3A` | `D3B` | the ISO's kernel has no `.sbat`, so its shim would not run it; also no boot entry the loader can take, or no shim on the ISO |
| UEFI | `DIR` or `D3B` | `CHN` | no boot entry in the ISO's configuration the loader can take, or (`D3B`) its grub.cfg cannot be replaced in place |
| both | `WIN` | `CHN` | Windows media with nothing to patch |
| UEFI | `BSD` | `CHN` | the medium is not one the memory-disk route applies to |
| BIOS | `BSD` | `DIR` | as above |

On the BIOS loader, `DIR` with no boot entry it can take stops with a
message rather than falling through; `c` on the row chainloads.

A route falls through only before it has committed to the medium. A
failure after that is reported on screen and the key returns to the menu.

A code seen on an error screen names the route that failed, which is the
first thing to report with it. `docs/DESIGN.md` Section 12 draws each
route.

### 1.4 Stationary build

The build for a machine's own disk (`docs/DESIGN.md` Section 13):
`falcon-boot-pos.efi` for UEFI, and `falcon-bios-pos-stage3.bin` as stage 3
behind the stick's stages 1 and 2 for BIOS. Either opens at the passphrase
prompt; Escape there goes to the list.

UEFI:

| code | route | reached by |
|---|---|---|
| `POS` | personal OS, as on the stick, from the boot disk's shelf | the prompt it opens at, or the Personal OS row |
| `NXT` | a firmware boot entry: BootNext is set to it and the machine restarts | its row, labelled with the entry's own description |

Every entry the firmware's own boot menu offers is a row (active, not
hidden, boot category), except the one this boot came through.

BIOS:

| code | route | reached by |
|---|---|---|
| `POS` | personal OS, as on the stick, from the boot disk's shelf | the prompt it opens at, or the Personal OS row |
| `MBR` | another disk's own boot code, started from 0x7C00 with DL naming that disk | its row, `Disk 0x81` and so on |

Every BIOS disk but the boot disk whose first sector carries the boot
signature is a row.
