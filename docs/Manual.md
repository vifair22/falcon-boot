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
block: on the UEFI loader, one row above the block's first line; on the BIOS
loader, row 0 of the screen. It appears as soon as the route is chosen and
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
| `DIR` | direct kernel boot: the ISO's kernel and initrds with falcon-hook appended | Enter or `d` on a Linux ISO, Secure Boot off |
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
| `DIR` | direct kernel boot, falcon-hook appended | Enter on a Linux ISO |
| `WIN` | Windows install media, boot.wim patched, the tail served from extended memory | Enter on a Windows ISO |
| `BSD` | a BSD medium served as its own memory disk | Enter on a BSD ISO |
| `CHN` | the ISO's own bootloader off the resident virtual CD | `c` on an ISO, or a fallback |

The BIOS loader also writes `route <code>` to serial (COM1) when a route is
chosen.

### 1.3 Fallthrough

| loader | shown first | then | when |
|---|---|---|---|
| UEFI | `D3A` | `D3B` | the ISO's kernel has no `.sbat`, so its shim would not run it |
| UEFI | `DIR`, `D3A` or `D3B` | `CHN` | no boot entry in the ISO's configuration the loader can take |
| both | `WIN` | `CHN` | Windows media with nothing to patch |
| UEFI | `BSD` | `DIR`, `D3A`/`D3B`, or `CHN` | the medium is not one the memory-disk route applies to |
| BIOS | `BSD` | `DIR` | as above |

On the BIOS loader, `DIR` with no boot entry it can take stops with a
message rather than falling through; `c` on the row chainloads.

A code seen on an error screen names the route that failed, which is the
first thing to report with it.
