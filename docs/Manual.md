# falcon-boot Manual

The operator's reference: what falcon-boot does, what the operator sees and
types, and what to do when something fails. `docs/DESIGN.md` is the design;
this file cites it by section and does not repeat it.

1. The menu
2. Route codes
3. Secure Boot states and enrollment
4. Blessing
5. The personal OS
6. Windows media
7. The station
8. STATION.INI reference
9. falcon jump
10. falcon-boot-mkstick
11. Signing, and what lives where
12. Troubleshooting by failure page
13. Building and testing
14. Releases

## Start here

To make a bootable stick and nothing more:

1. Install `falcon-boot-bin` on Arch, or unpack the release tarball under `/`
   (Section 14).
2. `sudo falcon-boot-mkstick create /dev/disk/by-id/usb-<your stick> --force`
   (Section 10), then `falcon-boot-mkstick bios <device>` if you want the BIOS
   loader too.
3. Copy `.iso` files onto the partition labelled `FALCON-ISO`. They appear in
   the menu and boot.

With Secure Boot on, the first boot enrolls the MOK in MokManager (Section
3.1). Everything past Section 1 is the operator's reference: come back for the
station, blessing, the personal OS, falcon jump, or a failure page.

## 1. The menu

Both loaders on a stick open at the same menu: the title bar on the top
row, the list in a panel under it, a line of counts under the panel, and
the key hints on the bottom row, over a backdrop of dark grey shade.
Questions, notices and failures open in a box over the menu
(`docs/DESIGN.md` Section 7.10; `docs/tui-mockup.html` shows every screen).

### 1.1 What is listed

The UEFI loader lists every `.iso`, `.img` and `.efi` on p2; the BIOS loader
every `.iso` and `.img`. The extension is matched without regard to case.
Names starting with `.` are skipped, files and folders alike. Files are
found in p2's root and up to three folders below it, at most 128.

The list is shown a folder at a time, sorted by name without regard to
case: folders first, then the files in the folder. A folder is listed only
when something under it is. The panel's title is `ISO library` at the root
and the folder's path inside one, where the first row is the way back up,
and it names the folder too:

    ..  (now in /linux/)

At the root the first row is the personal OS, when this stick's shelf is in
sight and can be opened:

| row | when |
|---|---|
| `Personal OS (encrypted)` | an encrypted shelf with a keyslot the loader can use |
| `Personal OS (plaintext shelf)` | a plaintext shelf with something on it to boot, and no such encrypted shelf |

On UEFI the last row at the root is `Reboot to firmware setup`, when the
firmware offers it (`OsIndicationsSupported`). It asks first, in a box
titled `Firmware setup`:

    Reboot into this machine's firmware setup?

    y to reboot, any other key to go back.

A firmware that refuses says so, and the menu comes back:

    The firmware refused the request, so nothing was changed. Use its own
    key at power-on instead.

    press any key for the menu.

A menu with no rows at all says, in the panel:

    (no ISOs found on the exFAT partition)

A list longer than the panel scrolls. The panel's right border is then a
scroll bar: a block for where the window is in the list, and an arrow at
either end while there is more that way. The bottom border carries the
selected row's position, `20/41`, at its right, and at its left what the
selected image is: its size, and on UEFI its family once it has been looked
at. The line under the panel counts the images, and while the probe is
still reading, how many it has left: `41 images, 6 still being read`.

### 1.2 Keys

The key hints on each loader:

    enter boot   c chain   d direct   b bless   a bless new   esc back

    enter boot   c chain   d direct   esc back

The first is UEFI, the second BIOS. Letters are taken in either case. The
selected row is a cyan bar with a `>` in its first cell; a row predicted
refused (Section 1.4) is yellow, and selected, a brown bar.

| key | on |
|---|---|
| Up, Down | move the selection; it stops at the first and last row |
| Page Up, Page Down | move the selection a panel's height |
| Home, End | the first row, the last row |
| Enter | boot the row, open the folder, go up on `..` |
| `c` | an `.iso` or `.img` row: boot the medium's own loader (`CHN`; `IMG` for a `.img` on UEFI). Any other row: as Enter |
| `d` | as Enter; on a blessed row, the ISO's default route without the blessing |
| `b` | UEFI. An image row: bless it. Any other row: as Enter |
| `a` | UEFI. Bless every image on the stick that can be blessed and is not, from any row |
| Escape | in a folder, go up one; at the root, leave the menu (Section 1.6) |

On a blessed row Enter boots the blessing; `c` and `d` boot the ISO as if it
were unblessed. `b` and `a` open a confirmation before anything runs; the
pages are in Section 4.

Which route each key takes is in Section 2. A row tagged `{BIOS ONLY}`,
`{EFI ONLY}` or `{64-BIT ONLY}` shows a page instead, on every key
(Section 1.4).

### 1.3 The title bar

The title bar is `falcon-boot <version>` at the left, a route's code after
it while one runs (Section 2), and the machine's state at the right. On
BIOS the state is `BIOS`. On UEFI it is the Secure Boot state:

| state | meaning |
|---|---|
| `SB: off` | Secure Boot is off |
| `SB: on  trusts: MOK, MS UEFI CA, MS Windows, +2` | Secure Boot is on; what db and MokList hold |
| `SB: on  trusts: ?` | Secure Boot is on, and db could not be read or held nothing recognised |
| `SB: on (shim validation DISABLED)` | Secure Boot is on and shim's validation is off (`MokSBState`); drawn yellow on red |
| `SB: setup mode` | the platform has no PK, so nothing is enforced |
| `SB: unknown` | the variables do not give a state |

When the Secure Boot variables cannot be read at all, the title bar has the
title alone. When the title and the state do not both fit, the title is
cut.

The `trusts:` list names only what is there: `MOK` is this stick's own
certificate, `MS UEFI CA` and `MS Windows` are Microsoft's two CAs, and
`+N` counts any other certificates.

While the background probe is reading entries, a spinner turns on the
panel's top border at its right end (`/`, a full-width dash, `\`, `|`). It
stops when every entry has been looked at.

### 1.4 Row tags

An image row carries up to two tag columns after its name: a bracket, and a
brace. A row the probe has not reached yet shows `...` in the first column.
The probe works in the background for as long as the menu is up, starting
with the selected row, then the folder on screen. Tags are predictions; a
row stays selectable whatever it carries, except for the three platform
tags at the end of this section (`docs/DESIGN.md` Section 7.8).

The bracket is on UEFI only, and says who signed what the row boots:

| bracket | meaning |
|---|---|
| `[MOK]` | signed by this stick's key |
| `[MS UEFI CA]`, `[MS Windows]` | signed under Microsoft's CA |
| `[<name>]` | signed under another certificate, named by its CN |
| `[enrolled hash]` | the image's hash is in db or MokList |
| `[unsigned]` | no signature |
| `[?]` | a signature whose certificate has no readable name |
| `[blessed]` | a fresh blessing boots this row, or a `.efi` signed in place with the stick's key |
| `[signed]`, `[unsigned]`, `[sig?]` | what was found on the medium, when no prediction could be made |
| `[no efi]` | the medium has no EFI boot image |

A row whose blessing is stale carries `[blessed, stale]` after its bracket:
the ISO has changed since it was blessed, and the row boots as if
unblessed.

The brace is the reason the platform is expected to refuse the row. A row
with one is drawn yellow.

| brace | loader | shown when |
|---|---|---|
| `{UNTRUSTED}` | UEFI | Secure Boot on; signed, and nothing enrolled reaches the signature |
| `{NOT BLESSED}` | UEFI | Secure Boot on; unsigned |
| `{BLESS STALE}` | UEFI | Secure Boot on; one of the two above, with a stale blessing |
| `{REVOKED}` | UEFI | Secure Boot on; dbx, MokListX or the SBAT level revokes it |
| `{VALIDATION OFF}` | UEFI | shim validation disabled; on the personal OS row, a blessed Linux row, a `.efi` that is a UKI, and the station's UKI rows |
| `{BIOS ONLY}` | UEFI | every state; the medium has no UEFI boot loader and no kernel UEFI can start |
| `{EFI ONLY}` | BIOS | the medium has no BIOS boot image and no kernel falcon-bios can start |
| `{64-BIT ONLY}` | BIOS | the medium needs long mode, and this processor has none |

The last three are not predictions. Any key on such a row opens a box,
titled `BIOS only`, `EFI only` or `64-bit only`, and starts nothing:

    this image boots only on a BIOS machine: it has no UEFI boot loader,
    and no kernel UEFI can start.

    Start the machine from this stick in BIOS mode (legacy or CSM) instead.
    The stick needs its BIOS loader for that (mkstick bios).

    press any key to return to the menu.

    this image boots only on a UEFI machine: it has no BIOS boot image, and
    no kernel falcon-bios can start.

    Start the machine from this stick in UEFI mode instead.

    press any key to return to the menu.

    this image needs a 64-bit processor, and this one has no 64-bit mode.

    Start it on a machine with a 64-bit processor instead.

    press any key to return to the menu.

Each box starts with the row's path.

### 1.5 Startup steps

Until the menu is up, each loader lists the steps it takes in a box titled
`starting`, each one marked OK as the next begins:

| step | loader |
|---|---|
| `reading the Secure Boot keys` | UEFI |
| `finding disks` | both |
| `reading the shelf` | both |
| `reading the ISO library` | both |
| `reading the TPM` | UEFI station |
| `finding boot entries` | stations |
| `reading the floppy` | falcon jump |

A row chosen before the probe reached it is looked at first, under a
`checking the image` step on a page named for the row.

Before any of these, the BIOS loader's stage 2 shows
`falcon-boot: loading` with a turning spinner while stage 3 is read.

With shim validation disabled, every UEFI menu says so on the line under
its panel, in yellow on red:

    shim signature validation is DISABLED (MokSBState): every unsigned EFI binary runs here

`mokutil --enable-validation` from Linux, confirmed at MokManager on the
next boot, turns it back on.

When the loader cannot read its own partition's record, it says so once,
in a box titled `This install`, and waits for a key; the menu then offers no
ISO library and no shelf:

    The partition this loader started from has no falcon record,
    so no ISO library or shelf is offered.
    Write the stick with a current mkstick.

    press any key to continue.

The other forms of the first sentence:

| loader | sentence |
|---|---|
| both | `The falcon record on the partition this loader started from is damaged or not a boot record, so no ISO library or shelf is offered.`, then `Write the stick with a current mkstick.` |
| UEFI | `The firmware did not name the partition this loader started from, so no ISO library or shelf is offered.` |
| both | `The partition this loader started from would not read, so no ISO library or shelf is offered.` |

On UEFI the variable `FalconDiscoverStatus` says which check stopped it
and what the firmware answered. It lasts until the next reboot: `cat` it
from `/sys/firmware/efi/efivars/` in an OS started from that same boot
(the first four bytes are its attributes).

### 1.6 Leaving the menu

Escape at the root leaves the loader. The page it leaves on, the cursor
under it:

    falcon-boot: returning to firmware.

with any of these under it:

| line | loader |
|---|---|
| `note: no readable ISO volume was found.` | both |
| `note: <n> image(s) not shown (list full or path too long)` | both |
| `note: <n> block device(s) past the inventory cap were not looked at` | UEFI |
| `note: <n> blessed-artifact record(s) past the cap were not read` | UEFI |

The UEFI loader returns to the firmware, which goes on to its next boot
option. The BIOS loader calls INT 18h, and resets the machine if the BIOS
returns from it.

A menu or page left five minutes with no key powers the machine off, on
every loader (`docs/DESIGN.md` Section 7.9). The UEFI loader asks the
firmware to shut down. The BIOS loader tries APM, then ACPI; on a machine
that takes neither, the screen goes blank until a key. On a station,
`STATION.INI`'s `[general] poweroff` changes the time.

## 2. Route codes

The route code is three characters, drawn in white in the title bar after
the version. It says which boot path is running. It appears as soon as the
route is chosen, before the route's first step, stays through any screen the
route draws (the passphrase prompt, an error), and is removed on return to
the menu. A box that starts nothing (Section 1.4's platform boxes, the
firmware-setup question) has no code.

When a route falls through to another one, the code changes before the new
route's first line. The cases are in Section 2.3.

### 2.1 UEFI loader

| code | route | reached by |
|---|---|---|
| `POS` | personal OS: a UKI from p3, with the passphrase credential beside it | the personal OS row |
| `APP` | the signing appliance, a UKI from p3 | `b` on a row, `a` anywhere |
| `BLS` | a blessed wrap: the MOK-signed UKI built for a Linux ISO; or memtest86+'s own loader, MOK-signed | Enter on a blessed row |
| `BLB` | a blessed BSD medium: its loader, MOK-signed, started with the medium as its device | Enter on a blessed BSD row |
| `EFI` | a bare `.efi` file on p2, launched as itself | Enter, `c` or `d` on a `.efi` row |
| `DIR` | direct kernel boot: the ISO's kernel and initrds with falcon-hook appended | Enter or `d` on a Linux ISO, Secure Boot not on (off, validation disabled, setup mode, unknown) |
| `D3A` | the ISO's own shim, which loads the distro's signed kernel | Enter or `d` on a Linux ISO whose kernel carries `.sbat`, Secure Boot on |
| `D3B` | the ISO's own signed GRUB, with our grub.cfg served in place of its own | Enter or `d` on a Linux ISO whose kernel has no `.sbat`, Secure Boot on |
| `WIN` | Windows install media, boot.wim patched in memory as it is served | Enter or `d` on a Windows ISO |
| `BSD` | a BSD medium served as its own memory disk | Enter or `d` on a BSD ISO |
| `CHN` | chainload: the ISO's own bootloader off the virtual CD | `c` on an ISO, or a fallback |
| `IMG` | a `.img` served as a disk, and its own bootloader started | Enter, `c` or `d` on a `.img` row |

`c` and `d` on a blessed row bypass the blessing and boot the ISO as if
unblessed, so the code is one of `DIR`, `D3A`, `D3B`, `WIN`, `BSD` or `CHN`
rather than `BLS`.

### 2.2 BIOS loader

| code | route | reached by |
|---|---|---|
| `POS` | personal OS: a UKI from p3, unwrapped and its kernel started | the personal OS row |
| `DIR` | direct kernel boot, falcon-hook of the kernel's width appended | Enter or `d` on a Linux ISO |
| `WIN` | Windows install media, boot.wim patched, the tail served from extended memory | Enter or `d` on a Windows ISO with a boot.wim (Vista and later) |
| `WXP` | Windows 2000/XP text-mode setup: txtsetup.sif edited in memory to load a virtual-CD driver, then the ISO's own loader off the resident virtual CD | Enter or `d` on Windows 2000/XP media |
| `BSD` | a BSD medium served as its own memory disk | Enter or `d` on a BSD ISO |
| `CHN` | the ISO's own bootloader off the resident virtual CD | `c` on an ISO, or a fallback |

The BIOS loader mirrors every screen to serial (COM1), the route code
included, as the escape sequences OVMF's terminal sends for the UEFI
loader's, its boxes in ASCII. Both loaders also write a transcript there:
every page's title, each step as it starts, every line of a page or a box,
and each menu's title, rows and key hints as it opens, one to a line
(`docs/DESIGN.md` Section 7.10).

The BIOS loader is one image for every i586 or later processor, and boots
32-bit and 64-bit kernels alike. On a processor with no 64-bit mode, a row
the probe has tagged `{64-BIT ONLY}` shows its page and starts nothing
(Section 1.4). A 64-bit kernel the probe did not tag is refused when it is
about to start, on `DIR`:

    direct boot failed while starting the kernel

    the kernel is 64-bit and this processor has no 64-bit mode

and on `POS` as `personal OS boot failed while starting the kernel`. A key
returns to the menu.

A processor older than i586 stops before the menu, on screen and serial:

    falcon-bios: this processor is older than i586, which this loader needs
    press any key to reboot.

### 2.3 Fallthrough

| loader | shown first | then | when |
|---|---|---|---|
| UEFI | `D3A` | `D3B` | the ISO's kernel has no `.sbat`; also no boot entry the loader can take, or no shim on the ISO |
| UEFI | `DIR` or `D3B` | `CHN` | no boot entry in the ISO's configuration the loader can take, or (`D3B`) its grub.cfg cannot be replaced in place |
| UEFI | `WIN` | `CHN` | Windows media with nothing to patch |
| UEFI | `BSD` | `CHN` | the medium is not one the memory-disk route applies to |
| BIOS | `WIN` | `WXP` | Windows media with no boot.wim to patch |
| BIOS | `WXP` | `CHN` | not Windows 2000/XP media (no `i386/txtsetup.sif`) |
| BIOS | `BSD` | `DIR` | the medium is not one the memory-disk route applies to |
| BIOS | `DIR` | `CHN` | no boot entry in the ISO's configuration the loader can take, or its kernel is not a Linux kernel the loader can start (DOS, BSD, anything else) |

A route falls through only before it has committed to the medium. A
failure after that is reported on the route's page (Section 2.4).

A code seen on an error screen names the route that failed, which is the
first thing to report with it. `docs/DESIGN.md` Section 11 draws each
route.

### 2.4 The route page

Every route draws a page as it starts: a panel titled

    booting: <path>

(`POS` with the row's label, `APP` titled `signing appliance`), and in it
the route's steps as it takes them, each marked OK, or with what it read
(`14.2 MB`), when the next begins; the one under way has a spinner. Just
before the payload has the machine, the last step is marked OK, the rows
under the panel are cleared, and the cursor waits there: whatever the
payload writes first appears under the page.

When the route comes back, a box opens over the menu: red, titled `<what>
failed`, when it failed; grey when the booted image returned on its own. It
starts with the path, then one of:

    <what> failed while <stage>: <status>

    <advice>

    press any key to return to the menu.

on UEFI, or the image's exit line and status. On BIOS a route that comes
back has failed, and the line has no status:

    <what> failed while <stage>.

The advice is the route's sentence on what to do, when it has one.

| code | failure line | UEFI exit line |
|---|---|---|
| `POS` | `personal OS boot failed while ...` | `the image exited, status <status>` |
| `APP` | `the signing appliance could not be started while ...` | `the appliance exited without rebooting, status <status>` |
| `BLS`, `BLB` | `blessed boot failed while ...` | `the blessed image exited, status <status>` |
| `EFI` | `boot failed while ...` | `the payload exited, status <status>` |
| `DIR`, `D3A`, `D3B` | `direct boot failed while ...` | `the kernel exited, status <status>` |
| `WIN` | `boot failed while ...` | `the payload exited, status <status>` |
| `WXP` | `Windows setup failed while ...` | |
| `BSD` | `the memory-disk boot failed while ...` | `the ISO's loader exited, status <status>` |
| `CHN`, `IMG` | `boot failed while ...` | `the payload exited, status <status>` |

Lines some routes add:

| line | route |
|---|---|
| `entry: <title>`, under the path | `DIR`, `D3A`, `D3B`: the ISO's boot entry that was taken |
| `family: <name>`, under the path | `CHN`, `IMG`: what the medium was found to be |
| `press c on the entry to boot the ISO's own loader instead.` | `DIR`, `D3A`, `D3B` |

On UEFI, a `CHN` or `IMG` boot the platform refused adds:

    the platform refused to load this payload.

    Bless the media with the sign tool, turn Secure Boot off in firmware
    setup, or run mokutil --disable-validation from Linux.

After a platform refusal on `CHN`, `IMG`, `EFI`, `BLS`, `BLB` or `WIN`, the
UEFI loader lists what the row's tag was predicted from, one line per
signature:

    signature 1: <signer> <- <anchor> (<list>)
    revoked by <list> (image hash)

`<list>` is the store that decided: `db`, `MokList`, `dbx`, `MokListX`,
`SbatLevel`, or `not enrolled`. An image with no signature shows
`signature: none`, and one that could not be parsed `signature <n>:
unreadable`. `revoked by` appears only for a revoked image, and
`(image hash)` only when its hash is what was revoked.

A `CHN`, `IMG`, `EFI` or `WIN` payload that waited for a key and gave up
adds:

    the payload waited for a keypress and gave up. Windows media draws
    "press any key to boot from CD" on the graphics console, which this loader
    never sees. Boot it again and press a key while the screen looks empty.

The station's routes use the same two pages.

## 3. Secure Boot states and enrollment

The UEFI loader's title bar names the state the machine is in. It is read once
at startup:

| title bar | state |
|---|---|
| `SB: off` | Secure Boot off; nothing is verified |
| `SB: on  trusts: MOK, MS UEFI CA, MS Windows, +N` | Secure Boot on; what db and MokList hold: `MOK` is this stick's certificate, `MS UEFI CA` and `MS Windows` Microsoft's two CAs, `+N` any other certificates |
| `SB: on  trusts: ?` | Secure Boot on; the key stores could not be read |
| `SB: on (shim validation DISABLED)` | Secure Boot on and shim told not to verify (3.3), in yellow on red |
| `SB: setup mode` | no platform key; nothing is verified |
| `SB: unknown` | the variables could not be read |

The BIOS loader has no Secure Boot and no state to show.

### 3.1 First boot: enrolling the MOK

Every stick has its own MOK key pair. The loader and every UKI on p3 are
signed with it, and its certificate is `falcon-boot.cer` at the root of p1.
On a machine with Secure Boot on, shim does not run the loader until that
certificate is enrolled. `falcon-boot-mkstick create` ends with:

    First boot with Secure Boot ON: shim launches MokManager; enroll p1:/falcon-boot.cer, reboot.

On the first boot shim starts MokManager in place of the loader. In
MokManager: Enroll key from disk, the stick's ESP, `falcon-boot.cer`,
Continue, Yes, then reboot. The next boot reaches the menu.

From Linux on that machine instead:

    mokutil --import falcon-boot.cer

`mokutil` asks for a one-time password. On the next boot MokManager asks to
enroll the key and for that password.

Enrollment is per machine. A second machine needs it again; a second stick
has its own certificate and needs its own enrollment.

The station's certificate is `\EFI\falcon\falcon-boot.cer` on the machine's
ESP, and `station install` ends with:

    Secure Boot on: enroll \EFI\falcon\falcon-boot.cer with MokManager on the first boot (mokutil --import).

### 3.2 A machine that trusts only its owner's keys

A machine whose db holds its owner's certificates and nothing from
Microsoft refuses shim as `mkstick create` writes it: shim carries only
Microsoft's signatures. To boot the stick there:

1. Append a signature by the stick's MOK to p1's `/EFI/BOOT/BOOTX64.EFI`
   (shim) and `/EFI/BOOT/mmx64.efi` (MokManager), keeping the ones they
   carry. The recipe and the check are in Section 11.1.
2. Enroll `falcon-boot.cer` in that machine's **db** from its firmware
   setup.

Do not sign the stick with the machine's own db key.

The title bar then reads, for a db holding one owner certificate and this
stick's:

    SB: on  trusts: MOK, +1

A station needs no shim there, and a default `falcon-boot-mkstick station
install` has none: its firmware entry names the jumper, which starts the
loader, and it ends with:

    Secure Boot on: the firmware's db must hold \EFI\falcon\falcon-boot.cer; enroll it from firmware setup.

On such a machine no Microsoft-signed medium boots: a distro ISO's own
shim, and Windows install media, are refused. A blessed ISO boots (Section
4). `docs/DESIGN.md` Section 7.2 has the reasoning.

### 3.3 Turning enforcement off, and back on

Two ways, both the platform's and both per machine. The stick carries
neither.

**Secure Boot off in firmware setup.** The root of the menu ends with
`Reboot to firmware setup` when the firmware offers it. It asks:

    Reboot into this machine's firmware setup?

    press y to reboot, any other key to go back.

A firmware that refuses says so, and its own key at power-on is the way in:

    the firmware refused the request, so nothing was changed.
    Use its own key at power-on instead.

**shim validation off (MokSBState).** From Linux on that machine:

    mokutil --disable-validation

`mokutil` asks for a password; on the next boot MokManager asks to confirm
the change with characters of it. From then on the title bar reads
`SB: on (shim validation DISABLED)` in yellow on red, and every menu says
on the line under its panel, in the same colours:

    shim signature validation is DISABLED (MokSBState): every unsigned EFI binary runs here

To undo: `mokutil --enable-validation` from Linux, then confirm at
MokManager on the next boot.

In this state no UKI boots: the personal OS, the signing appliance and a
blessed wrap are all refused (`docs/DESIGN.md` Section 7.4). Their rows carry
`{VALIDATION OFF}`. Choosing one anyway prints, before the image is read:

    warning: this image is very unlikely to start on this machine;
             see the note after it exits.

and after the failure:

    shim signature validation is disabled, so shim left the firmware's own
    LoadImage in place, and the firmware refuses the kernel inside a UKI while
    Secure Boot is on.

    Re-enable validation (mokutil --enable-validation from Linux, then confirm
    at MokManager), or turn Secure Boot off in firmware setup. Booting an ISO
    the ordinary way still works.

On a blessed row, `c` or `d` boots the ISO itself, which works in this
state.

**Back on.** `mokutil --enable-validation` from Linux, confirmed at
MokManager on the next boot; or Secure Boot on again in firmware setup.

### 3.4 What boots in each state

Enter on each kind of row, by state. The columns are `docs/coverage.md`'s,
plus validation off. Setup mode boots as Secure Boot off does.
`docs/DESIGN.md` Section 7.2 has the reasoning; `docs/coverage.md` has the
measured result for each ISO.

| row | SB off | SB on, MS CA | SB on, own keys | SB on, validation off | BIOS |
|---|---|---|---|---|---|
| personal OS, signing appliance | `POS`, `APP` | `POS`, `APP` | `POS`, `APP` (3.2) | refused, `{VALIDATION OFF}` | `POS` |
| blessed Linux ISO | `BLS` | `BLS` | `BLS` | refused, `{VALIDATION OFF}`; `c` or `d` boots the ISO | boots unblessed |
| Linux ISO with its own Microsoft-signed shim | `DIR` | `D3A`, else `D3B` | refused | `DIR` | `DIR` |
| Linux ISO with no signed loader of its own | `DIR` | refused: bless it | refused: bless it | `DIR` | `DIR` |
| Windows install media | `WIN` | `WIN` | refused | `WIN` | `WIN`, `WXP` |
| BSD medium | `BSD` | blessed only: `BLB` | blessed only: `BLB` | `BSD` | `BSD` |
| memtest86+ ISO | `CHN` | blessed only: `BLS` | blessed only: `BLS` | `CHN`; blessed `BLS` | `CHN` |
| `.efi` on p2, unsigned | `EFI` | blessed only | blessed only | `EFI` | not listed |

A refusal with Secure Boot on is the platform's. With it on, a row the
platform is expected to refuse carries a tag in braces (`{NOT BLESSED}`,
`{UNTRUSTED}`, `{REVOKED}`, `{BLESS STALE}`), and choosing it still tries. A
refused unblessed Linux ISO ends:

    the platform refused this ISO's kernel. Bless the ISO, or press c on its row to boot its own loader.

and a refused payload:

    the platform refused to load this payload.

    Bless the media with the sign tool, turn Secure Boot off in firmware
    setup, or run mokutil --disable-validation from Linux.

A blessed EFI shell starts with Secure Boot on, but every program started
from it is refused with `The image is not an application.` (shim's loader,
`docs/DESIGN.md` Section 7.6). Use the shell with Secure Boot off.

## 4. Blessing

Blessing signs an ISO's boot with the stick's MOK so it boots with Secure
Boot on wherever that MOK is enrolled (`docs/DESIGN.md` Sections 7.5 and
8.1). For a Linux ISO the sign tool wraps the ISO's kernel, its initrds and
falcon-hook in a UKI, signs it, writes it under p2's `falcon/blessed/` and
adds a record to `falcon/blessed/manifest`; the ISO is not changed. A BSD
medium's own loader is signed instead and boots as `BLB`; so is
memtest86+'s, which boots as `BLS`. A `.efi` on p2,
or the EFI payload inside a `.img`, is signed in place: **the file itself
is rewritten**, with the signature appended to the ones it carries.

Windows media is never blessed; it boots as it is (Section 6). The BIOS
loader has no blessing: a blessed row boots there as if unblessed, and its
key hints have no `b` or `a`.

A blessed row's tag is `[blessed]`. Enter boots the blessing; `c` and `d`
boot the ISO as if unblessed.

### 4.1 From the menu

On the UEFI loader:

| key | blesses |
|---|---|
| `b` | the image row it is pressed on; on any other row it acts as Enter |
| `a` | every image on p2 not freshly blessed, from any row |

Neither takes a Windows ISO, an image tagged `{BIOS ONLY}`, or one already
blessed. The loader reads the images first (a `reading N images` step when
there are several), then asks, in a box titled `Bless`:

    Bless 2 images with the key on p3:

      /Linux/archlinux-2026.09.01-x86_64.iso
      /Linux/systemrescue-13.02-amd64.iso

    The signing appliance signs them and reboots into /Linux/archlinux-2026.09.01-x86_64.iso.

    y to bless, any other key to go back.

At most 12 paths are listed, then `  ... and N more`. ` into /<path>` is
there when the row the key was pressed on is in the list. A path that is
not plain ASCII, or is too long, is left out:

    cannot be listed for the appliance (not plain ASCII, or too long): <path>

`y` starts the signing appliance (`APP`): the loader asks for the shelf's
passphrase, and the appliance signs the list with no questions (4.2) and
reboots into the stick. When the row the key was pressed on is in the
list, the next-boot marker names it, and that boot starts the blessed image
without the menu.

Nothing to bless is one of:

| page | when |
|---|---|
| `Nothing new to bless.` | `a`, and every image is blessed or not blessable |
| `This image is already blessed.` | `b` on a blessed row |
| `This image boots only on a BIOS machine, where it needs no blessing.` | `b` on a `{BIOS ONLY}` row |
| `Only a Linux or other non-Windows .iso is blessed. Windows media boots as it is: select it and press enter.` | `b` on Windows media, or anything else not blessable |

When the stick has no shelf the loader can open:

    the signing appliance lives on this stick's encrypted shelf, and no
    shelf this loader can open is in sight. Bless on a Linux host with
    falcon-boot-sign instead.

On the appliance's page (`signing appliance`, `APP`), before it starts, the
loader may say, and goes on:

    the next-boot marker could not be written (<status>); the images are
    still blessed, and the menu comes back after the reboot.

    note: no boot entry for this stick to return to (<status>); after
    signing the machine boots whatever is first in the firmware's boot
    order.

A shelf with no appliance on it fails with:

    the signing appliance could not be started while looking for the signing appliance: <status>

    no signing-appliance image is on this stick's shelf.
    Bless the ISO on a Linux host with falcon-boot-sign instead.

### 4.2 The signing appliance

A UKI on p3 that signs what is on p2 with the MOK key on p3, then reboots.
It is started two ways:

- **With a list**, by `b` or `a` (4.1). It signs exactly that list.
- **Interactive**, from the personal OS chooser (5.1), where it is the last
  row, `Personal OS: falcon-boot signing appliance`. It surveys p2 and asks.

It finds the stick's p2 and p3 by the install ID the loader hands it, and
touches no other disk. It opens with:

    falcon-boot signing appliance
    =============================
    Signs what is on p2 - ISOs, and the bare EFI payloads beside them -
    with the MOK private key encrypted on p3.

      p2 (ISO library): /dev/sdb2
      p3 (UKI shelf):   /dev/sdb3

`waiting for the stick to enumerate...` is shown while the USB stick
appears; it waits up to 30 seconds.

**Survey.** p2's `.iso`, `.img` and `.efi` files are read (its top-level
`falcon` directory is skipped), each against the manifest:

    manifest: 3 blessed record(s)
    surveying p2...
      blessed     /Linux/archlinux-2026.09.01-x86_64.iso (already; skipping)
      windows     /Windows/Win11_25H2.iso
                  Windows media boots as it is; nothing to bless
      unreadable  /broken.iso
                  <the reason>
    survey: 2 to sign, 1 already blessed, 1 Windows, 1 unreadable

With a list, the survey is the list: `bless list from the loader:`, and
`  missing     /<path>` for a listed file that is not there. With nothing to
sign, p3 is not opened:

    nothing to sign; p3 stays locked. FALCON-APPLIANCE-PENDING 0

**Unlock.** The passphrase typed at the loader's prompt comes with the
appliance, and is tried first:

    unlocking p3 with the passphrase from the loader...

If it does not open p3, or there is none:

    the delivered credential did not open p3; falling back to the prompt.
    passphrase for p3 (attempt 1 of 3):

Typing does not echo. An empty line gives up (`no passphrase given; nothing
was unlocked.`); a wrong one is `wrong passphrase.`; after the third,
`out of attempts. p3 stays locked and no key was read.` Then
`p3 unlocked (read-only).` and `MOK keypair found on p3.` A shelf with no
key (`--p3-plain`) stops here:

    ERROR: no MOK keypair on p3 (falcon-mok.key / falcon-mok.crt).
           A stick built with --p3-plain has no on-stick key, so it
           cannot bless anything here; sign from a host instead.

A payload already carrying this stick's certificate is dropped from the
queue (`  blessed     /<path> (already; this stick's certificate is in it)`).

**Choosing** (interactive only). The pending images, by folder:

    pending unsigned images:

      /
        [ ]  1  memtest86+.efi  (2 MiB)
      Linux/
        [ ]  2  systemrescue-13.02-amd64.iso  (1043 MiB)
               blessed earlier; the file has changed since

    Mark what you vouch for, then review it:
      <n>       toggle image n      all, a / none, n  mark every / clear
      list, l   reprint the list    done, d           review the marked set
      quit, q   reboot without signing anything
    select>

Commands are case-insensitive. An empty line is `done`. `done` with nothing
marked asks for a mark. `quit` prints `leaving without signing anything.`
`done` prints the review: every marked path, and for each payload what the
sign tool would write, then

    Signing these says you trust every one of them to run on this
    machine with Secure Boot on.

and, when payloads are marked, that blessing one writes to that file.
`type 'yes' to sign them, anything else to go back to the list: ` takes
exactly `yes`; anything else is `not confirmed; back to the list (the marks
are kept).`

With a list, the review is `confirmed in the loader's menu:` and the paths.

**Signing.** Each image in turn:

    FALCON-APPLIANCE-SELECTED 2

    [1/2] /memtest86+.efi
          signing it in place...
          FALCON-APPLIANCE-SIGNED memtest86+.efi

    [2/2] /Linux/systemrescue-13.02-amd64.iso
          signing...
          FALCON-APPLIANCE-SIGNED Linux/systemrescue-13.02-amd64.iso

    summary: FALCON-APPLIANCE-SUMMARY signed=2 failed=0 skipped=0

One that fails is `FALCON-APPLIANCE-FAILED <path> (exit <n>)` and the rest go
on. `FALCON-APPLIANCE-*` lines are for scripts reading the console.

**End.** A run from a list that signed everything ends with
`rebooting to the loader.` Any other run waits:

    press Enter to reboot to the loader.

and with no console to read reboots after 30 seconds.

The errors before the survey:

| line | meaning |
|---|---|
| `ERROR: the loader passed no install ID (falcon.ident.cred).` | started some other way than from the loader |
| `ERROR: no ISO library and LUKS shelf carry this stick's install ID.` | followed by `disks seen:` and the kernel's disks |
| `ERROR: mounting p2: <reason>` | p2 is not readable exFAT |
| `ERROR: p3 (<dev>) is not a LUKS2 volume this appliance can open;` | p3 is plaintext or damaged |

Each waits for Enter.

### 4.3 Building and staging the appliance

    make appliance

builds `build/appliance/appliance-uki.efi`, unsigned, and

    make appliance-aa64

the same image for an AArch64 machine, `appliance-uki-aa64.efi`. Both build
in the harness (`make` runs there), which carries both sd-stubs and the
arm64 `sbsign`. Their kernel and modules are Fedora's, at the build
`tools/fetch-fedora-kernel.sh` pins; the first build downloads it into
`build/fedora-kernel/`. `APPLIANCE_KERNEL=<file>` names the kernel instead,
and `APPLIANCE_CMDLINE` replaces its cmdline (default
`console=ttyS1,115200 console=tty0 quiet loglevel=3 panic=15`, `ttyAMA0` on
AArch64). When the rig's test MOK exists each also writes a `-signed.efi`
copy, signed with that key, for the rig.

On a new stick, `create` signs and stages it:

    falcon-boot-mkstick create /dev/X --force --uki build/appliance/appliance-uki.efi ...

On an existing stick, sign it with the stick's MOK (Section 11.1) and copy
it into p3's `/uki` (5.4). The loader takes a UKI as the appliance by its
name: it must start with `appliance`, in any case.

### 4.4 falcon-boot-sign

    falcon-boot-sign [bless] <iso> --stick <dir> --key <mok.key> --cert <mok.crt> [options]

Blesses one ISO, `.efi` or `.img` from a Linux host. `bless` may be left
out. `--key` and `--cert` are the stick's MOK pair (p3's `/falcon-mok.key`
and `/falcon-mok.crt`, or `create --key-out`'s copy). `--help` prints the
usage and the known families with the cmdline template of each, or
`(signs its own loader)`.

| option | |
|---|---|
| `--stick <dir>` | p2's mount point; the wrap and the manifest go under its `falcon/blessed/` |
| `--key <file>` | MOK private key, PEM |
| `--cert <file>` | MOK certificate, PEM |
| `--name <file.uki>` | output name under `<stick>/falcon/blessed` (default: the ISO's name with `.uki` for `.iso`) |
| `--iso-path <path>` | the ISO's path from p2's root as the booted kernel sees it (default: from where it sits under `--stick`) |
| `--dev <token>` | how the cmdline names p2, such as `UUID=1234-ABCD` (default: `UUID=` and the filesystem UUID behind `--stick`) |
| `--cmdline <string>` | this cmdline instead of the family's template; required for an unknown family |
| `--kernel <path>` | the kernel inside the ISO, instead of the family's; required with `--cmdline` for an unknown family; a trailing `*` matches a suffix |
| `--cmdline-extra <s>` | added to the cmdline, before a `--` |
| `--stub <file>` | sd-stub to wrap with (default `/usr/lib/systemd/boot/efi/linuxx64.efi.stub`) |
| `--no-initrd` | kernel and cmdline only; the ISO's initramfs is not embedded and not signed |
| `--no-hook` | no falcon-hook; the initramfs finds the ISO file itself and must read p2's filesystem |
| `--work <dir>` | scratch directory (default `$TMPDIR` or `/tmp`) |
| `--dry-run` | print what would be written, write nothing |
| `--verbose`, `--quiet` | more or less diagnostic output |

A `.efi` or `.img` takes only `--key`, `--cert` and `--dry-run`, and is
signed in place. A `.img` is loop-mounted to reach its payloads, which
needs root; `\EFI\BOOT\BOOTX64.EFI` and `BOOTAA64.EFI` are signed on each
of its filesystems that has them. One already signed with this key is left as it is:

    already signed with this key, left alone: <path>

For example:

    falcon-boot-sign /mnt/p2/Linux/systemrescue-13.02-amd64.iso \
        --stick /mnt/p2 --key falcon-mok.key --cert falcon-mok.crt

prints:

    blessed Linux/systemrescue-13.02-amd64.iso
      uki      /mnt/p2/falcon/blessed/systemrescue-13.02-amd64.uki
      cmdline  <the cmdline signed into the wrap>
      size     <bytes>
      sha256   <hex>
      manifest /mnt/p2/falcon/blessed/manifest (record added)

A BSD medium or memtest86+ prints `loader` in place of `uki` and
`cmdline`; the loader is written as `<name>.efi`. memtest86+'s 32-bit ISO has
no x86-64 loader to sign:

    this medium has no /EFI/BOOT/BOOTX64.EFI, so there is nothing to sign for it

`uki` builds an unsigned UKI from parts, for an image the tool does not
bless (the appliance, a personal OS):

    falcon-boot-sign uki --linux <kernel> --cmdline <string>
                         --os-release <file> --output <uki>
                         [--initrd <file>] [--stub <sd-stub>]
                         [--bios-only]

`--bios-only` is for a kernel with no EFI stub: the UKI boots only on the
BIOS loader. Sign the result with `sbsign` (Section 11.1).

### 4.5 Stale blessings

A record names the ISO by its path on p2 and holds its size. When the file
on p2 has a different size, the blessing is stale:

- the row's tag is `[blessed, stale]`, and with Secure Boot on
  `{BLESS STALE}` where the ISO itself would be refused;
- Enter boots the ISO as if unblessed;
- the appliance lists it with `blessed earlier; the file has changed since`.

Bless it again (`b`, `a`, the appliance, or `falcon-boot-sign`); the sign
tool replaces the record (`record replaced`). An ISO moved or renamed on p2
has no record under its new path and is unblessed until blessed again.

## 5. The personal OS

The pinned first row of the stick's menu. It reads `Personal OS
(encrypted)` for a LUKS2 shelf and `Personal OS (plaintext shelf)` for a
plaintext one. Its route code is `POS` on both loaders.

### 5.1 The shelf and its images

The shelf is p3, the partition carrying the `shelf` record of this
install. The loader boots the regular files ending in `.efi`, any case, in
p3's `/uki`; when there is no `/uki`, in `/EFI/Linux`. Anything else there
is not offered, and the chooser offers at most 32.

One image boots without asking. Several are offered after the unlock, in a
panel titled `Personal OS`:

    Personal OS: Arch Linux 7.2.6-arch2-1
    Personal OS: Arch Linux 6.18.52-1-lts
    Personal OS: falcon-boot signing appliance

    enter boot   esc back

A row is `Personal OS: ` and the image's `.osrel` `PRETTY_NAME` (or
`NAME`), or its file name when it has neither. Rows that would read the same
gain the kernel version from `.uname`, or the file name in parentheses when
that does not separate them. The newest kernel is first and opens selected;
an image whose name starts with `appliance` is always last. Escape returns
to the menu.

### 5.2 Unlocking

The prompt is a box titled `Unlock the shelf` over the route's page:

    passphrase for the encrypted partition:
    [                                            ]

    enter unlock   tab no echo   esc back

Each key is shown as `*` in the field. TAB hides even those for the rest of
the entry. Escape returns to the menu. Enter closes the box and tries the
passphrase against every keyslot, under an `unlocking` step on the page.

The prompt (`POS`, on every loader and station) takes five wrong
passphrases a boot, however often the row is chosen again. After each of
the second, third and fourth the next attempt waits 4, 8 and 16 seconds
(the first costs nothing); the fifth powers the machine off
(`docs/DESIGN.md` Section 5.2). Neither is said on screen: the wait runs
under the `unlocking` step, and every refusal marks that step `wrong
passphrase` and opens the box again, empty, with

    wrong passphrase.

under the field.

On the UEFI station the TOTP lines are in the box, under the field, kept
current as the code rolls.

A keyslot whose argon2id memory the machine cannot give fails with:

    personal OS boot failed while unlocking the encrypted partition: <status>

    this keyslot needs 1024 MiB of memory; this machine has 512 MiB free.

Add a keyslot with a smaller memory cost from Linux (`cryptsetup
luksAddKey --pbkdf-memory <KiB>`). The BIOS loader says the same with no
status.

The other failures while unlocking:

| advice | |
|---|---|
| `no encrypted shelf of this install was found.` | no LUKS2 partition carries this install's record |
| `the passphrase did not open any keyslot.` | |
| `the partition could not be read.` | a read error |
| `the volume is outside what this loader supports.` | a LUKS2 header or cipher the loader does not take |
| `the decrypted volume is not a filesystem this loader can read.` | not ext4 inside |
| `no *.efi image for this machine was found in /uki or /EFI/Linux.` | none, or only images for the other architecture: the chooser lists a machine's own (x86-64 or ARM64) UKIs alone |

### 5.3 The passphrase handoff

The passphrase typed at the prompt also unlocks the OS's root, so it is
typed once. p3 and p4 share it.

The UEFI loader publishes the UKI on a read-only filesystem in memory, with
the passphrase beside it as `<uki>.efi.extra.d/cryptsetup.passphrase.cred`.
The UKI's sd-stub puts it in the initramfs at
`/.extra/credentials/cryptsetup.passphrase.cred`. The BIOS loader unwraps
the UKI and builds that credential archive itself, to the same path.

The OS needs a systemd initramfs and two `systemd-cryptsetup@.service`
drop-ins, named for example `falcon-credential.conf` (`docs/DESIGN.md`
Sections 5 and 5.1). The initramfs one reads the file:

    [Service]
    ImportCredential=
    LoadCredential=cryptsetup.passphrase:/.extra/credentials/cryptsetup.passphrase.cred

The real root's, in `/etc/systemd/system/systemd-cryptsetup@.service.d/`,
only clears the import, since a `LoadCredential=` there fails every boot
that did not come through falcon:

    [Service]
    ImportCredential=

Without the initramfs drop-in the root's unlock unit fails; without the
real root's, every `/etc/crypttab` unit fails, the shelf's included.

The credential reaches the initramfs only, so the running system opens the
shelf with a keyfile kept on the encrypted root, in a second keyslot. Without
one the shelf stays locked after boot, and nothing can put the next UKI on
it. As root, with the shelf's partition and a mapper name:

    install -d -m 700 /etc/cryptsetup-keys.d
    ( umask 077; head -c 64 /dev/urandom > /etc/cryptsetup-keys.d/<name>.key )
    cryptsetup luksAddKey --pbkdf pbkdf2 --pbkdf-force-iterations 1000 \
        <shelf-partition> /etc/cryptsetup-keys.d/<name>.key

and in `/etc/crypttab` and `/etc/fstab`:

    <name>  UUID=<shelf-luks-uuid>  /etc/cryptsetup-keys.d/<name>.key  luks,discard,nofail
    /dev/mapper/<name>  /efi  ext4  defaults,noatime,nofail  0 2

`cryptsetup luksUUID <shelf-partition>` gives the UUID. The passphrase
keeps keyslot 0 at its own cost. The keyfile's slot is pbkdf2 at 1000
iterations because the loader tries every keyslot after a mistyped
passphrase: an argon2id slot there would cost a second full derivation, and
the keyfile's 512 random bits need no stretching. `nofail` keeps a shelf
that will not open from stopping the boot.

The initramfs drop-in goes in through the initramfs builder, so a rebuild
keeps it. On Arch it is a `falcon-credential` mkinitcpio install hook; see
`docs/personal-os.md` for that recipe.

For dracut, a module puts it in, with the initramfs drop-in kept at
`/etc/falcon/initrd-cryptsetup.conf`:

    #!/bin/bash
    check() {
        # systemd-cryptsetup lives in systemd's utility directory, not on
        # PATH, so require_binaries cannot find it by name.
        [[ -x "${systemdutildir}/systemd-cryptsetup" ]] || return 1
        return 0
    }

    depends() {
        echo systemd crypt
    }

    install() {
        inst_simple /etc/falcon/initrd-cryptsetup.conf \
            /etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf
    }

and a `dracut.conf.d` file enables it beside the systemd initramfs:

    add_dracutmodules+=" systemd crypt falcon-credential "

`lsinitrd <uki> | grep falcon-credential` shows the drop-in in a built
image. A `kernel-install` plugin can automate the shelf side: put each signed
UKI on the shelf as `<name>-<kver>.efi` and keep `<name>.efi` as a copy of the
newest, so a STATION.INI default naming `<name>.efi` survives kernel updates.

When the handoff cannot be made the boot goes on, and the OS asks for the
passphrase again:

    warning: the credential filesystem could not be published; booting from
             memory instead. The initramfs will ask for the passphrase again.

and on the BIOS loader:

    warning: the passphrase credential could not be packed.
             The initramfs will ask for the passphrase again.

A plaintext shelf has no passphrase and hands none over.

### 5.4 Adding or replacing a UKI

1. Sign the UKI with the stick's MOK (Section 11.1). `falcon-boot-sign uki`
   (4.4) builds one from a kernel, an initramfs and an os-release file.
2. Copy it into p3's `/uki`. From the personal OS, p3 is mounted at
   `/efi`. From another Linux:

       cryptsetup open /dev/X3 shelf
       mount /dev/mapper/shelf /mnt
       cp NEW.efi /mnt/uki/
       umount /mnt
       cryptsetup close shelf

The name must end in `.efi`: a copy kept as `arch-usb.efi.prev` is never
offered. A rollback is `arch-usb-prev.efi`. A name starting `appliance` is
taken as the signing appliance. With Secure Boot on, a UKI not signed with
an enrolled key fails at the platform:

    the UKI is not signed with an enrolled key.

    Sign it with the MOK key this machine has enrolled, turn Secure Boot off
    in firmware setup, or run mokutil --disable-validation from Linux.

### 5.5 A shelf its OS left mounted

A shelf its OS left mounted read-write, because it hibernated or lost
power, is read through its journal and boots. When the OS lost power after
an fsync the booting page also says:

    the shelf's last few seconds of changes were not read.

A shelf that does not mount fails at `mounting the filesystem inside it`
(`mounting the plaintext UKI shelf` for a plaintext one) with one of:

| advice | what to do |
|---|---|
| `the shelf's journal is damaged; run fsck on it from another system, then boot its OS once with noresume.` | as it says; `noresume` keeps a hibernated OS from resuming over the repaired shelf |
| `reading the shelf's journal needs N MiB of memory; this machine has M MiB free.` | boot it on a machine with more memory |
| `the filesystem was not cleanly unmounted; run fsck on it from Linux.` | as it says |

Setting `/sys/power/freeze_filesystems` to 1 in the OS leaves the shelf
clean when it hibernates (`docs/DESIGN.md` Section 5.4).

### 5.6 Boot timing

The UEFI loaders, the stick and the station, write the variables systemd
reads to time a boot (`docs/DESIGN.md` Section 5.5) whenever they start a
UKI. On the booted OS:

    systemd-analyze
    systemd-analyze blame
    bootctl status

`systemd-analyze` splits the boot into firmware, loader and kernel:

- **firmware** is everything before falcon starts: the firmware's own
  start, its boot manager, and shim.
- **loader** is from falcon's start to the UKI's start. It includes the
  time the menu was up, its countdown, the time at the passphrase prompt
  and the image chooser, and the unlock and the read of the UKI.
- **kernel** starts with the UKI's sd-stub.

`bootctl status` names the loader (`falcon-boot <version>`) and the
firmware under "Current Boot Loader".

`FalconTimeWaitUSec` is the part of the loader span spent waiting on the
keyboard. After an unlock, `FalconTimeKdfUSec` is the part spent deriving
the keyslots, and `FalconKdfInfo` describes that unlock in one line: the
block function, the processors, the slot's lanes, memory and passes, the
time, and the boot processor's effective clock while it ran, which is
below the processor's rated clock when the firmware holds it there:

    argon2id avx2 4 processors 4 lanes 1048576 KiB t=4 1612 ms 2394 MHz

The clock is left out where the processor does not count it (no
APERF/MPERF) and under a hypervisor. This prints the firmware, falcon's
own work (the loader span without the wait) with the keyslots' part of
it, the wait, the time since the kernel's clock started, and the unlock's
line; each file is a 4-byte attribute word, then the value in UTF-16:

    v() { tail -c +5 /sys/firmware/efi/efivars/$1-$2 2>/dev/null | tr -d '\0'; }; L=4a67b082-0a4c-41cf-b6c7-440b29bb8c4f; F=50614485-081b-4b7c-8b54-2abeffdf2fb3; i=$(v LoaderTimeInitUSec $L); e=$(v LoaderTimeExecUSec $L); w=$(v FalconTimeWaitUSec $F); d=$(v FalconTimeKdfUSec $F); awk -v i="$i" -v e="$e" -v w="$w" -v d="$d" -v k="$(cut -d' ' -f1 /proc/uptime)" 'BEGIN { printf "firmware %.3f s, falcon %.3f s (keyslots %.3f s), wait %.3f s, kernel to now %.3f s\n", i/1e6, (e-i-w)/1e6, d/1e6, w/1e6, k }'; v FalconKdfInfo $F; echo

`FalconReadInfo` says what the firmware's reads cost on the way, in three
stretches: until the menu was up, while it was up (the background probe),
and from the boot's start to the UKI's (the unlock and the UKI itself),
with the last one's rate:

    tail -c +5 /sys/firmware/efi/efivars/FalconReadInfo-50614485-081b-4b7c-8b54-2abeffdf2fb3 | tr -d '\0'; echo
    startup 112 calls 1260 KiB 7840 ms, menu 40 calls 2560 KiB 2800 ms, boot 1430 calls 91504 KiB 100000 ms 0.93 MB/s

The milliseconds are the firmware's alone. A boot rate far under what the
stick does in Linux, or a `startup` that takes tens of milliseconds a call
for a few KiB each, is the firmware's USB driver, not the stick: the same
stick on another machine shows the difference. The line above is an ASUS
B550's AMI firmware with a Kingston DataTraveler Max, which costs about
70 ms for every 64 KiB.

Booted from a route that is not a UKI (an ISO, Windows, memtest86+), the
exec time and the wait are not written, and systemd-analyze shows neither
the firmware nor the loader span. A UKI booted without an unlock (a
plaintext shelf, a blessed ISO) has no keyslot time or line. The BIOS
loader writes none of them. The serial port gets a
`falcon-bli:` line at startup and another at the UKI's start, naming the
counter's rate and how it was measured (`cpuid` or `stall`).

## 6. Windows media

A Windows install ISO on p2 boots as it is, with Secure Boot on or off, on
any machine whose db trusts Microsoft (3.4). Enter or `d` is `WIN`: the
loader patches `boot.wim` in memory as it serves it, and in WinPE
falcon-winjump attaches the ISO so Setup finds its files. Nothing on p2 is
written. The page reads `booting with boot.wim patched in memory`.

falcon-winjump logs to `X:\falcon-winjump.log` in WinPE. Shift+F10 opens a
command prompt there to read it.

The BIOS loader checks the processor first, and stops at `checking the
processor` with one of:

    Windows needs PAE; this processor has none.
    Windows 8 and later need NX and SSE2; this processor has no SSE2.
    Windows 8 and later need NX and SSE2; this processor has no NX.
    this is 64-bit Windows and this processor has no 64-bit mode.

### 6.1 p2:/falcon/windows.cfg

Options for Setup, in `falcon/windows.cfg` on p2. Every key is optional;
without the file nothing changes.

    # one key = value a line
    win11_bypass=1
    unattend=falcon/autounattend.xml

| key | value |
|---|---|
| `win11_bypass` | `1`, `yes` or `true`: Setup skips Windows 11's TPM, Secure Boot, RAM, CPU and storage checks (`HKLM\SYSTEM\Setup\LabConfig`) |
| `unattend` | an answer file, by its path from p2's root; copied to `X:\falcon-unattend.xml` and set as Setup's `UnattendFile` |

Keys and values are case-sensitive; spaces around the `=` and at either end
of the line are ignored. A line starting `#` is a comment, and an unknown
key is logged and ignored. The file is read when WinPE attaches
the ISO itself; media whose WinPE needs falcon-vdisk (6.2) does not read it.
What was applied is in the log, as `options:` lines.

### 6.2 Windows 2000, XP and older WinPE

**Windows Vista and 7.** Their WinPE cannot attach an ISO. falcon-winjump
loads falcon-vdisk, a driver that serves the ISO as a CD-ROM from the
stick's disk, and Setup finds its files there. The driver loads in 32-bit
WinPE.

**Windows 2000 and XP** (BIOS loader only). The media has no `boot.wim`, so
`WIN` falls through to `WXP`: the loader adds falcon-vdisk to text-mode
Setup's driver list (`txtsetup.sif`) on the served CD, and the driver reads
the ISO from the stick. The page reads:

    booting Windows setup with the virtual-CD driver injected

A failure is `Windows setup failed while <stage>`, with one of:

    this machine has too little memory above 64 MB to inject the driver.
    there is not enough memory to inject the driver.
    txtsetup.sif is not one this loader can edit.
    the ISO's i386 directory has no room for the driver's record.
    this ISO's i386 layout is one this loader does not edit.

Media that is not 2000/XP text-mode setup falls through to `CHN`.

## 7. The station

The station (`docs/DESIGN.md` Section 12) is falcon as a machine's own boot
loader, beside the systems already on its disks: `falcon-boot-station.efi`
for UEFI, and `falcon-bios-station-stage3.bin` as stage 3 behind the
stick's stages 1 and 2 for BIOS. `falcon-boot-mkstick station install` puts
it on a disk; this section is the order a person does it in, and what the
station shows once it is there. `/dev/X` is the machine's disk.

### 7.1 What it needs

| item | where it comes from |
|---|---|
| `falcon-bios-stage1.bin`, `falcon-bios-stage2.bin` | `make bios` |
| `falcon-bios-station-stage3.bin`, `falcon-boot-station.efi`, `falcon-esp-jump.efi` | `make station` |
| `falcon-bios-station-legacy-stage3.bin` and jump's drivers in `build/bin/mod/` (station-legacy, 7.7) | `make station-legacy` |
| `falcon-boot-mkstick` | `make` |
| Fedora shim 16.1 and its MokManager (UEFI with `--shim` only) | `falcon-efi/tools/shim-spike/fetch-shims.sh`, into `build/shim/bins/` |
| a MOK key pair (UEFI) | the shelf's (`/falcon-mok.key`, `/falcon-mok.crt`), or `--key` and `--cert` |
| memtest86+ (optional) | `--memtest`: a build carrying the x86 boot protocol header, for BIOS; `--memtest-efi`: its `.efi`, for UEFI, signed with the MOK at install |
| `mkfs.fat`, mtools; `sgdisk` on GPT; `sbsign`, `openssl` on UEFI; `cryptsetup` or `debugfs` for a shelf | the distribution |

### 7.2 Free space

The installer resizes nothing. A BIOS station needs a partition of its own,
64 MiB by default and 40 MiB at least (288 and 272 MiB on a disk of
4096-byte sectors), in space no partition holds, and on MBR a free primary
slot; it refuses and says how much it needs when there is none. On a disk
one Windows partition fills, shrink it first, from any Linux:

```sh
ntfsresize -n -s <new size in bytes> /dev/X1    # a dry run
ntfsresize -s <new size in bytes> /dev/X1
echo '63,<new size in sectors>,7,*' | sfdisk -N 1 /dev/X
```

Windows runs chkdsk once on its next boot. A UEFI station needs no new
partition: it goes into the machine's ESP, which must not be mounted.

### 7.3 Install

```sh
falcon-boot-mkstick station install /dev/X --dry-run
falcon-boot-mkstick station install /dev/X --backup station.bak
```

The dry run prints the plan: where the station partition goes, the stage
sectors, the `STATION.INI` it will write and any disk signature it will
give. Both firmwares are installed where the disk allows it; `--bios` or
`--uefi` asks for one. `--shelf` names the shelf the station boots Linux
from, which is stamped with the station's install ID. Keep the backup file
off the disk it describes. The full option list, what is written where and
every refusal are under `falcon-boot-mkstick station` in the mkstick
reference.

The installer writes `STATION.INI` from what it finds on the machine's
disks: the first Windows or DOS is the default (else the first UKI on the
shelf, else the first other loader), with `timeout = 5`. Section 8 is the
file's reference.

It ends:

    /dev/X boots the station (install ID <32 hex digits>).
    `falcon-boot-mkstick station remove --backup station.bak` undoes it.

An AArch64 machine's station is the same with its own names
(`BOOTAA64.EFI`, `falconaa64.efi`, `shimaa64.efi`), UEFI only;
`--arch aa64` installs one from another machine, onto an image or a disk
that machine will boot.

A station has no shim by default: the firmware entry names
the jumper, `\EFI\BOOT\BOOTX64.EFI`, which starts `\EFI\falcon\falconx64.efi`;
a firmware that has lost the entry boots the same file by the
removable-media path (Section 10.6). With Secure Boot on the firmware's db
must hold `falcon-boot.cer`. Enroll the certificate before the first boot, from firmware
setup or, where the machine's own keys are yours, with them (`sbctl` keeps
it as a custom db certificate: copy the PEM `falcon-mok.crt` into
`/var/lib/sbctl/keys/custom/db/` and run `sbctl enroll-keys --custom
--partial db`). A stick is the other way round: `mkstick create` writes shim
by default, since it boots machines whose keys are not yours (Section 3).

`--shim build/shim/bins` puts shim and MokManager before the loader, for a
machine whose db is not yours: the firmware entry names shim, and the
first boot with Secure Boot on stops in MokManager to enroll
`\EFI\falcon\falcon-boot.cer` (or run `mokutil --import` on it before
rebooting).

### 7.4 Check and remove

```sh
falcon-boot-mkstick station status /dev/X
falcon-boot-mkstick station remove --backup station.bak
```

`status` says what is installed and what each menu offers, with the
default marked, and parses each `STATION.INI` as the loaders do (or names
the line it fails at). `remove` puts the old boot code back and deletes
falcon's files and firmware entry; the station partition stays until it is
deleted. `remove --full` also puts the partition table back as it was.

### 7.5 The station's menu

Both stations open at their list, a panel titled `boot entries`,
`STATION.INI`'s default highlighted and, when it sets a timeout, the count
on the panel's bottom border, with the key hints saying any key stops it:

    Booting in 5 s

Any key stops the countdown for good and is then taken as any key is, so
Enter boots the default at once. The countdown runs on the first list only;
a boot that comes back lands on the list with no countdown. Escape on the
list leaves for the firmware (on BIOS, INT 18h, then a reset if the BIOS
returns):

    falcon-boot: returning to firmware.

A page left with no key for five minutes powers the machine off, as on
every loader; `[general] poweroff` changes the time (Section 8.2).

**UEFI.** The rows, in the order found: `Personal OS (encrypted)` when a
LUKS2 shelf of this install is there; one row per UKI on a plaintext shelf,
labelled from its `.osrel` name; every entry the firmware's own boot menu
offers (active, not hidden, boot category), in `BootOrder`'s order, except
the one this boot came through and the station's own (`falcon`); Windows
Boot Manager on each ESP no firmware entry names; `Memory test
(memtest86+)` when `\EFI\falcon\memtest.efi` is on the station's ESP; and
`Reboot to firmware setup` last, when the firmware offers it.

| code | route | reached by |
|---|---|---|
| `POS` | a UKI from a shelf of this install, as on the stick | `Personal OS (encrypted)` (the prompt, then the chooser), a plaintext UKI's row, or a `uki` section's row (the prompt, then that file) |
| `NXT` | a firmware boot entry: `BootNext` is set to it and the machine restarts | its row, labelled with the entry's own description |
| `ESP` | an image on an ESP, loaded by device path: Windows Boot Manager, or a `STATION.INI` entry's `efi` | a `method = efi` or `efi` entry, or `Windows Boot Manager (disk gpt:1a2b3c4d, partition 1)` for an ESP no firmware entry names |
| `MEM` | memtest86+, `\EFI\falcon\memtest.efi` on the station's ESP | `Memory test (memtest86+)` |

`Reboot to firmware setup` has no route code. It asks first:

    Reboot into this machine's firmware setup?

    press y to reboot, any other key to go back.

and when the firmware refuses:

    the firmware refused the request, so nothing was changed.
    Use its own key at power-on instead.

    press any key for the menu.

`NXT` says `restarting into it...` before the reset. `ESP` says, before it
loads `bootmgfw.efi`:

    BitLocker will ask for the recovery key if the volume is sealed.

A UEFI route that comes back draws a page with the row's label and

    <route> failed while <stage>: 0x<status>

(the route is `personal OS boot` for `POS`, else its code), the advice, and
`press any key to return to the menu.` An image that returns on its own
gets `the image exited, status 0x<status>` instead. The station's own
advice:

| route | stage | advice |
|---|---|---|
| `NXT` | `finding its firmware entry` | `No firmware boot entry names this disk. Set method = efi in STATION.INI to start its bootmgfw.efi.` (a `disk` section that matched no firmware entry) |
| `NXT` | as the reset failed | `Use the firmware's own boot menu at power-on instead.` |
| `ESP`, `MEM` | `finding its ESP` | `No partition of that disk carries the file.` |

With no row at all the station leaves for the firmware at once, adding
`note: no personal OS shelf and no other boot entry was found.`

**BIOS.** `STATION.INI` is read from the station partition's root. The
rows, in the order found: each encrypted shelf carrying the station's
install ID as `Personal OS (encrypted)`, and each plaintext shelf's UKIs;
each primary partition of an MBR disk whose boot sector names a known
loader; each other disk whose LBA 0 has boot code and an active entry;
station-legacy's rows (7.7); and `Memory test (memtest86+)` when the
station partition holds `MEMTEST.BIN`. An empty list reads `(nothing to
boot was found)`.

| code | route | reached by |
|---|---|---|
| `POS` | a UKI off a shelf carrying the station's install ID, unwrapped as the stick's personal OS | a plaintext shelf's UKI rows, an encrypted shelf's `Personal OS (encrypted)` row (the prompt, then the chooser), a `uki` section's row (the prompt, then that file), or a UKI in station-legacy's `/uki` (no prompt) |
| `VBR` | a primary partition's boot sector: the disk's LBA 0 at 0000:0600 with the entry marked active, DS:SI at it, the boot sector at 0000:7C00 with its BPB's hidden sectors and drive number made to agree | a row such as `Windows (NTLDR) (disk mbr:cdcccdcc, partition 1)` |
| `MBR` | another disk's own boot code, started from 0x7C00 | a row such as `Boot code (disk mbr:1a2b3c4d)` |
| `MEM` | memtest86+, `MEMTEST.BIN` in the station partition, as jump starts it | `Memory test (memtest86+)` |
| `CDR`, `USB`, `PCC` | station-legacy only: jump's routes (9.3) | `CD drive`, `USB stick`, `CF card (PC Card)` |

A partition row's loader is named from its boot sector: `Windows
(BOOTMGR)`, `Windows (NTLDR)`, `Windows 9x` (WINBOOT.SYS), `FreeDOS`
(KERNEL.SYS), `MS-DOS` (IO.SYS or IBMBIO.COM), `GRUB` or `SYSLINUX`
(SYSLINUX or EXTLINUX). A chain of a disk that is not 0x80 swaps that disk
with 0x80 for everything it boots (`swap = no` in its section turns that
off) and leaves 1 KB of conventional memory taken: the BDA's count at 0x413
reads 1 KB less.

A BIOS route that comes back draws a page with the row's label, `<route>
failed while <stage>` (`personal OS boot` for `POS`, else its code, and no
status), the advice, and `press any key to return to the menu.` The
station's own advice, beyond the stick's for `POS` and `MEM`:

| route | stage | advice |
|---|---|---|
| `POS` | `looking for a UKI shelf` | `No shelf of this install was found.` |
| `VBR`, `MBR` | `finding the disk` | `The disk this entry names is not attached.` |
| `VBR` | `finding the partition` | `The disk has no partition of that number.` |
| `VBR` | `checking the partition` | `The disk's sectors are not 512 bytes, and a BIOS starts boot code only from a disk whose sectors are.`, `A partition on a GPT disk has no boot sector a BIOS starts.`, `It is a logical partition; DOS, Windows 9x and NT start only from a primary one.` or `It is an extended partition, which holds other partitions and has no boot sector.` |
| `VBR` | `reading the disk's partition table` | `The disk did not answer a read.` or `The disk's partition table is not the one the menu was built from.` |
| `VBR` | `reading the partition's boot sector` | `The disk did not answer a read.` or `The partition's first sector carries no boot code.` |
| `MBR` | `reading the disk's first sector` | `The disk's sectors are not 512 bytes, and a BIOS starts boot code only from a disk whose sectors are.` or `The disk did not answer a read.` |
| `MBR` | `checking the boot signature` | `Its first sector carries no boot code.` |
| `VBR`, `MBR` | `installing the drive swap` | `There is no conventional memory left for it; swap = no in STATION.INI starts this entry without it.` |
| `MEM` | `opening MEMTEST.BIN`, `starting memtest86+` | the kernel loader's reason |

**A `STATION.INI` that cannot be taken** is said once, before the list,
which then waits with no default. One of four pages, the same on both
stations:

    STATION.INI line 2: unknown key 'bogus'.

    The menu will wait with no default.

    press any key to continue.

with `STATION.INI is larger than 16 KB and was not read.` or `STATION.INI
could not be read.` in place of the first line when the file is too large
or will not read, and, when the file parses but its default names no row on
this machine:

    STATION.INI's default, win2k, names no entry on this machine.

(the BIOS station breaks that line after `win2k,`). A missing file says
nothing: the list waits with no default.

**Next boot.** `falcon-boot-mkstick next-boot <name>` asks the station to
boot one row on the next boot without the list (on UEFI the
`FalconNextBoot` variable, on BIOS the marker sector of the station
partition). The value is a row's name, generated or a section's (Section
8.4), hidden rows included. `uki-<stem>` that no row carries boots
`<stem>.efi` from the encrypted shelf, behind the prompt. A value that is
not acted on is said before the list, on a page ending `press any key for
the menu.`:

    the next-boot marker names <value>, and no entry has that name.
    It has been cleared; nothing was booted from it.

    the next-boot marker held something that is not an entry name.
    It has been cleared; nothing was booted from it.

and on BIOS, where the marker is dated:

    note: the next-boot marker was not set within the last ten minutes.
    note: this machine's clock did not answer, so the next-boot marker could not be dated.
    note: the next-boot marker could not be cleared, so nothing was booted from it.

(each but the last followed by `It has been cleared; nothing was booted from
it.`). A boot that comes back lands on the list, with no countdown.

### 7.6 The TOTP check

The UEFI station can show a six-digit TOTP code, released by the
machine's TPM (2.0 or 1.2) only while the firmware, its settings and the
boot chain are what they were at enrollment. Compare it with an
authenticator before typing a passphrase (`docs/DESIGN.md` Section 12.15).
The code is a label; the station boots the same either way.

Enroll from a system the station started, on the station's machine:

    falcon-boot-mkstick station totp enroll /dev/X

It takes the PCR values the station read at its menu, which it saves on
every boot, checks them against the firmware's event log up to the
station's own measurement, seals a new seed in the TPM (NV index 0x01800FA1 on 2.0,
0x20000FA1 on 1.2) to PCRs 0, 2, 4 and 7 (`--pcrs` for others), writes
`STATION.INI`'s `[totp]` section, and prints the secret once:

    Add this to an authenticator now; it is not shown again.
      account: falcon-station:<host>
      secret:  <base32>
      uri:     otpauth://...

with a QR code when `qrencode` is installed (`qrencode is not installed,
so there is no QR code: enter the secret by hand.` otherwise). `--label`
names the account instead of the host name. `--owner-auth-file` gives the
TPM owner's authorisation: the file's bytes with a trailing newline
dropped; on 1.2 exactly 20 bytes is the secret itself, anything else a
passphrase whose SHA-1 it is. The default is none on 2.0 and the
well-known secret on 1.2.

On a TPM 1.2 the station saves its own event log at boot (`FalconEvLog`):
boot the station once before enrolling on a machine where it has not run.

On the station, the code is drawn in a box titled `TOTP` beside the menu
and again under the passphrase field, kept current as it rolls:

    TOTP: 123 456
    (falcon-station:<host>)

The line in its place when there is no code:

| line | when |
|---|---|
| `The TPM did not release the code:` / `firmware, its settings or the boot chain changed since enrollment.` | a PCR is not what it was; `station totp reseal` once the change is trusted |
| `No TPM 2.0 found.`, `No TPM 1.2 found.` | the TPM `[totp]` names did not answer |
| `The TPM key is not the enrolled one.` | the TPM's storage key is not the one `[totp]` pins |
| `The TPM's answer did not verify.` | the session's answer failed its check |
| `The TPM did not release the code: TPM error 0x<code> in <command>.` | any other refusal (`in <command>` on 1.2) |
| `The TPM could not be read.` | the read failed below the TPM's own answers |
| `The TOTP code is shown again after a restart.` | something else ran since the seed was read |
| `The firmware clock could not be read.`, `The code could not be computed.` | as said |

```sh
falcon-boot-mkstick station totp reseal /dev/X   # a new seed, sealed to this boot
falcon-boot-mkstick station totp remove /dev/X   # the index deleted, [totp] removed
falcon-boot-mkstick station totp check /dev/X    # the station's own read, from here
```

After a reseal the authenticator's old entry stops matching; replace it.
`check` changes nothing. From a running system the TPM refuses on the PCRs,
and that is the good answer:

    the TPM refused on the PCRs, as it does outside the station: the index, the storage key recorded in STATION.INI are this TPM's

(`the storage key and its secret` on 1.2). `mkstick update` on a station
with `[totp]` reseals it to the new loader in the same run (7.8).

### 7.7 station-legacy and DevLink recovery

station-legacy (`docs/DESIGN.md` Section 12.10) is the BIOS station plus
falcon jump's routes, for a machine whose BIOS cannot boot a CD, a USB
stick or a CF card: `falcon-bios-station-legacy-stage3.bin` from `make
station-legacy`, which also builds the drivers. BIOS only.

```sh
make station-legacy devlink-uki
falcon-boot-mkstick station install /dev/X --bios --legacy \
    --uki build/devlink/devlink-recovery.efi --memtest <memtest86+>
```

`--legacy` takes the station-legacy stage 3 and copies `ATA.MOD`,
`EHCI.MOD`, `UHCI.MOD`, `OHCI.MOD` and `PCC.MOD` from `--mods` (default
`build/bin/mod`) into the station partition, printing `warning: no <NAME>.MOD in
<dir>; not copied` for one that is missing. Each `--uki` goes into the
station partition's `/uki`.

It adds, after the partition and disk rows and in jump's order:

| row | name | there when |
|---|---|---|
| `CD drive` | `cd` | `ATA.MOD` is in the station partition and an IDE controller is on the PCI bus |
| `CF card (PC Card)` | `cf` | `PCC.MOD` is there and a CardBus bridge is on the bus |
| `USB stick` | `usb` | `EHCI.MOD`, `UHCI.MOD` or `OHCI.MOD` is there and its controller is on the bus |
| one per UKI in `/uki` | `uki-<stem>` | always; labelled by its `.osrel` name, booted as `POS` with no prompt |

Each route boots as falcon jump's does (9.3), its failure page the
station's, with the route's code as the word: `CDR failed while <stage>`.
`STATION.INI` relabels, hides, orders and names these rows as it does any
found row (Section 8).

**DevLink recovery** is a UKI for `/uki`, built by `make devlink-uki`
into `build/devlink/devlink-recovery.efi` from devlink-rescue's kernel and
initrd (`DEVLINK_IMAGES=<dir>` names another build's images; default
`~/git/home/devlink-rescue/build/classic/images`). Its row reads `DevLink
recovery`. It is a `--bios-only` UKI: only falcon-bios starts it.

When station-legacy itself was started through falcon jump's CF or USB
route, a `VBR` or `MBR` row on a drive other than the one jump serves
releases jump's resident before it hands over.

### 7.8 Updating a station in the field

Two ways, neither a reinstall (`docs/DESIGN.md` Section 12.14). From Linux
beside a build, `falcon-boot-mkstick update /dev/X` brings both halves of a
station to the build (Section 10.7). For a BIOS
station on a machine with nothing but DevLink recovery, use a station
bundle.

```sh
tools/coverage/harness.sh make station-bundle [MEMTEST=<memtest86+>]
```

builds `build/bin/falcon-station-<version>.tar.gz`. Build it in the
harness container: a static mkstick built against a host libc tuned to the
build machine will not run on an older processor, and `make
station-bundle` refuses one. The tarball holds one directory,
`station-bundle/`:

| file | what |
|---|---|
| `stage1.bin`, `stage2.bin` | the BIOS stages 1 and 2 |
| `stage3-station.bin`, `stage3-legacy.bin` | both stage 3s |
| `mod/*.MOD.gz` | station-legacy's drivers |
| `MEMTEST.BIN` | only with `MEMTEST=` |
| `falcon-boot-mkstick32` | a static i686 mkstick, which does the writing |
| `update.sh` | the update |
| `VERSION`, `MANIFEST` | the version, and every file's SHA-256 |

On the machine it needs `sh`, `tar`, `sha256sum` and mtools. From DevLink
recovery:

    devlink put falcon-station-<version>.tar.gz /tmp/ --force
    devlink exec 'cd /tmp && gunzip -c falcon-station-<version>.tar.gz |
                  tar xf - && sh station-bundle/update.sh /dev/sda --yes'

or on any Linux with the disk attached, `sh station-bundle/update.sh
<disk> [--yes]`. It checks the manifest, finds the station partition by its
boot record, prints what it will write and keep, and asks (`--yes` skips
the question):

    Update the station on <disk>? y to update, any other key to stop.

Then it writes stages 1-3 through the bundle's `mkstick bios`, the
station-legacy stage 3 and the drivers when the station partition already
holds any `*.MOD`, the plain station's stage 3 otherwise, and
`MEMTEST.BIN` when the bundle has one. It keeps the install ID,
`STATION.INI`, `/uki` and every other partition, clears a pending next-boot
marker, and ends with `station status` and:

    updated to <version>
    the sectors it replaced are in <bundle>/station-update-backup-<stamp>.bin; copy it off this machine

Undo it with `./falcon-boot-mkstick32 bios <disk> --remove --backup <that
file>`, or run the previous version's bundle. Its refusals:

| message | what to do |
|---|---|
| `the bundle does not match its MANIFEST; copy it again` | copy the tarball again |
| `<disk> has no falcon boot partition; install the station first` | install the station |
| `<disk>: partition <n>'s node (<node>) is not the partition the station's record is on; reread the partition table (partx -u <disk>) and run this again` | as it says |
| `<disk>: partition <n> is a falcon boot partition but not a station's (no STATION.INI); this bundle updates stations only` | a stick is updated with `mkstick update` |
| `nothing written` | any answer but `y` |

A bundle is BIOS only: a UEFI station's loader is signed with the MOK,
which the machine being updated does not hold.

## 8. STATION.INI reference

The station's configuration (`docs/DESIGN.md` Section 12.7):
`\EFI\falcon\STATION.INI` on the station's ESP for UEFI, `STATION.INI` in
the station partition's root for BIOS. Optional; at most 16 KB. The
installer writes one, and `station status` parses it as the loaders do.

    [general]
    default = win2k          ; a section name, or a found row's name
    timeout = 5              ; seconds; 0 or absent: the menu waits
    order   = win2k, linux   ; names, first shown first; the rest follow
    poweroff = 300           ; seconds with no key before power-off; 0 never

    [win2k]
    disk  = mbr:cdcccdcc
    part  = 1
    label = Windows 2000

### 8.1 Syntax

- One `[section]` per entry, plus `[general]` and `[totp]`. A key before
  any section is an error.
- `key = value`, one per line; spaces around both are ignored.
- `;` or `#` starts a comment anywhere on a line, so neither can appear in a
  value. Blank lines are ignored, and so is a CR before the LF.
- Keys, `[general]`, `[totp]`, and the words `yes`, `no`, `firmware`,
  `efi`, `zero` and `sha1-zero` are matched without regard to case.
- A section name is 1 to 31 characters of `A-Z`, `a-z`, `0-9`, `_` and
  `-`. At most 32 entry sections.
- Text values (`label`, `uki`, `efi`) are printable ASCII.

### 8.2 `[general]`

| key | value | absent |
|---|---|---|
| `default` | a name (8.4): a section of this file, or a found row's generated name, matched without regard to case | no default: the list waits |
| `timeout` | seconds, 0 to 3600; needs `default` | 0: no countdown |
| `order` | names, comma-separated, each once, at most 32 | the order found |
| `poweroff` | seconds with no key before the machine powers off, 0 to 86400; 0 never | 300 |

`order` shows the rows it names first, in its order, then every other shown
row as found; a name that matches no row is passed over. `Reboot to
firmware setup` stays last whatever `order` says.

In the file, `default` must name a section of the file or have the shape of
a generated name: `memtest`, `pos`, `nxt-<4 hex>`, `uki-<anything>`,
`mbr-<8 hex>[-p<n>]` or `gpt-<8 hex>[-p<n>]`, spelled as shown (a
section's name with its own case). Any other name is refused as
`default '<name>' names no entry`. station-legacy's `cd`, `usb` and `cf`
are not of that shape: name one with an empty section of that name first,
which matches the found row (8.5):

    [general]
    default = cd
    timeout = 5

    [cd]

A `default` that parses but names no row on this machine is said before the
list, which waits (7.5). One that names a hidden row gets no countdown.
When `default` names a row, the list opens with it selected on both
loaders; `timeout` adds the countdown on it, and without one the list waits
and Enter boots it.

### 8.3 Entry sections

| key | value | absent |
|---|---|---|
| `disk` | `mbr:<8 hex>` (the disk signature) or `gpt:<DiskGUID>` (the whole GUID) | |
| `part` | a partition number, 1 to 9999; needs `disk` | BIOS: the disk's own MBR; UEFI: any partition of the disk |
| `uki` | a UKI's file name on the shelf, up to 127 characters | |
| `efi` | a path on an ESP, up to 127 characters, `\` or `/`; with `disk`, that disk's ESP, else the station's own | |
| `method` | `firmware` or `efi`: how a UEFI Windows is started, through its firmware entry or its ESP's `bootmgfw.efi` | `firmware` |
| `swap` | `yes` or `no`: BIOS, the drive swap of a chain of a disk that is not 0x80 | `yes` |
| `label` | the row's text, up to 63 characters | the found row's own, or as 8.5 says |
| `hide` | `yes` or `no` | `no` |

`efi` and `method` are UEFI's. On BIOS a section that has either is left
out, whatever else it says.

### 8.4 Names

Every row the loader finds has a generated name; a row a section matches
or adds also takes the section's name. `default`, `order` and next boot
take either, without regard to case, hidden rows included.

| name | row |
|---|---|
| `pos` | `Personal OS (encrypted)` (on BIOS, the first encrypted shelf's) |
| `uki-<stem>` | a UKI by file name, `.efi` left off; any character outside `A-Za-z0-9_-` becomes `-`, and the name is cut to 31 characters |
| `nxt-<4 hex>` | UEFI: firmware entry `Boot####`, lower-case hex |
| `mbr-<signature>`, `mbr-<signature>-p<n>` | a disk of that signature, or its partition n |
| `gpt-<8 hex>`, `gpt-<8 hex>-p<n>` | a GPT disk by its DiskGUID's first eight hex digits, or its partition n |
| `memtest` | `Memory test (memtest86+)` |
| `cd`, `usb`, `cf` | station-legacy: `CD drive`, `USB stick`, `CF card (PC Card)` |

`Reboot to firmware setup` has no name, and no section changes it.

### 8.5 What a section does

The first rule that applies:

| section has | UEFI | BIOS |
|---|---|---|
| `uki` | the plaintext shelf's row for that file; else a new row that prompts for the encrypted shelf and boots that file from it, labelled with the file name | the same |
| `disk` with `method = efi`, or `disk` with `efi` | the ESP row on that disk (and partition) for that path (`\EFI\Microsoft\Boot\bootmgfw.efi` without `efi`); else a new one, looked for when chosen, labelled `Windows Boot Manager (disk ..., partition n)` or with the path | left out |
| `disk` otherwise | the firmware entry on that disk (and partition); else the ESP row found there; else a new `NXT` row labelled with the section's name, which says when chosen that no firmware entry names the disk | with `part`: that partition's row, else a new `VBR` row labelled `Partition (disk ..., partition n)`; without: that disk's row, else a new `MBR` row labelled `Boot code (disk ...)`; a new row's disk and partition are looked for when it is chosen |
| `efi` alone | that path on the station's own ESP, a new row unless one is there, labelled with the path | left out |
| none of these | the found row whose generated name is the section's name | the same |

The matched or added row takes the section's `label` and `hide` when
given, and on BIOS its `swap`. A section that matches no row and adds none
is left out. To keep one prompt on the list when a `uki` section sets the
default:

    [general]
    default = linux
    timeout = 5

    [linux]
    uki = gentoo.efi

    [pos]
    hide = yes

### 8.6 `[totp]`

Written by `station totp enroll` and `reseal` (7.6); UEFI only.

| key | value |
|---|---|
| `tpm` | `2.0` or `1.2` |
| `index` | the seed's NV index: `0x` and 1 to 8 hex digits |
| `pcrs` | what the seed is sealed to: PCR numbers 0 to 15, comma-separated, each once |
| `key` | the pinned TPM key in hex: on 2.0 the SRK's name, 34 bytes starting `000b`; on 1.2 the SRK's modulus, 128 to 256 bytes |
| `label` | the authenticator's account name, up to 63 characters; shown after the code |
| `srkauth` | 1.2 only: `zero` or `sha1-zero`, the form of the SRK's well-known secret |

`tpm`, `index`, `pcrs` and `key` are required.

### 8.7 Errors

The file is strict. The first error ends the parse: the entry sections
before it still apply, and the result has no default, no countdown, the
default power-off and no `[totp]`. The station says it before the list
(7.5) as `STATION.INI line <n>: <reason>.`; `station status` prints the
same line. Quoted text in a reason is cut at 24 characters.

| reason | |
|---|---|
| `a section header that is not [name] '<line>'` | |
| `section '<name>' is not a name ([A-Za-z0-9_-], up to 31)` | |
| `section '<name>' is given twice`, `section [general] is given twice`, `section [totp] is given twice` | |
| `more than 32 entry sections` | |
| `a line that is not key=value '<line>'` | |
| `key '<key>' is outside a section` | |
| `unknown key '<key>'` | a key the section does not take |
| `<key> is given twice` | |
| `default '<value>' is not a name` | |
| `default '<value>' names no entry` | at the `default` line, checked at the end (8.2) |
| `timeout '<value>' is not a number of seconds up to 3600` | |
| `timeout is set but default is not` | a nonzero timeout |
| `poweroff '<value>' is not a number of seconds up to 86400` | |
| `order names nothing`, `order entry '<name>' is not a name`, `order names '<name>' twice`, `order names more than 32 entries` | |
| `disk '<value>' is not mbr:<8 hex> or gpt:<GUID>` | |
| `part '<value>' is not a partition number` | |
| `part is given without disk` | at the section's header line |
| `uki '<value>' is not a path of up to 127 characters`, `efi '<value>' is not a path of up to 127 characters` | |
| `method '<value>' is not firmware or efi` | |
| `swap '<value>' is not yes or no`, `hide '<value>' is not yes or no` | |
| `label '<value>' is not text of up to 63 characters` | |
| `tpm '<value>' is not 2.0 or 1.2` | |
| `index '<value>' is not 0x and up to 8 hex digits` | |
| `pcrs '<value>' is not PCR numbers 0 to 15, each once` | |
| `key '<value>' is not hex of up to 256 bytes` | |
| `srkauth '<value>' is not zero or sha1-zero` | |
| `[totp] needs tpm, index, pcrs and key`, `[totp] key is not a key that TPM version has`, `[totp] srkauth is for a TPM 1.2` | at the `[totp]` header line |

## 9. falcon jump

falcon jump (`docs/DESIGN.md` Section 13) is a 1.44 MB boot floppy for an
old BIOS machine: it boots the machine's disks, memtest86+, and, through
drivers read off the floppy, the disc in an IDE CD drive, a falcon stick on
USB and a CompactFlash card in a PC Card slot. It is one image for every
i586 or later processor.

### 9.1 Making the floppy

```sh
make jump [MEMTEST=<memtest86+>] [JUMP_INI=<file>]
dd if=build/bin/falcon-jump.img of=/dev/fd0
```

`build/bin/falcon-jump.img` is a FAT12 volume labelled `FALCON JUMP`, with
jump's boot sector, stages 2 and 3 in its reserved sectors, and these
files:

| file | what | on the floppy |
|---|---|---|
| `JUMP.INI` | the default and countdown (9.2) | with `JUMP_INI=` |
| `ATA.MOD` | the CD drive route's driver | always |
| `EHCI.MOD`, `UHCI.MOD`, `OHCI.MOD` | the USB stick route's drivers, one per host controller type | always |
| `PCC.MOD` | the CF card route's driver | always |
| `MEMTEST.BIN` | memtest86+ | with `MEMTEST=` |

`MEMTEST=` has to be a build carrying the x86 boot protocol header
(memtest86+ 6 and later), and the 32-bit build for a processor with no
64-bit mode. Building needs mtools and `falcon-boot-mkstick`; no root.

After `dd` the floppy is an ordinary FAT12 volume: change its files with
`mcopy` or by mounting it. A driver deleted from it takes its route off the
menu. The drivers are stored compressed and checked as they are read;
copy them only from the same build as the floppy's boot sector.

### 9.2 JUMP.INI reference

Optional, at the floppy's root, at most 4096 bytes.

    default=hdd        ; hdd, memtest, cd, usb or cf
    timeout=5          ; seconds; any key stops the countdown

| key | value | absent |
|---|---|---|
| `default` | `hdd` (the first disk row), `memtest`, `cd` (the CD drive row), `usb` (the USB stick row) or `cf` (the CF card row) | no default |
| `timeout` | seconds, 0 to 3600; a nonzero one needs `default` | 0: no countdown |

Keys and values are matched without regard to case. `;` or `#` starts a
comment anywhere on a line; blank lines are ignored, and so is a CR before
the LF. A default whose row is not on the menu is no default, and nothing
is said.

The file is strict. The first error makes the result no default and no
countdown, and is said before the menu:

    JUMP.INI line 3: unknown key 'tmeout'.

    The menu will wait with no default.

    press any key to continue.

| reason |
|---|
| `a line that is not key=value '<line>'` |
| `unknown key '<key>'` |
| `default is given twice`, `timeout is given twice` |
| `default '<value>' is not hdd, memtest, cd, usb or cf` |
| `timeout '<value>' is not a number of seconds up to 3600` |
| `timeout is set but default is not` |
| `the file could not be read` (line 1: larger than 4096 bytes, or a read error) |

Quoted text in a reason is cut at 24 characters.

### 9.3 The menu and its routes

The menu lists, in this order:

| row | code | route | there when |
|---|---|---|---|
| `Disk 0x80`, `Disk 0x81`, ... | `MBR` | the disk's LBA 0 at 0x7C00 with DL naming that disk; nothing resident but the USB keyboards, when a `USB` route left them up | the BIOS counts the disk and its LBA 0 carries the boot signature |
| `CD drive` | `CDR` | the disc in the first drive on the IDE channels that has one, through `ATA.MOD`: each PCI IDE controller's channels, in native or compatibility mode | the floppy holds `ATA.MOD` and an IDE controller is on the PCI bus |
| `CF card (PC Card)` | `PCC` | the first CompactFlash card in a PC Card slot, through `PCC.MOD`: its own MBR, whatever is on it; a falcon card as the next BIOS disk, any other as drive 0x80 behind the drive swap | the floppy holds `PCC.MOD` and a CardBus bridge is on the bus |
| `USB stick` | `USB` | the first falcon stick (a partition carrying a `boot` record) on a root port of any controller, or behind up to two hubs on one, through `EHCI.MOD`, `UHCI.MOD` and `OHCI.MOD`, as the next BIOS disk; the stick's own stages take it from there | the floppy holds a USB driver and its controller is on the bus |
| `Memory test (memtest86+)` | `MEM` | `MEMTEST.BIN`, started through the x86 boot protocol | the floppy holds `MEMTEST.BIN` |

With no row the list reads `(no disk with boot code, and no MEMTEST.BIN)`.
Keys are `up/down select | enter choose | esc back`; Escape leaves for the
firmware (`falcon-boot: returning to firmware.`, INT 18h, then a reset).
The countdown is the first menu's only. A page left with no key for five
minutes powers the machine off, through APM where the BIOS has it.

`CDR`, `PCC` and `USB` keep their driver resident at 15 MB, so they need
RAM at 15-16 MB, which is 16 MB of memory. A no-emulation disc (every
Linux, BSD and Windows 2000-on disc) is served by the stick's virtual CD
as drive 0xE0. A floppy-emulation disc (DOS and Win9x boot discs, vendor
utility CDs) is read into memory above 64 MB and served as a floppy, so it
needs more than 64 MB, and nothing of the CD driver stays. A disc that asks
for hard-disk emulation is refused. `PCC` reads the card's configuration
through 4 KB of memory between 0xC8000 and 0xDFFFF that no option ROM
uses, chosen when the route starts and named on serial; the card's
registers sit at the first of I/O 0x1E0-0x1EF, 0x1C0-0x1CF, 0x1A0-0x1AF
and 0x180-0x18F that nothing on the machine answers at, named on serial
too, and a slot whose registers the BIOS never placed on the bus (a
second socket on some machines) is given an address by the route, named
on serial. `USB` loads every USB driver on
the floppy, and a USB keyboard works once the route has started, on a root
port or behind up to two hubs
(a hub inside the keyboard counts as one), its lock LEDs following
Caps, Num and Scroll Lock, for the stick's loader and
whatever it starts that reads the keyboard through the BIOS; DOS under
EMM386 does not see it. A hub or a keyboard on a USB 2.0 (EHCI) port is
handed to that port's USB 1.1 companion controller, where the hub runs at
full speed. When the route comes back to jump's menu, the USB keyboard
stays up: it answers the failure page and the menu, and whatever the menu
starts next that reads the keyboard through the BIOS (a disk, a CD, a CF
card). Memory test and Escape let it go, as does station-legacy when it
starts a system on another disk; after that only a PS/2 keyboard answers
until a reset.
A card or stick with no falcon record on it (DOS, Windows 9x, any other
system's MBR) boots as drive C: (0x80), and for that boot the machine's own
first disk is 0x81; a falcon card or stick boots as the disk after the
machine's own, and the numbers stay as they are. DOS on the card reads it
only in real mode: with EMM386 or another memory manager loaded, reads of
C: fail.

### 9.4 Jump's pages

A route's page starts with its code and `booting: <name>` (`Disk 0x80`,
`CD`, `CF card`, `USB stick`, `memtest86+`). `CDR`, `PCC` and `USB` then
say each step as it starts (`loading ATA.MOD...`, `looking for a CD
drive...`), and name the device they found. A route that comes back draws:

    CD
    boot failed while looking for a CD drive

    there is no CD drive on the IDE channels.

    press any key to return to the menu.

| stage | advice |
|---|---|
| `loading ATA.MOD`, `loading PCC.MOD`, `loading EHCI.MOD`, `loading UHCI.MOD`, `loading OHCI.MOD` | `this machine's memory map has no RAM at 15-16 MB, where falcon jump's drivers run.`, `the driver is not on the floppy.`, `the driver could not be read off the floppy.`, `the driver did not read back as it was written, twice. The floppy may be failing.`, `the file is not a falcon jump module.`, `the module is from another build of falcon jump. Copy the floppy's modules from the same build as its boot sector.`, `the module is built for another slot; it may have been copied under the wrong name.`, `the module is cut short on the floppy, or does not fit its slot.`, `the driver would not start.` |
| `looking for a CD drive` | `there is no CD drive on the IDE channels.`, `there is no disc in the CD drive.` |
| `reading the disc's boot catalog` | `the disc has no El Torito boot catalog.`, `the disc boots only on UEFI.`, `the disc's boot image asks for hard-disk emulation, which this loader does not do.` |
| `reading the floppy image's geometry` | `the boot image is a floppy type this loader does not know.` |
| `reserving memory for the floppy image` | `this machine has too little memory above 64 MB to hold the floppy image.`, `there is not enough memory to hold the floppy image.` |
| `reading the floppy image` | `the floppy image could not be read off the medium.` |
| `installing the CD driver` | `this machine will not give up the conventional memory the CD driver needs.`, `the CD driver did not answer through INT 13h.` |
| `reserving memory for the virtual floppy`, `reserving memory for the virtual CD` | `this machine will not give up the conventional memory the virtual floppy needs.`, `... the virtual CD needs.` |
| `bringing up the PC Card slots` | `there is no CompactFlash card in a PC Card slot.` |
| `looking for a falcon stick` | `there is no USB storage on the ports.`, `none of the USB storage is a falcon stick.` |
| `installing the driver` | `this machine will not give up the conventional memory the driver needs.`, `the driver did not answer through INT 13h.` |
| `reading the disk's first sector` | `The disk did not answer a read.` |
| `checking the boot signature` | `Its first sector carries no boot code.` |
| `opening MEMTEST.BIN`, `starting memtest86+` | the kernel loader's reason |

On station-legacy the same stages and advice name the disk, not the
floppy (`the driver is not on the disk.`).

Before stage 2 runs, jump's boot sector can stop, on screen and serial,
with

    falcon-bios: read error
    press a key

or `falcon-bios: no stage 2`: the floppy could not be read, or its reserved
sectors do not hold this build's stage 2. Write the image again. A key
reboots. A floppy whose volume will not mount still boots its disks; the
reason goes to serial only.

## 10. falcon-boot-mkstick

The stick maker (`docs/DESIGN.md` Section 8). Its commands are `bios`,
`create`, `ident`, `kdf-bench`, `keygen`, `next-boot`, `station`,
`status`, `update` and `varstore`. `falcon-boot-mkstick <command> --help` prints a command's
options, and a usage error prints the same help after the error. Every
command takes `--verbose` (every command it runs is shown) and `--quiet`
(warnings and errors only). It exits 0 on success and 1 on any failure.
`make install` puts it on `PATH` (Section 13.2).

The defaults below that name `build/bin/` and `build/shim/bins/` are a tree
build's, read from the repository root. A release's mkstick reads the same
files under `/usr/lib/falcon-boot/bin/` and `/usr/lib/falcon-boot/shim/bins/`
(Section 14), and `--help` prints the defaults it was built with.

Sizes (`create`) are a number with an optional unit: `K`, `M`, `G` or `T`,
powers of 1024, case-insensitive, with `iB` or `B` after it accepted
(`64M`, `64MiB` and `64mb` are the same). A partition size is rounded down
to a whole MiB.

A passphrase file (`--passphrase-file`, every command that takes one) is
read up to its first newline or carriage return; the passphrase is
printable ASCII and at most 511 characters. Without the option the
passphrase is read from `/dev/tty` with echo off.

### 10.1 create

    falcon-boot-mkstick create <target> [--force] [--size <size>]
        [--p1-size <size>] [--iso-size <size|rest>] [--p3-size <size>]
        [--p4-size <size|rest>] [--passphrase-file <file>]
        [--shim <dir>] [--loader <file>] [--loader-aa64 <file>]
        [--uki <file>]... [--cn <name>]
        [--key <file> --cert <file>] [--cert-out <file>] [--key-out <dir>] [--p3-plain]
        [--p2-fs <fs>] [--p3-fs <fs>] [--stage3 <file>]

Makes the four-partition stick of DESIGN Section 3 on a block device or an
image file. Needs root. Before writing anything it checks that `sgdisk`,
`mkfs.fat`, `mkfs.exfat`, `mkfs.ext4`, `cryptsetup`, `losetup`, `mount`,
`umount`, `openssl` and `sbsign` are on `PATH`, that the loader and shim
exist, and that every `--uki` exists.

| option | default | what it does |
|---|---|---|
| `--force` | off | required for a block device; also overwrites a target that already carries a partition table |
| `--size` | none | creates the image file at this size when `<target>` does not exist; ignored, with a warning, when it does |
| `--p1-size` | `64M` | p1, the ESP |
| `--iso-size` | half of what is left | p2, the exFAT ISO library; `rest` takes everything p4 does not |
| `--p3-size` | `1G` | p3, the UKI shelf |
| `--p4-size` | the rest | p4, LUKS2 root and home; `rest` takes everything p2 does not |
| `--passphrase-file` | prompt | the p3 and p4 passphrase; the prompt is `passphrase for p3 and p4: ` then `repeat: ` |
| `--shim` | `build/shim/bins` | the directory holding shim and MokManager (Section 13.6) |
| `--loader` | `build/bin/falcon-boot.efi` | the loader to sign and install |
| `--loader-aa64` | `build/bin/falcon-boot-aa64.efi` | the AArch64 loader, staged beside the x64 one with shimaa64 from `--shim`. Without this option, a missing loader or shim leaves the stick x86-64 only, with a warning; given, it is required |
| `--uki` | none | a UKI to sign with the MOK and put in p3's `/uki`; repeatable; two with the same basename are refused |
| `--cn` | `falcon-boot MOK` | the new MOK certificate's subject CN; refused with `--key` |
| `--key`, `--cert` | a new MOK | an existing MOK private key and its certificate, both PEM, instead of a new pair: a replacement stick keeps the MOK the machines already enrolled. The key must be the certificate's, checked before anything is written. The first boot needs no enrollment where that certificate is enrolled |
| `--cert-out` | none | also writes the public certificate here, PEM |
| `--key-out` | none | also writes `falcon-mok.key` (0600) and `falcon-mok.crt` into this directory, made 0700 if new |
| `--p3-plain` | off | p3 without LUKS; requires `--key-out` |
| `--p2-fs` | `exfat` | p2's filesystem; `exfat` is the only one accepted |
| `--p3-fs` | `ext4` | p3's filesystem; `ext4` is the only one accepted |
| `--stage3` | `build/bin/falcon-bios-stage3.bin` | the BIOS stage 3 p1's reserved sectors are sized for |

With neither `--iso-size` nor `--p4-size`, what is left after p1 and p3 is
halved. One of them given leaves the rest to the other; both given are
taken as they are. The floors are p1 16 MiB, p2 8 MiB, p3 48 MiB and p4
24 MiB, and a layout below one is refused with the size it needs.

What it makes:

| partition | type, label | contents |
|---|---|---|
| p1 | ESP, `FALCON-ESP`, FAT32 | `/EFI/BOOT/BOOTX64.EFI` (shim), MokManager under the name shim launches it by, the loader under shim's next-stage name; the same for AArch64 (`BOOTAA64.EFI`, `mmaa64.efi`, `grubaa64.efi`); `/falcon-boot.cer` |
| p2 | Microsoft basic data, `FALCON-ISO`, exFAT | `/falcon/blessed/`, `/.falcon` |
| p3 | falcon shelf, `FALCON-UKI`, LUKS2 and ext4 | `/uki/` with each `--uki`, `/falcon-mok.key` (0600), `/falcon-mok.crt` |
| p4 | Linux LUKS, `FALCON-ROOT`, LUKS2 and ext4 | empty |

p3's keyslot is argon2id at 32768 KiB, time 4, 4 lanes, `aes-xts-plain64`
with a 512-bit key. p4 takes cryptsetup's own argon2id cost. Both share the
one passphrase. With `--p3-plain`, p3 is ext4 alone and holds `/uki/`,
`/falcon-mok.crt` and `/.falcon`, with no private key; the loader asks for
no passphrase and the initramfs prompts for p4 itself.

One random 128-bit install ID goes into every partition's identity record
(DESIGN Section 3.1):

| partition | record |
|---|---|
| p1 | role `boot`, in its boot sector at byte 0x1DC and in the FAT32 backup boot sector; stage 3 at sector 32, the marker at sector 8 |
| p2 | `/.falcon`, role `library` |
| p3, LUKS2 | a `falcon` token in the header, role `shelf` (`cryptsetup token export --token-id 0 /dev/X3` shows it) |
| p3, `--p3-plain` | `/.falcon`, role `shelf` |
| p4 | none |

p1 is formatted with `mkfs.fat -R <n>`: 32 sectors, then room for BIOS
stage 3 of twice the size of `--stage3`, rounded up to a whole MiB and
never less than 1 MiB. The stick's stage 3 is about 200 KB, so the room is
1 MiB (2080 reserved sectors in all). A later stage 3 up to that size
installs without reformatting p1. With no stage 3 built, the 1 MiB floor is
used. `create` does not install the BIOS stages; `bios` (10.2) does.

The MOK pair is generated by `openssl`: RSA 4096, valid 3650 days. Each
architecture's loader and each `--uki` are signed with it by `sbsign`; shim
and MokManager are copied as they are (Section 11.1). The shim for each
architecture is `shim<arch>.efi` or `fedora-16.1-shim<arch>.efi` in
`--shim`, and is refused when its PE machine is another's. Fedora's
shimaa64 is signed by the Microsoft UEFI CA 2023 alone: a machine whose db
holds only the 2011 CA refuses it, and needs the 2023 CA enrolled, or
Secure Boot off.

A target that already carries an MBR or a GPT partition table, or a table
that does not read cleanly, is refused without `--force`. A block device is
refused without `--force`, after its model and size are printed, and
refused while any of its partitions is mounted.

It ends:

    <target> is ready (install ID <32 hex digits>).
    First boot with Secure Boot ON: shim launches MokManager; enroll p1:/falcon-boot.cer, reboot.

### 10.2 bios

    falcon-boot-mkstick bios <target> [--backup <file> | --no-backup]
        [--reserve] [--remove] [--dry-run] [--force]
        [--stage1 <file>] [--stage2 <file>] [--stage3 <file>]

Installs falcon-bios on a disk whose `boot` partition exists (DESIGN
Section 3.3). Partitions, filesystems and files are not touched. It finds
the boot partition the way stage 2 does: the first ESP- or
falcon-boot-typed partition (GPT) or FAT-typed one (MBR) whose boot sector
carries a `boot` record naming a stage 3 sector (an ESP holding a UEFI
station's record, stage 3 at 0, is passed over). A disk with none is
refused; `create` makes one, and `ident --write-boot` (10.4) stamps an
existing FAT partition. It writes:

| what | where |
|---|---|
| stage 1 | LBA 0, bytes 0-439, with stage 2's first LBA (LE32 at 0x1B2) and length in sectors (LE16 at 0x1B6); the disk signature, table and `0x55AA` are kept |
| stage 2 | LBA 34 on GPT, LBA 1 on MBR, its real length; it must end before the first partition |
| stage 3 | the boot partition's reserved sectors, from the sector its record names; it must end inside them |
| the marker sector | the one the record names, zeroed |

| option | default | what it does |
|---|---|---|
| `--stage1` | `build/bin/falcon-bios-stage1.bin` | the 512-byte MBR image |
| `--stage2` | `build/bin/falcon-bios-stage2.bin` | stage 2 |
| `--stage3` | `build/bin/falcon-bios-stage3.bin` | stage 3 |
| `--backup` | `falcon-bios-backup-<target>.bin` in the current directory | where every sector it writes is saved first, and what `--remove` restores from |
| `--no-backup` | off | saves nothing |
| `--remove` | off | undoes an install |
| `--reserve` | off | GPT only: also claims stage 2's gap as a BIOS boot partition (type EF02, slot 5, named `FALCON-BIOS`); rewrites the partition table |
| `--dry-run` | off | prints the plan and where the MOK comes from, and writes nothing |
| `--force` | off | overwrites an existing backup file, and allows `--reserve` on a disk with mounted partitions |

It takes disks of 512- and 4096-byte sectors and refuses any other ("the
disk's sectors are neither 512 nor 4096 bytes"). It refuses an existing
backup file without `--force`, `--reserve` on an MBR disk, a stage 2 that
would reach the first partition, and a stage 3 that does not fit the
reserved sectors, for which it prints the `mkfs.fat -R` count that would
fit. Boot code in LBA 0 that is not falcon's is replaced with a warning
(the backup holds it).

`--remove` restores the backup file, LBA 0 last; with no backup file it
clears what an install wrote (the stage 2 run LBA 0 names, stage 3 by its
header's length, the marker, LBA 0's boot code). It also deletes a
`FALCON-BIOS` partition in whatever slot it is, unless the disk has
mounted partitions.

### 10.3 status

    falcon-boot-mkstick status <target> [--no-esp]

Read-only. Reads the table (MBR or GPT) and every partition's record with
the loaders' own code, and reports one line per check, `OK`, `WARN`,
`FAIL` or `--` (skipped or informational):

- the install ID, from the boot partition's record; a partition carrying a
  different install ID, or a record that does not decode, is a FAIL, and a
  partition with none is listed as such;
- the ISO library (p2's files), the shelf (its keyslots, or a plaintext
  shelf's contents), and a LUKS2 volume with no record (the root);
- the BIOS stages where the loaders look for them: stage 2 at the LBA stage
  1 names, stage 3 at the sector the boot record names, and whether a
  next-boot marker is pending;
- the ESP's files, through a read-only loop mount, which needs root
  (`--no-esp` skips it).

It ends with `<n> checks passed, <n> warnings, <n> failures, <n> skipped`
and exits nonzero when anything fails.

### 10.4 ident

    falcon-boot-mkstick ident <target>
    falcon-boot-mkstick ident <target> --write-boot <n> --id <hex|new>
    falcon-boot-mkstick ident <target> --write-boot-at <byte>
        --stage3-sector <n> --marker-sector <n> --id <hex|new>
    falcon-boot-mkstick ident --reserved-sectors [--stage3 <file>]

With a target alone, prints one line per partition, tab-separated: number,
role (`boot`, `library`, `shelf`; `-` for none; `bad` for a record that
does not decode), install ID (`-` for none) and first LBA. Scripts find a
partition by its role this way (`tools/rig-part.sh` does). No root is
needed on an image file.

`--write-boot <n>` writes the `boot` record (stage 3 at sector 32, the
marker at sector 8) into partition n's FAT boot sector and its FAT32
backup, and prints the install ID. The partition must be ESP-,
falcon-boot- or FAT-typed and formatted with at least that many reserved
sectors.

`--write-boot-at <byte>` writes the `boot` record into the FAT boot sector
at that byte offset of an image with no partition table (jump's floppy),
with `--stage3-sector` and `--marker-sector` relative to the volume
(`--marker-sector 0` for none). Both are required with it, and it cannot be
combined with `--write-boot`.

`--id` is 32 hex digits, not all zero, or `new` for a random one. It is
accepted only with `--write-boot` or `--write-boot-at`.

`--reserved-sectors` prints the `mkfs.fat -R` count of 10.1 for `--stage3`
(default `build/bin/falcon-bios-stage3.bin`).

The other two records are text: a `/.falcon` file holding
`falcon 1 <role> <32 hex digits>`, and the LUKS2 token of 10.1.

### 10.5 next-boot

    falcon-boot-mkstick next-boot [--sector] [--disk <dev>] <value>
    falcon-boot-mkstick next-boot [--sector] [--disk <dev>] --clear
    falcon-boot-mkstick next-boot [--sector] [--disk <dev>] --print

Asks the loader to start one entry on the next boot instead of drawing the
menu (DESIGN Sections 6.3.1 and 12.8). Needs root, or write access to the
disk. The value is printable ASCII, at most 255 characters:

| loader | value |
|---|---|
| stick, both loaders | a path on p2 as the menu lists it, or `:personal` for the personal OS |
| station, both loaders | a row's name: a STATION.INI section name, or a generated one (`pos`, `memtest`, `uki-<stem>`, `nxt-<4 hex>`, `mbr-<8 hex>[-p<n>]`, `gpt-<8 hex>[-p<n>]`; on station-legacy `cd`, `usb`, `cf`), matched without regard to case |

On a station whose shelf is encrypted, `uki-<stem>` that no row carries is
`<stem>.efi` on that shelf: the passphrase prompt, then that UKI. That is
how a system asks to come back to itself.

The loader clears the marker before acting on it and matches it against
what it finds, as a menu pick would be.

- **UEFI** (the machine booted with UEFI: `/sys/firmware/efi` exists): sets
  the `FalconNextBoot` variable (vendor GUID
  `50614485-081b-4b7c-8b54-2abeffdf2fb3`; non-volatile, boot service and
  runtime access) through efivarfs. `--clear` deletes it. `--disk` without
  `--sector` is refused.
- **BIOS** (no `/sys/firmware/efi`, or `--sector`): writes the marker
  sector of the disk's `boot` partition, at the sector its record names:

      falcon-next-boot 1
      <the RTC's reading, /sys/class/rtc/rtc0/since_epoch>
      <the value>

  and zeros to the end of the sector. The marker is one whole sector of the
  disk: 512 bytes, or 4096 on a 4Kn disk, the text in its first 512. The
  loader honours it within ten minutes after it was written, or up to two
  minutes before (a clock that went back). Without `--disk` every disk is
  looked at and exactly one must carry a `boot` partition; with more than
  one it lists them and asks for `--disk`. With `--disk <dev>` (a disk or
  an image file) only that disk is looked at, whatever else is attached,
  and it is an error if it has no `boot` partition. The sector is written
  only when it is empty or already a marker; anything else is left alone
  and reported. `--clear` zeros it.

`--print` reports what is queued and changes nothing. It writes one
`key=value` per line to stdout and exits 0 whether or not anything is
queued; it exits 1 only on an error (no boot partition, a disk it cannot
read). Keys may be added later; a script reads the ones it wants and
ignores the rest. For the variable (UEFI):

| key | value |
|---|---|
| `source` | `efivar` |
| `queued` | `yes`, `no`, or `invalid` (set, but not a value the loader accepts; it deletes it and draws the menu) |
| `value` | the value; only with `queued=yes` |

For the sector (BIOS, or `--sector`):

| key | value |
|---|---|
| `source` | `sector` |
| `disk` | the disk it was read from |
| `lba` | the marker's LBA, in the disk's sectors |
| `sector_size` | the disk's sector size, `512` or `4096` |
| `queued` | `yes`, `no` (all zeros, or not a marker, which the loader leaves alone), or `invalid` (a marker the loader clears and does not act on) |
| `value` | the value; this and the keys below only with `queued=yes` |
| `set_rtc` | the RTC's reading when it was set, seconds |
| `age` | seconds since then by the RTC now; negative when the clock went back |
| `window` | `yes` when the loader would honour it now (within ten minutes, or up to two minutes in the future), else `no` |

`age` and `window` are `unknown` when `/sys/class/rtc/rtc0/since_epoch`
cannot be read. For example:

    $ falcon-boot-mkstick next-boot --print --sector --disk /dev/sdb
    source=sector
    disk=/dev/sdb
    lba=2056
    sector_size=512
    queued=yes
    value=:personal
    set_rtc=1790495324
    age=37
    window=yes

**Pages for a marker the loader does not act on.** Each is drawn instead
of the menu, and any key goes on to the menu. "both" is both the UEFI and
the BIOS loader of that build; "BIOS, both" is the BIOS stick and the BIOS
station. Every one but the last ends
`It has been cleared; nothing was booted from it.` and then
`press any key for the menu.`

| loader | line | when |
|---|---|---|
| stick, both | `note: the next-boot marker held something that is not a path on this stick.` | the value does not decode |
| stick, both | `the next-boot marker held a reserved value this loader does not know.` | a `:` name other than `:personal` |
| stick, BIOS | `the personal OS the next-boot marker names is not on this stick.` | `:personal` with no shelf in sight |
| stick, both | `the ISO this boot was supposed to start is not on the stick any more.` | no p2 entry has that path |
| station, both | `the next-boot marker held something that is not an entry name.` | the value does not decode |
| station, both | `the next-boot marker names <value>, and no entry has that name.` | no row has that name |
| BIOS, both | `note: the next-boot marker was not set within the last ten minutes.` | outside the window |
| BIOS, both | `note: this machine's clock did not answer, so the next-boot marker could not be dated.` | the RTC did not answer |
| BIOS, both | `note: the next-boot marker could not be cleared, so nothing was booted from it.` | the sector would not clear and stays on the disk; the page ends `press any key for the menu.` alone |

`:personal`, and a station row behind the prompt, start at the passphrase
prompt; Escape there goes to the menu.

### 10.6 station

    falcon-boot-mkstick station install <disk> [--bios] [--uefi]
        [--arch x64|aa64] [--size <MiB>] [--shelf <n|disk:n|partition>] [--esp <n>]
        [--memtest <file>] [--memtest-efi <file>] [--key <file> --cert <file>]
        [--passphrase-file <file>] [--legacy [--uki <file>]... [--mods <dir>]]
        [--also <disk>]... [--no-scan] [--backup <file>] [--force] [--dry-run]
        [--shim <dir>]
        [--stage1 <file>] [--stage2 <file>] [--stage3 <file>] [--loader <file>]
    falcon-boot-mkstick station remove [<disk>] --backup <file> [--full]
        [--arch x64|aa64] [--force] [--dry-run]
    falcon-boot-mkstick station status <disk> [--also <disk>]... [--no-scan]
        [--arch x64|aa64]
    falcon-boot-mkstick station totp enroll|reseal|remove|check <disk> ...

Section 2 gives the install procedure; this is the reference.

#### install

Installs the station on a machine's disk (DESIGN Section 12.9), in space
that is already free; nothing is resized. One new install ID is drawn and
written into every record the station has: the station partition's, the
ESP's and the shelf's.

| option | default | what it does |
|---|---|---|
| `--bios`, `--uefi` | both, where the disk allows | which station to install |
| `--arch` | this machine's | the machine the station boots: `x64`, or `aa64`, whose station is UEFI only and whose names are `falconaa64.efi`, `BOOTAA64.EFI` and `shimaa64.efi`; `--bios` and `--legacy` are refused with it |
| `--size` | 64 (288 on a 4Kn disk) | the station partition's size in MiB, at least 40 (272 on a 4Kn disk), at most 4096 |
| `--shelf` | none: the station offers no Linux | the shelf: `n` is partition n of `<disk>`, `disk:n` partition n of another disk, anything else a partition node; it is stamped with the install ID |
| `--esp` | the disk's ESP | the ESP's partition number |
| `--memtest` | none | memtest86+ (x86 boot protocol build) for the BIOS station, as `MEMTEST.BIN` |
| `--memtest-efi` | none | memtest86+'s `.efi` for the UEFI station, signed with the MOK, as `memtest.efi` |
| `--key`, `--cert` | the shelf's | the MOK private key and its PEM certificate; given together |
| `--passphrase-file` | prompt | unlocks a LUKS2 shelf |
| `--legacy` | off | station-legacy: its stage 3, jump's drivers and `--uki` in `/uki` |
| `--uki` | none | with `--legacy` only: a UKI for the station partition's `/uki`; repeatable |
| `--mods` | `build/bin/mod` | with `--legacy`: the drivers' directory |
| `--also` | none | another disk or image of the same machine, scanned for systems; repeatable |
| `--no-scan` | off | scans only `<disk>` and `--also`, not every disk of this machine |
| `--backup` | `falcon-station-backup-<disk>.bin` | where everything it changes is saved first |
| `--force` | off | overwrites an existing backup file |
| `--dry-run` | off | prints the plan and STATION.INI, writes nothing |
| `--stage1` | `build/bin/falcon-bios-stage1.bin` | |
| `--stage2` | `build/bin/falcon-bios-stage2.bin` | |
| `--stage3` | `build/bin/falcon-bios-station-stage3.bin` (`falcon-bios-station-legacy-stage3.bin` with `--legacy`) | |
| `--loader` | `build/bin/falcon-boot-station.efi` | the UEFI station's loader |
| `--shim` | none: no shim, and the firmware entry names the jumper at `\EFI\BOOT\BOOTX64.EFI`, which the firmware's db must trust (Section 11.1) | UEFI through shim: shim and its MokManager from this directory (`build/shim/bins` is where `fetch-shims.sh` puts them) |

The UEFI station needs the MOK: `--key` and `--cert`, or `--shelf` to read
the shelf's (a LUKS2 shelf asks for its passphrase). With no `--shelf` it
notes each partition that looks like a shelf and the `--shelf disk:n` that
would name it. A locked shelf's UKIs are rows at boot; `--passphrase-file`
lets STATION.INI name one as the default.

**BIOS.** What it writes:

| what | MBR disk | GPT disk |
|---|---|---|
| station partition | a free primary slot, type `0x1C` (hidden FAT32), in free space | the falcon boot type `D0ACB249-F895-4E0D-AB0D-CFBB6F48AE36`, named `FALCON-STATION` |
| stage 2 | LBA 1, before the first partition | the `FALCON-BIOS` partition (EF02), made at LBA 34 when the disk has none |
| stage 1 | LBA 0's boot code, table and signature kept | the same |

The station partition goes in the highest free run that holds `--size`, on
a MiB boundary where the run allows it and a 4 KiB one where it does not.
It is formatted FAT32 with one sector a cluster and the reserved sectors
`bios` uses (10.1): its boot record at byte 0x1DC, the marker at reserved
sector 8, stage 3 from 32. Its files are `STATION.INI`, `MEMTEST.BIN`
(`--memtest`), and with `--legacy` jump's drivers from `--mods` as `*.MOD`
and each `--uki` in `/uki`.

**Sector sizes.** Disks of 512- and 4096-byte sectors, MBR or GPT. Every
LBA above is the disk's own sector: on a 4Kn disk stage 2 is still at LBA
34 on GPT (byte 139264) and LBA 1 on MBR, the reserved sectors, the marker
and stage 3 are 4096-byte sectors, and the station partition's FAT has
4096-byte sectors, which is why its size floor is 272 MiB there. An image
file is taken as 512-byte sectors; a 4Kn image is written through a loop
device made with `losetup --sector-size 4096 -P`. A loop device is an
image's: nothing else of this machine is scanned for it and no firmware
entry is made.

**UEFI.** Into `\EFI\falcon\` on the disk's ESP: the station loader as
`falconx64.efi` signed with the MOK, `falcon-boot.cer`, `STATION.INI` and
`memtest.efi` (`--memtest-efi`, signed with the MOK). The ESP's boot sector
gets the station's boot record, with stage 3 and marker 0. Without
`--shim` the jumper is written as `\EFI\BOOT\BOOTX64.EFI` (below), and on a
block device of a machine that booted with UEFI a firmware entry named
`falcon` is created for it and put first in BootOrder, any other `falcon`
entry deleted; for an image it prints the `efibootmgr` line to run on its
machine. With `--shim`, `shimx64.efi` and MokManager under the name shim
launches it by are written too, the loader is named shim's next stage
(`grubx64.efi`), there is no jumper, and the entry names
`\EFI\falcon\shimx64.efi`.

**The jumper.** Without `--shim`, `falcon-esp-jump.efi` (beside the
loader) is signed with the MOK and written as `\EFI\BOOT\BOOTX64.EFI`:

    ESP (partition 1): \EFI\BOOT\BOOTX64.EFI, the jumper to falconx64.efi

The `falcon` entry names it, and a firmware with no entry for the station
(an NVRAM reset, a firmware update that cleared its entries, the disk in
another machine) boots the same file by the removable-media path; either
way it starts `\EFI\falcon\falconx64.efi`. When that is missing, or the
firmware refuses it, it prints

    falcon-esp-jump: \EFI\falcon\falconx64.efi: status 0x<status>

and returns to the firmware after five seconds. A `BOOTX64.EFI` already on
the ESP is another system's; it is saved beside the backup file before the
jumper replaces it, and `remove` puts it back:

    \EFI\BOOT\BOOTX64.EFI was another system's; saved to <backup>.BOOTX64.EFI

With `--shim` there is no jumper, since the MOK is not in that machine's
db.

**What it finds.** Every disk of the machine (a block-device target; an
image's machine is the image and `--also`; `--no-scan` limits a block
device to itself and `--also`) is read as the loaders read it:

- each primary partition whose boot sector names a loader: `BOOTMGR`,
  `NTLDR`, `WINBOOT.SYS`, `IO.SYS`, `KERNEL.SYS`, GRUB, SYSLINUX;
- each other disk whose MBR has boot code, an active entry and no `0xEE`
  entry;
- with UEFI, `\EFI\Microsoft\Boot\bootmgfw.efi` on a disk's ESP, and on
  this machine the firmware's Windows Boot Manager entry;
- the shelf's UKIs, when it can be read.

`STATION.INI` is written from that: a section per partition, disk and ESP
Windows found (`windows`, `dos`, `grub`, `syslinux`, `disk`, numbered when
repeated) with its `disk`, `part` and label, and `default` naming the first
Windows or DOS, else the first UKI (`uki-<name>`), else the first other
loader, with `timeout = 5`. A UKI default on a LUKS2 shelf is a `[linux]`
section with `uki = <file>` instead of its generated name. Nothing found is
no default and no countdown, and the file is one comment line. Each disk
the file names that has signature 0 is given a random one.

**Refusals.** No partition table; sectors other than 512 or 4096 bytes; a
disk already carrying a falcon boot record; no free space for the station
partition (it names the size it needs and the largest free run); no free
primary slot; no room before the first partition for stage 2; a mounted
ESP; `--size` out of range; `--uki` without `--legacy`; the UEFI station
with neither `--key` nor `--shelf`. It warns when the station partition or
the shelf ends past 8 GB (`a BIOS without INT 13h extensions does not
reach it`) or 128 GiB (`a BIOS with 28-bit LBA does not reach it`).

**The backup** (refused when it exists, without `--force`) is written
before anything else and holds, in the disk's sectors: LBA 0; from the
table's end to the first partition (stage 2's area); on GPT the primary
and backup tables; the station partition's reserved sectors and 8 MiB
after them; the ESP's boot sector and its FAT32 backup; each other disk's
LBA 0 before its signature. Then, in order: the shelf's stamp, the station
partition, its files, record and stages, the ESP, other disks' signatures,
and LBA 0 last.

#### remove

| option | what it does |
|---|---|
| `--backup <file>` | required: the file `station install` wrote |
| `--full` | also puts back the partition tables, the station partition's first sectors and other disks' signatures |
| `--force` | restores onto a disk that carries no record of this install |
| `--bootx64 <file>` | the `BOOTX64.EFI` to put back where the jumper was (default: `<backup>.BOOTX64.EFI` when install saved one) |
| `--dry-run` | reports and writes nothing |

It reads the backup (its disk, or `<disk>` when named now), checks that a
partition there carries the backup's install ID, and puts back LBA 0's boot
code, stage 2's area and the ESP's boot sectors; deletes `\EFI\falcon`, the
jumper (putting back the `BOOTX64.EFI` it replaced: `<backup>.BOOTX64.EFI`,
or `--bootx64 <file>` for one `update` saved), the `falcon` firmware entry
it made and any `falcon` entry naming the jumper; deletes the `FALCON-BIOS` partition it
made. The station partition stays until it is deleted. `--full` restores
every run: the partition tables whole (the station partition's entry goes),
the station partition's first sectors, and the other disks' signatures.

#### status

Read-only: the install ID and every partition's record, the BIOS stages
where stage 1 and the station partition's record put them, the ESP's files
(the loader is the UEFI install: shim's next stage when shim is there,
else `falconx64.efi`, or `grubx64.efi` on an install from before the
jumper; its signature is checked against `falcon-boot.cer` when `sbverify`
and `openssl` are installed), without shim `\EFI\BOOT\BOOTX64.EFI` (a
failing check when it is not the jumper, whose signature is checked the
same way) and, on a disk of the machine it runs on, that the `falcon`
firmware entry names the jumper and is first in BootOrder, each `STATION.INI` as the loaders parse it (or
the line it fails at), the shelf carrying the install ID, and the rows each
menu shows (found systems, what STATION.INI adds, relabels and hides) with
the default marked. On a block device every disk of this machine is read;
`--also` adds a disk or image and `--no-scan` limits it to `<disk>` and
`--also`. It exits nonzero when a stage or file is missing.

#### totp

The UEFI station's TOTP check (DESIGN Section 12.15):

    falcon-boot-mkstick station totp enroll <disk> [--pcrs <list>]
        [--owner-auth-file <file>] [--label <name>]
    falcon-boot-mkstick station totp reseal <disk> [--pcrs <list>]
        [--owner-auth-file <file>] [--label <name>]
    falcon-boot-mkstick station totp remove <disk> [--owner-auth-file <file>]
    falcon-boot-mkstick station totp check <disk>

The test rig's options for these (`--tpm-socket`, `--event-log`,
`--saved-log`) are in Section 13.4.

| option | default | what it does |
|---|---|---|
| `--arch` | this machine's | the machine the station boots, `x64` or `aa64` (10.6); every totp command takes it |
| `--pcrs` | `0,2,4,7` | the PCRs to seal to, 0 to 15 |
| `--owner-auth-file` | none on a TPM 2.0, the well-known secret on 1.2 | the TPM owner's authorisation: the file's bytes, a trailing newline dropped; on 1.2 exactly 20 bytes is the secret itself, anything else a passphrase whose SHA-1 it is |
| `--label` | this host's name | the authenticator's account name |

**enroll** runs from a system the station started. It seals a new 20-byte
seed into the TPM (NV index `0x01800FA1` on 2.0, `0x20000FA1` on 1.2),
writes STATION.INI's `[totp]` section, and prints:

    Add this to an authenticator now; it is not shown again.
      account: falcon-station:<label>
      secret:  <base32>
      uri:     <otpauth URI>

with a QR code when `qrencode` is installed, else `qrencode is not
installed, so there is no QR code: enter the secret by hand.`

The PCR values are the ones the station read at boot and saved
(`FalconEvLog`), checked against the firmware's event log: on a TPM 1.2
against the log the station saved, alone or after the one Linux reads; on
a TPM 2.0 against the one Linux reads. Enrollment says which:

| line | meaning |
|---|---|
| `PCRs from the event log the station saved` (`, after the kernel's`) | TPM 1.2 |
| `PCRs the station read, matching the event log` | TPM 2.0 |
| `PCRs the station read: this firmware extends PCRs it does not log, so the event log is set aside` | TPM 2.0 on a firmware whose log does not account for the TPM (an AMD firmware TPM on an ASUS B550 board is one) |

Without shim every boot of the station goes through the jumper, through
the `falcon` entry or the removable-media path alike, so it is enrolled
from either and the code shows on both. It refuses when the log has no
measurement of the station's loader (the one
`\EFI\falcon\shimx64.efi` starts, else `\EFI\falcon\falconx64.efi`),
when the saved PCRs are not of this boot (`PCR <n> is not what the station
read`), or when a log that accounts for the TPM does not give what the
station read. A station older than the saved PCRs saves none, and the log
alone then gives them, as long as it accounts for the TPM; on a firmware
whose log does not, update the station and boot it once first. When the
station ran and saved none, enrollment says what the station found
instead, from the variable `FalconEvLogStatus`:

    the station saved no PCRs: TPM 2.0: reading PCR 0: the TPM answered 0x00000101

(`cat` it from `/sys/firmware/efi/efivars/` on any boot; the first four
bytes are the variable's attributes).

What the code is sealed to changes with what is in the chosen PCRs:
firmware (0, 2), the station image (4), Secure Boot's keys and the
authority that verified the station (7), and with 5, the GPT and the boot
variables, so an `efibootmgr` change or a repartition moves it too. After
any of these the station shows a warning instead of the code.

**reseal** is enroll over an existing enrollment: a new seed, sealed to
this boot. The authenticator's old entry stops matching; replace it.

**remove** deletes the NV index as the owner and removes `[totp]`.

**check** runs the station's own read of the seed with STATION.INI's
`[totp]` from the running system and changes nothing. From a running
system the TPM refuses on the PCRs, which is the good answer: `the TPM
refused on the PCRs, as it does outside the station: the index, the
storage key recorded in STATION.INI are this TPM's` (on 1.2, `the storage
key and its secret`). Any other refusal is printed with the step that got
it (`the TPM refused: code 0x00000001 in EstablishTransport`).

### 10.7 update

    falcon-boot-mkstick update <disk> [--from <dir>] [--backup-dir <dir>]
        [--passphrase-file <file>] [--key <file> --cert <file>]
        [--owner-auth-file <file>] [--dry-run] [--yes]

Brings a stick or a station to the loaders in `--from`, in place (DESIGN
Section 12.14). The test rig's options (`--tpm-socket`, `--event-log`,
`--saved-log`) are in Section 13.4.

| option | default | what it does |
|---|---|---|
| `--from` | `build/bin` | the built loaders: `falcon-boot.efi`, `falcon-boot-aa64.efi`, `falcon-boot-station.efi`, `falcon-esp-jump.efi` (with `-aa64` for an AArch64 station), the BIOS stages, and `mod/` |
| `--shim` | `build/shim/bins` | shimaa64 and its MokManager, for a stick that has no AArch64 chain yet |
| `--arch` | this machine's | the station's architecture, `x64` or `aa64` (10.6) |
| `--backup-dir` | `.` | where what is replaced is saved |
| `--passphrase-file` | prompt | the shelf's passphrase; the open shelf's mapping is then not used |
| `--key`, `--cert` | the shelf's | the MOK pair, for an install whose shelf does not hold it; given together |
| `--owner-auth-file` | as for `station totp` | the TPM owner's authorisation, for the reseal |
| `--dry-run` | off | prints the plan and writes nothing |
| `--yes` | off | does not ask |

What the disk is comes from its records:

| disk | found by | written |
|---|---|---|
| stick | a boot record, with a library and a shelf of its install ID | the loader on p1, and the AArch64 loader beside it; a stick made before it had the AArch64 chain gets shimaa64, its MokManager and the loader, when `--from` and `--shim` hold them; stages 1-3 when LBA 0 holds stage 1 |
| BIOS station | a boot record naming stage 3, with `STATION.INI` | stages 1-3 (`falcon-bios-station-stage3.bin`) |
| station-legacy | the same, with any `*.MOD` on the partition | stages 1-3 (`falcon-bios-station-legacy-stage3.bin`) and the drivers |
| UEFI station | the ESP's boot record, with a loader in `\EFI\falcon` | that loader; without shim, the jumper and its entry |

A UEFI station without shim gets the jumper as install writes it (Section
10.6): signed again where it is falcon's, written where there is none, and
where another system's `BOOTX64.EFI` is there, that one saved first:

    \EFI\BOOT\BOOTX64.EFI was another system's; saved to <dir>/<disk>-bootx64-<stamp>.efi (`station remove --bootx64 <file>` puts it back)

On a disk of the machine it runs on, the `falcon` entry is made to name the
jumper and put first in BootOrder; otherwise it prints the `efibootmgr`
line to run on the disk's machine. `--from` must hold `falcon-esp-jump.efi`.

A station installed before the jumper (no jumper of falcon's on its ESP)
is migrated in the same run: the plan says

    write: partition <n> \EFI\falcon\falconx64.efi in place of grubx64.efi, signed with the station's MOK
    write: \EFI\BOOT\BOOTX64.EFI, the jumper, signed with the station's MOK
    entry: falcon naming \EFI\BOOT\BOOTX64.EFI, first in BootOrder
    totp: not resealed; PCR 4 measures the jumper from now on, so the check is enrolled again after the next boot

and the old `grubx64.efi` is saved (`<disk>-station-loader-<stamp>.efi`)
and deleted, and the entry that named it deleted. With `[totp]`, boot the
updated station once and run `falcon-boot-mkstick station totp reseal
<disk>` from the system it started; update says so, and it says the same
when the jumper it wrote is a new image.

A disk with both stations gets both halves. Anything else is refused
(`<disk>: <why>; nothing written`), and so is a disk with a partition it
writes mounted. A driver missing from `--from`'s `mod/` is skipped with a
warning and the one on the disk stays.

The UEFI loader is signed with the install's MOK, read into memory off the
shelf carrying the install ID. On the personal OS, whose crypttab opens the
shelf with its keyfile (Section 5.3), update reads it through that mapping
and asks nothing (`the MOK is read off the shelf, open as
/dev/mapper/<name>, into memory`). A shelf that is not open is unlocked
with its passphrase, asked for once or read from `--passphrase-file`.
`--key` and `--cert` name the pair instead. The signed loader must verify against the certificate before it
is written, and is read back after. `sbsign` and `sbverify` must be
installed; mtools writes the ESP.

Before writing, it prints the plan (`write:`, `reseal:`, `keep:`, `clear:`
lines) and asks `update it? type yes to go on: `. Anything but `yes` ends
it with `not confirmed; nothing written`. What each half replaces is saved
to `--backup-dir` first, `<stamp>` being `YYYYMMDD-HHMMSS`:

| backup | undo |
|---|---|
| `<disk>-bios-<stamp>.bin` | `falcon-boot-mkstick bios <disk> --remove --backup <file>` |
| `<disk>-stick-loader-<stamp>.efi`, `<disk>-station-loader-<stamp>.efi` | copy it back over the loader with `mcopy` |
| `<disk>-esp-jump-<stamp>.efi` | copy it back over `\EFI\BOOT\BOOTX64.EFI` with `mcopy` |
| `<disk>-bootx64-<stamp>.efi` | `falcon-boot-mkstick station remove --bootx64 <file>`, or `mcopy` it back over `\EFI\BOOT\BOOTX64.EFI` |

It keeps every record, `STATION.INI`, `/uki`, shim, MokManager, the
certificate and the firmware's boot entries. It clears a pending next-boot
marker.

A UEFI station with `[totp]` is resealed to the new loader in the same
run, and prints a new secret for the authenticator. Run it from the system
the station started. If the reseal fails, the loader is still updated and
it says `the TOTP seed was not resealed: <why>`; run `station totp reseal`
after the next boot.

### 10.8 keygen

    falcon-boot-mkstick keygen <dir> [--cn <name>]

Generates a MOK pair for signing on a host (DESIGN Section 7.5): writes
`falcon-mok.key` (mode 0600), `falcon-mok.crt` (PEM, what `sbsign` takes)
and `falcon-mok.cer` (DER, what MokManager enrolls) into `<dir>`, made
0700 if it does not exist. RSA 4096, valid 3650 days, CN `falcon-boot MOK`
unless `--cn` says otherwise. It refuses to overwrite an existing
`falcon-mok.key`. Needs `openssl`.

### 10.9 varstore

    falcon-boot-mkstick varstore <cert.crt> <out.fd> [--template <file>]
        [--virt-fw-vars <path>]

For the QEMU rig only: copies an OVMF Secure Boot varstore (`--template`,
default `$OVMF_DIR/OVMF_VARS.secboot.fd`, `OVMF_DIR` defaulting to
`/usr/share/edk2/OvmfX64`) to `<out.fd>` and adds `<cert.crt>` to its
MokList with `virt-fw-vars` (default `build/shim/venv/bin/virt-fw-vars`,
which `falcon-efi/tools/mk-test-mok.sh` creates), so a rig boot skips
MokManager's enrollment. It prints the line that boots it:

    boot it with: MOK_VARS=<out.fd> VARS_FILE=build/vars-run.fd falcon-efi/tools/run-qemu.sh --stick --secboot

Section 13.4 has the rig.

### 10.10 kdf-bench

    falcon-boot-mkstick kdf-bench [--memory <MiB,...>] [--time <n,...>]
        [--lanes <n>] [--threads <n>] [--level <name>] [--repeat <n>]

Times falcon-core's argon2id, the code both loaders unlock a shelf with,
and prints the seconds one derivation takes for each memory size (rows)
and pass count (columns). It recommends nothing; the table is for
choosing a keyslot's cost for a machine, which `cryptsetup luksFormat`'s
own benchmark cannot do, since it times Linux's implementation. Run it on
the machine the slot is for, with nothing else busy. It needs no
privileges and touches no device.

| option | default | what it does |
|---|---|---|
| `--memory` | 64,128,256,512,1024 | the rows, in MiB, up to 4096 |
| `--time` | 1,2,4,8 | the columns: passes, cryptsetup's `--pbkdf-force-iterations` |
| `--lanes` | 4 | cryptsetup's `--pbkdf-parallel` |
| `--threads` | the lanes, at most the processors | 1 runs the lanes one after another |
| `--level` | `auto` | the block function: `auto`, `portable`, `sse2`, `ssse3`, `avx2` |
| `--repeat` | 1 | runs a cell, the fastest kept |

Both loaders run a slot's lanes on one processor a lane, up to the
machine's cores, when the slot costs at least 512 MiB-passes, and one
after another below that. Each unlock says on serial what it ran on and
how long the keyslots took (falcon-bios to the 55 ms its clock counts in):

    falcon-pos: argon2id block function: avx2
    falcon-pos: argon2id ran on 4 processors in 370 ms: argon2id avx2 4 processors 4 lanes 262144 KiB t=4 370 ms 2394 MHz

which is the figure to compare with the table's. The UEFI loader's line
ends with the unlock's `FalconKdfInfo` (Section 5.6), the effective clock
included; falcon-bios's ends at the time. `auto` is the level the loaders pick on this processor, which they say on
serial at every unlock (`argon2id block function:`). A level the
processor cannot run is refused. The memory is allocated once and touched
before the first cell, so page faults are not timed; a row larger than
the machine can allocate stops the command before anything is timed.

    $ falcon-boot-mkstick kdf-bench --memory 256,1024 --time 1,4
    argon2id in falcon-core: avx2 block function, 4 lanes on 4 threads
    seconds an unlock attempt takes; rows memory, columns passes

      memory  t=1      t=4
     256 MiB  0.06     0.21
    1024 MiB  0.28     0.81

    cryptsetup luksAddKey --pbkdf argon2id --pbkdf-memory <MiB x 1024> --pbkdf-force-iterations <t> --pbkdf-parallel 4

The cell is one attempt. A wrong passphrase costs every keyslot the
passphrase is tried against (DESIGN Section 5.2), so a shelf with a
second argon2id slot pays twice for a typo; the keyfile slot Section 5.3
adds is pbkdf2 for that reason.

## 11. Signing, and what lives where

`docs/DESIGN.md` Sections 4 and 7.5 give the reasoning.

### 11.1 What carries which signature

"Ours" is the install's own MOK pair: on a stick, `/falcon-mok.key` and
`/falcon-mok.crt` on p3; for a host-side route, the pair `keygen` (10.8)
or `create --key-out` wrote.

| file | as `mkstick` writes it |
|---|---|
| p1 `/EFI/BOOT/BOOTX64.EFI` (shim) | its vendor signatures as fetched (Microsoft UEFI CA 2011 and 2023) |
| p1 `/EFI/BOOT/mmx64.efi` (MokManager) | its vendor signature as fetched (Fedora) |
| p1 `/EFI/BOOT/grubx64.efi` (the loader) | ours only |
| p3 `/uki/*.efi` | ours only |
| station `\EFI\falcon\shimx64.efi`, its MokManager (with `--shim` only) | as fetched |
| station `\EFI\falcon\falconx64.efi` (`grubx64.efi` behind shim), `\EFI\falcon\memtest.efi` | ours only |
| station `\EFI\BOOT\BOOTX64.EFI`, the jumper (without `--shim`) | ours only |
| p2 `/falcon/blessed/` wraps | ours only |

On a machine whose db trusts Microsoft, a stick's shim, or a station's
installed with `--shim`, boots and checks the loader and the UKIs against
the MOK once `falcon-boot.cer` is enrolled.

On a machine whose db holds only its owner's keys, enrol `falcon-boot.cer`
in **db**, not only as a MOK, and never sign anything with that machine's
own db key. A station there is installed as by default, without shim
(10.6): its firmware entry names the jumper, and db trusts it and the
loader. A stick
boots there only once its shim and MokManager also carry our signature,
appended with the recipe below.

Signing a UKI, or anything else, with the MOK (p3 mounted at `/efi`, as
the personal OS mounts it):

```sh
sbsign --key /efi/falcon-mok.key --cert /efi/falcon-mok.crt \
       --output NEW.efi FILE.efi
sbverify --list NEW.efi | grep '^signature'
```

Check that the listing shows exactly the signatures expected: one of ours
on a file that had none, and one more than before on a vendor binary.
`sbsign` appends, so a second run adds a duplicate, and `sbattach --remove`
deletes one entry per call. `sbverify --cert` cannot say who signed a
binary that carries more than one signature; compare certificate
fingerprints instead:

```sh
openssl x509 -in /efi/falcon-mok.crt -noout -fingerprint -sha256
```

### 11.2 Neither partition is a storage location

p1 holds four files and p3 holds five kinds of file:

```
p1  EFI/BOOT/BOOTX64.EFI  EFI/BOOT/grubx64.efi  EFI/BOOT/mmx64.efi
    falcon-boot.cer

p3  falcon-mok.key  falcon-mok.crt  uki/*.efi  vmlinuz-*  lost+found
```

- `vmlinuz-*` on p3 must stay: the UKI is rebuilt from it on a kernel
  upgrade (DESIGN Section 5.3).
- The loader offers only a UKI whose name ends in `.efi`, directly under
  `/uki`. A rollback saved as `something.efi.prev` is never offered; name
  it `something-prev.efi`.

Files, captures and backups go on p2. Check both, with p1 mounted at
`/mnt/p1` and p3 at `/efi`:

```sh
find /mnt/p1 -type f            # expect exactly the four above
ls -A /efi                      # expect exactly the five kinds above
```

## 12. Troubleshooting by failure page

Every route that comes back draws one page: the label, then
`<what> failed while <stage>`, then the advice when there is one, then
`press any key to return to the menu.` On UEFI the stage is followed by
`: ` and the firmware's status in hex; on BIOS there is no status. The
tables below give every stage a route can name, the advice the page prints
for it (`-` when it prints none), and what to do.

The `<what>` word is the route's: `boot` (CHN, IMG, EFI, WIN, and every
falcon jump route), `blessed boot` (BLS, BLB), `direct boot` (DIR, D3A,
D3B), `the memory-disk boot` (BSD), `personal OS boot` (POS),
`Windows setup` (WXP), and `the signing appliance could not be started
while` (APP). On a station the word is the route code itself (`NXT`,
`MEM`, `ESP`, `VBR`, `MBR`, `CDR`, `USB`, `PCC`), except POS.

A route that ran and returned prints `the payload exited, status <hex>`
(or `the kernel exited`, `the blessed image exited`, `the image exited`,
`the ISO's loader exited`, `the appliance exited without rebooting`)
instead of the failure line. That is the booted program giving up, not the
loader.

### 12.1 Before the menu (BIOS)

These come before stage 3 draws, on screen and on serial (COM1). Each ends
in a key and a reset; none halts.

| line | from | what to do |
|---|---|---|
| `falcon-bios: read error` then `press a key` | stage 1 (the MBR, or jump's floppy boot sector) | The BIOS could not read stage 2. Try another port or drive; on a floppy, another disk. |
| `falcon-bios: no stage 2` then `press a key` | stage 1 | Stage 2 is not where stage 1 says. Reinstall: `falcon-boot-mkstick bios <target>` (10.2), or `station install` on a station (10.6). |
| `falcon-bios: stage 2 did not read back as it was written; the disk may be failing` | stage 2 | The check on stage 2 failed. Reinstall it; if it recurs, replace the medium. |
| `falcon-bios: this processor is older than i586, which this loader needs` | stage 2 | The machine cannot run this loader. |
| `falcon-bios: the BIOS keeps memory below 608 KiB that this loader uses` | stage 2 | The BIOS's own data area reaches memory the loader needs. The loader does not run on this machine as it is configured. |
| `falcon-bios: A20 will not open; this machine cannot be booted` | stage 2 | Nothing to do on this machine. |
| `falcon-bios: the boot disk's partition table or a boot sector would not read` | stage 2, finding stage 3 | The disk failed a read. Try another port. |
| `falcon-bios: the boot disk's partition table is damaged` | stage 2 | Check the table with `falcon-boot-mkstick status` (10.3). |
| `falcon-bios: no partition on the boot disk carries falcon's boot record` | stage 2 | The `boot` record is gone. `falcon-boot-mkstick ident <target>` (10.4) shows what each partition carries. |
| `falcon-bios: the boot record's stage 3 is outside its volume's reserved sectors` | stage 2 | The record and the volume disagree. Reinstall with `bios` (10.2). |
| `falcon-bios: stage 3 could not be read` | stage 2 | The disk failed a read. |
| `falcon-bios: stage 3 is not where the boot record says it is` | stage 2 | Stage 3 was never installed, or was overwritten. Reinstall with `bios` (10.2). |
| `falcon-bios: stage 3 declares a length past its volume's reserved sectors` | stage 2 | Reinstall with `bios` (10.2). |
| `falcon-bios: stage 3 declares a size that will not fit below its staging` | stage 2 | Reinstall with `bios` (10.2) from a matching build. |
| `falcon-bios: stage 3 is on the disk but would not read whole` | stage 2 | The disk failed a read. |
| `falcon-bios: stage 3 did not read back as it was written, twice; the disk may be failing` | stage 2 | Reinstall stage 3; if it recurs, replace the medium. |
| `falcon-bios: stage 3 inflated to something that is not stage 3` | stage 2 | Reinstall with `bios` (10.2) from a matching build. |
| `falcon-bios: stage 3 returned` | stage 2 | Report it with the serial log. |
| `falcon-bios: processor exception <n> (<name>)` and `eip 0x<addr>, error 0x<code>` | stage 2 or 3 | A fault inside the loader. Report it with the serial log and the row chosen. |

From stage 2 on, each line is followed by `press any key to reboot.`.
While stage 3 loads, the screen shows `falcon-boot: loading /` with a
spinner; a spinner that stops turning is a drive that stopped answering.

### 12.2 Route failures

#### UEFI: CHN and IMG (the medium's own loader)

| stage | advice | what to do |
|---|---|---|
| `preparing the virtual media` | - | Reboot and try again. |
| `previous virtual media still held by a driver` | - | A driver kept the last medium. Reboot before booting another. |
| `opening the image on its volume` | - | p2 failed a read, or the file is gone. |
| `the .iso is too small to be CD media` / `the .img is too small to be disk media` | - | The file is truncated. Copy it again. |
| `the .img is not a whole number of 512-byte sectors` | - | The file is truncated or not a disk image. |
| `the override set has more entries than the loader allows`, `... has a zero-length entry`, `... is unsorted or its entries overlap`, `an override reaches past the end of the virtual medium`, `an override's sidecar file is missing or too short`, `an override names the image itself as its sidecar` | - | The image's edits are malformed. Report it with the ISO's name. |
| `publishing the virtual media` | - | Reboot and try again. |
| `enumerating filesystems`, `ConnectController on the virtual media` | - | The firmware did not read the medium. Try `d` (direct) or bless it. |
| `finding the medium's EFI System Partition` | - | The medium has no UEFI boot partition. It boots on BIOS only, or not at all. |
| `reading the ISO ESP's device path`, `building the payload's device path` | - | Report it. |
| `LoadImage of \EFI\BOOT\BOOTX64.EFI (shim)` or `(firmware)` | the platform-refusal block of 12.3, on `0x800000000000001A` | See 12.3. |
| `StartImage`, `payload exit` | - | The medium's loader failed. Its own messages, if any, were on screen before the page. |
| `releasing the virtual media` | - | Reboot before booting another medium. |

A refusal also prints the signatures the image carries:
`signature <n>: <signer> <- <anchor> (<list>)` (`<list>` is `db`,
`MokList`, `dbx`, `MokListX`, `SbatLevel` or `not enrolled`),
`signature: none`, and `revoked by <list>` when a revocation decided it.

#### UEFI: DIR, D3A, D3B (direct boot)

The page also shows `entry: <title>`, the ISO configuration entry booted,
and always ends with `press c on the entry to boot the ISO's own loader
instead.`

| stage | advice | what to do |
|---|---|---|
| `reading the ISO library's record` | `the ISO library has no readable /.falcon record.` | Check p2 with `falcon-boot-mkstick status` (10.3); restore it with `ident` (10.4). |
| `finding the ISO library's partition` | `the ISO library is not a partition this loader can name to Linux.` | Press `c`. |
| `mapping the ISO's extents on p2` | `the ISO is in too many pieces on the stick. Copy it again, onto a stick with more free space.` | As it says, or defragment p2 by copying its files off and back. |
| `opening the ISO`, `reading the ISO's filesystem` | - | The ISO is damaged or not ISO9660/UDF. Copy it again. |
| `reading the ISO's boot configuration`, `building the cmdline` | - | Press `c`. |
| `reading the kernel`, `reading the initrd` | - | The ISO is damaged. Copy it again. |
| `checking the kernel for SBAT metadata`, `reading the ISO's shim` | - | Press `c`, or bless the ISO. |
| `setting FalconIsoMap` | - | The firmware refused the variable write. Press `c`. |
| `publishing the initrd`, `publishing the ISO's shim and kernel`, `setting the cmdline` | - | Reboot and try again. |
| `LoadImage of the ISO's shim`, `LoadImage of the kernel (shim)` or `(firmware)` | `the platform refused this ISO's kernel. Bless the ISO, or press c on its row to boot its own loader.` | See 12.3. |
| `StartImage` | - | The kernel failed to start. |
| `locating the ISO's grub.cfg`, `building the hook's archive`, `writing the grub.cfg` (D3B) | - | Press `c`, or bless the ISO. |

D3B goes on to boot the ISO's own GRUB over the virtual medium, so its later
stages are the CHN stages above.

#### UEFI: BLS, BLB (blessed media)

| stage | advice | what to do |
|---|---|---|
| `opening /falcon/blessed` | `the blessed directory is gone from this volume. Re-bless the ISO.` | Re-bless. |
| `finding the blessed image the manifest names` | `the manifest names an image that is not on the stick. Re-bless the ISO.` | Re-bless. |
| `reading the blessed image` | `the blessed image is empty or implausibly large. Re-bless the ISO.` | Re-bless. |
| `publishing the image`, `naming the image on the synthetic filesystem` | - | Reboot and try again. |
| `LoadImage of the image (shim)` or `(firmware)` | `the blessed image is not signed with a key this machine has enrolled.` and the four ways out | See 12.3. |
| `StartImage`, `image exit` | the validation-disabled advice of 12.3, when it applies | The wrapped kernel failed. |
| `releasing the synthetic filesystem` | - | Reboot before booting another entry. |
| `naming the blessed loader as the medium's` (BLB) | - | Re-bless the ISO. |

BLB serves the medium as BSD does, so its other stages are the BSD stages
below.

#### UEFI: EFI (a bare `.efi` on p2)

| stage | advice | what to do |
|---|---|---|
| `reading the payload` | `this payload is empty or implausibly large for an EFI binary.` | Copy the file again. |
| `LoadImage of the image (shim)` or `(firmware)` | `this EFI payload is not signed with a key this machine has enrolled.` and the ways out | See 12.3. |
| `StartImage`, `image exit` | - | The payload failed; its own output was on screen before the page. |

#### UEFI: BSD (memory disk)

| stage | advice | what to do |
|---|---|---|
| `opening the ISO`, `reading the ISO's filesystem` | - | Copy the ISO again. |
| `locating the ISO's loader.conf` | - | Press `c`. |
| `placing the memory-disk record` | `this ISO has no free bytes in its root or /boot directory for the record this route adds.` | Press `c`. |
| `checking memory for the image` | `this machine has too little memory below 4 GB to hold the image, which is where FreeBSD's loader has to put it. Press c to boot the ISO's own loader instead.` (on AArch64: `too little free memory in one run`) | As it says. |
| `writing the loader settings` | `this ISO's loader.conf has no comment block long enough to hold the memory-disk settings.` | Press `c`. |

The medium is then served as the CHN route serves it, and its later stages
are the CHN stages.

#### UEFI: WIN (Windows media patched at boot)

| stage | advice | what to do |
|---|---|---|
| `opening the ISO`, `reading the ISO's UDF tree`, `reading boot.wim` | - | Copy the ISO again. |
| `patching boot.wim`, `rewriting boot.wim's UDF entry`, `rewriting the UDF partition` | - | Press `c` to boot it unpatched; Setup then cannot find its source. |
| `booting` | - | The served medium's stages follow, as CHN. |

A refusal on this route prints the signature lines alone, without the
refusal sentence of 12.3.

#### UEFI: POS (personal OS) and APP (signing appliance)

| stage | advice | what to do |
|---|---|---|
| `looking for a UKI shelf` | - | No shelf of this install was found. |
| `looking for an encrypted shelf` | `no encrypted shelf of this install was found.` | Plug in the stick's own shelf. |
| `unlocking the encrypted partition` | `the passphrase did not open any keyslot.` | Type it again. |
| `unlocking the encrypted partition` | `the partition could not be read.` | p3 failed a read. |
| `unlocking the encrypted partition` | `this keyslot needs <n> MiB of memory; this machine has <m> MiB free.` | Boot on a machine with more memory, or add a keyslot with less Argon2id memory from Linux. |
| `unlocking the encrypted partition` | `the volume is outside what this loader supports.` | Check the LUKS2 header with `cryptsetup luksDump`. |
| `opening the decrypted volume` | - | Report it. |
| `mounting the filesystem inside it`, `mounting the plaintext UKI shelf` | `the decrypted volume is not a filesystem this loader can read.` / `the shelf is not a filesystem this loader can read.` | The shelf is not ext4. |
| (same) | `the shelf's journal is damaged; run fsck on it from another system, then boot its OS once with noresume.` | As it says. |
| (same) | `reading the shelf's journal needs <n> MiB of memory; this machine has <m> MiB free.` | Run `fsck` on the shelf from another system. |
| (same) | `the filesystem was not cleanly unmounted; run fsck on it from Linux.` | As it says. |
| `looking for a UKI` | `no *.efi image for this machine was found in /uki or /EFI/Linux.` | Put a signed UKI for this machine's architecture in `/uki` (11.1). |
| `looking for the UKI` | `that UKI is not on the shelf.` | The marker or STATION.INI names a file that is gone. |
| `looking for the signing appliance` | `no signing-appliance image is on this stick's shelf. Bless the ISO on a Linux host with falcon-boot-sign instead.` | As it says, or stage the appliance UKI on p3. |
| `reading the UKI` | - | p3 failed a read. |
| `publishing the credential filesystem`, `naming the UKI on the credential filesystem` | - | Reboot and try again. |
| `LoadImage of the UKI (shim)` or `(firmware)` | `the UKI is not signed with an enrolled key.` and the ways out | See 12.3. |
| `StartImage`, `UKI exit` | the validation-disabled advice of 12.3, when it applies | The UKI failed to start its kernel. |
| `releasing the credential filesystem` | - | Reboot before booting another entry. |

`the shelf's last few seconds of changes were not read.` before the UKI
is read is a note, not a failure: the boot goes on.

#### UEFI station: NXT, ESP, MEM

| code | stage | advice | what to do |
|---|---|---|---|
| NXT | `finding its firmware entry` | `No firmware boot entry names this disk. Set method = efi in STATION.INI to start its bootmgfw.efi.` | As it says. |
| NXT | `setting BootNext`, `restarting` | `Use the firmware's own boot menu at power-on instead.` | As it says. |
| ESP, MEM | `finding its ESP` | `No partition of that disk carries the file.` | Fix the section's `disk`, `part` or `efi` in STATION.INI. |
| ESP, MEM | `building the image's device path` | - | Report it. |
| ESP, MEM | `LoadImage (shim)` or `(firmware)` | `the image is not signed with a key this machine trusts.` | See 12.3; sign `memtest.efi` with the MOK. |
| ESP, MEM | `StartImage`, `image exit` | - | The image failed. |

#### BIOS: CHN (the medium's own boot image, through the virtual CD)

| stage | advice | what to do |
|---|---|---|
| `finding p2's GPT entry` | `the ISO's partition has no GPT entry this loader can serve it from.` | Check the stick with `status` (10.3). |
| `opening the ISO` | - | Copy the ISO again. |
| `reading the ISO's boot catalog` | `the medium has no El Torito boot catalog.` | It is not bootable from BIOS. |
| `reading the ISO's boot catalog` | `the medium boots only on UEFI.` | Boot the stick in UEFI mode. |
| `reading the ISO's boot catalog` | `the medium's boot image asks for hard-disk emulation, which this loader does not do.` | Not bootable through this loader. |
| `mapping the ISO's extents on p2` | `the ISO is in too many pieces on the stick. Copy it again, onto a stick with more free space.` | As it says. |
| `mapping the ISO's extents on p2` | `the ISO is not laid out on the stick in whole CD sectors.` | Copy it again. |
| `reading the medium's boot image` | - | p2 failed a read. |
| `reserving memory for the virtual CD` | `this machine will not give up the conventional memory the virtual CD needs.` | Not bootable this way on this machine. |
| `placing the medium's edits` | - | Report it. |
| `building the memory map the ISO is handed` | `the memory map is too long to hand on with the virtual CD's memory taken out.` | Report it with the serial log. |

A floppy-emulation boot image (memtest86+, Haiku) is served as a memory
floppy instead:

| stage | advice | what to do |
|---|---|---|
| `reading the floppy image's geometry` | `the boot image is a floppy type this loader does not know.` | Not bootable through this loader. |
| `reserving memory for the floppy image` | `this machine has too little memory above 64 MB to hold the floppy image.` / `there is not enough memory to hold the floppy image.` | Use a machine with more memory. |
| `reading the floppy image` | `the floppy image could not be read off the medium.` | Copy the ISO again. |
| `reserving memory for the virtual floppy` | `this machine will not give up the conventional memory the virtual floppy needs.` | Not bootable this way on this machine. |

#### BIOS: DIR (direct boot)

The page shows `entry: <title>` and ends with `press c on the entry to
boot the ISO's own loader instead.`

| stage | advice | what to do |
|---|---|---|
| `opening the ISO`, `reading the ISO's filesystem` | - | Copy the ISO again. |
| `reading the ISO's boot configuration` | `the ISO has no boot entry this loader can take.` | Press `c`. |
| `reading the kernel` | `the entry names its kernel through a value only GRUB could compute.` | Press `c`. |
| `reading the kernel` | `the entry's kernel is not on the medium: <path>.` | Press `c`. |
| `reading the initrd` | `the entry's initrd is not on the medium: <path>.` | Press `c`. |
| `building the cmdline` | - | Press `c`. |
| `starting the kernel` | one of the kernel lines below | See below. |

The kernel lines, shared by DIR, POS and MEM, each ending in `.`:

| advice | what to do |
|---|---|
| `that is not a kernel` | The file named is not a Linux kernel. |
| `the kernel has no setup header; it is not a bzImage` | As it says. |
| `the kernel's boot protocol is older than this loader supports` | Press `c`. |
| `the kernel's setup is longer than the protocol allows` | Press `c`. |
| `the kernel is all setup and no kernel` | The kernel file is truncated. |
| `the kernel is not relocatable and names no address` | Press `c`. |
| `the kernel wants more memory than this loader can address` | Press `c`. |
| `the kernel is not relocatable and its address is this loader's own memory` | Press `c`. |
| `the kernel's setup header makes no sense` | The kernel file is damaged. |
| `the kernel's first sectors would not read`, `the kernel's setup would not read`, `the kernel would not read`, `an initrd would not read` | The medium failed a read. |
| `the kernel is 64-bit and this processor has no 64-bit mode` | Use a 32-bit build of the system, or a 64-bit machine. |
| `the kernel needs memory this machine's map does not have free where it has to go` | Not bootable on this machine. |
| `there is nowhere to put the initrd` | Not enough memory for the initrd. |

#### BIOS: WIN and WXP (Windows media)

| route | stage | advice | what to do |
|---|---|---|---|
| WIN, WXP | `opening the ISO`, `reading the ISO's filesystem` | - | Copy the ISO again. |
| WIN | `reading boot.wim` | - | Copy the ISO again. |
| WIN | `reserving memory for the patch` | `this machine has too little memory above 64 MB to patch boot.wim.` / `there is not enough memory to patch boot.wim.` | Use a machine with more memory. |
| WIN | `reading winpeshl.exe's headers` | `boot.wim has no winpeshl.exe this loader can read.` | Press `c` to boot it unpatched. |
| WIN | `checking the processor` | `Windows needs PAE; this processor has none.` | Use an older Windows. |
| WIN | `checking the processor` | `Windows 8 and later need NX and SSE2; this processor has no SSE2.` / `... has no NX.` | Use Windows 7 or older. |
| WIN | `checking the processor` | `this processor does not answer CPUID.` | Not bootable on this machine. |
| WIN | `checking the processor` | `this is 64-bit Windows and this processor has no 64-bit mode.` | Use 32-bit media. |
| WIN | `reading winpeshl.exe's headers` | `boot.wim is for a processor this loader has no jump binary for.` | Not bootable through this loader. |
| WIN | `patching boot.wim` | `boot.wim is not one this loader can patch.` | Press `c` to boot it unpatched. |
| WIN | `growing boot.wim` | - | Report it. |
| WXP | `reserving memory for the driver injection` | `this machine has too little memory above 64 MB to inject the driver.` / `there is not enough memory to inject the driver.` | Use a machine with more memory. |
| WXP | `reading txtsetup.sif` | - | Copy the ISO again. |
| WXP | `injecting the driver into txtsetup.sif` | `txtsetup.sif is not one this loader can edit.` | Press `c`. |
| WXP | `growing the ISO in memory` | `the ISO's i386 directory has no room for the driver's record.` / `this ISO's i386 layout is one this loader does not edit.` | Press `c`. |
| WIN, WXP | `booting` | - | The served medium's stages follow, as CHN. |

#### BIOS: BSD (memory disk)

| stage | advice | what to do |
|---|---|---|
| `opening the ISO`, `reading the ISO's filesystem` | - | Copy the ISO again. |
| `locating the ISO's loader.conf` | - | Press `c`. |
| `placing the memory-disk record` | `this ISO has no free bytes in its root or /boot directory for the record this route adds.` | Press `c`. |
| `checking the processor` | `this image needs a 64-bit processor, and this one has no 64-bit mode.` | Use i386 media. |
| `checking memory for the image` | `this ISO is too large for its i386 loader to hold as a memory disk: that loader runs under BTX and reaches only about 1 GB. A smaller i386 BSD ISO would boot.` | As it says. |
| `checking memory for the image` | `this machine has too little memory below 4 GB to hold the image, which is where FreeBSD's loader has to put it. Press c to boot the ISO's own loader instead.` | As it says. |
| `writing the loader settings` | `this ISO's loader.conf has no comment block long enough to hold the memory-disk settings.` | Press `c`. |
| `placing the medium's edits` | - | The CHN stages follow. |

#### BIOS: POS (stick and station)

| stage | advice | what to do |
|---|---|---|
| `looking for a UKI shelf` | - (station: `No shelf of this install was found.`) | Plug in the shelf of this install. |
| `unlocking the encrypted partition` | `the passphrase did not open any keyslot.`, `the partition could not be read.`, `this keyslot needs <n> MiB of memory; this machine has <m> MiB free.`, `the volume is outside what this loader supports.` | As on UEFI, above. |
| `opening the decrypted volume` | - | Report it. |
| `mounting the filesystem inside it` | `the decrypted volume is not a filesystem this loader can read.`, or one of the three journal lines given for UEFI | As on UEFI. |
| `looking for a UKI` | `no *.efi image for this machine was found in /uki or /EFI/Linux.` | Put a UKI in `/uki`. |
| `looking for a UKI` | `the image this entry names is not on the shelf.` | Fix the marker or STATION.INI's `uki`. |
| `reading the UKI` | `the image is not a PE file, so it is not a UKI.` / `the image has no .linux section, so it is not a UKI.` | Replace the file. |
| `starting the kernel` | one of the kernel lines (BIOS: DIR) | See there. |

#### BIOS station: VBR, MBR, MEM

| stage | advice | what to do |
|---|---|---|
| `finding the disk` | `The disk this entry names is not attached.` | Attach it, or fix `disk` in STATION.INI. |
| `finding the partition` | `The disk has no partition of that number.` | Fix `part` in STATION.INI. |
| `checking the partition` | `The disk's sectors are not 512 bytes, and a BIOS starts boot code only from a disk whose sectors are.` | Not bootable from BIOS. |
| `checking the partition` | `A partition on a GPT disk has no boot sector a BIOS starts.` | Not bootable this way. |
| `checking the partition` | `It is a logical partition; DOS, Windows 9x and NT start only from a primary one.` | Not bootable this way. |
| `checking the partition` | `It is an extended partition, which holds other partitions and has no boot sector.` | Fix `part` in STATION.INI. |
| `reading the disk's partition table`, `reading the partition's boot sector`, `reading the disk's first sector` | `The disk did not answer a read.` | Check the disk. |
| `reading the disk's partition table` | `The disk's partition table is not the one the menu was built from.` | Restart the station. |
| `reading the partition's boot sector` | `The partition's first sector carries no boot code.` | The system on it needs its boot sector restored. |
| `reading the disk's first sector` | `The disk's sectors are not 512 bytes, and a BIOS starts boot code only from a disk whose sectors are.` | Not bootable from BIOS. |
| `checking the boot signature` | `Its first sector carries no boot code.` | Not bootable. |
| `installing the drive swap` | `There is no conventional memory left for it; swap = no in STATION.INI starts this entry without it.` | As it says. |
| `starting its boot code` | - | The boot code returned. |
| `opening MEMTEST.BIN` | - | Put `MEMTEST.BIN` on the station partition (`station install --memtest`). |
| `starting memtest86+` | one of the kernel lines (BIOS: DIR) | Replace `MEMTEST.BIN` with memtest86+'s `.bin` build. |

#### falcon jump, and station-legacy's CDR, USB, PCC

Jump's disk and memtest rows give the MBR and MEM stages above. The CD,
USB and CF routes print each step as it starts (`loading ATA.MOD...`), and
a failure names the last one.

| stage | advice | what to do |
|---|---|---|
| `loading ATA.MOD`, `loading EHCI.MOD`, `loading UHCI.MOD`, `loading OHCI.MOD`, `loading PCC.MOD` | `the driver is not on the floppy.` | Copy the module onto the floppy (or the station partition). |
| (same) | `the driver could not be read off the floppy.` / `the driver did not read back as it was written, twice. The floppy may be failing.` | Write the image to another floppy. |
| (same) | `the file is not a falcon jump module.` / `the module is from another build of falcon jump. Copy the floppy's modules from the same build as its boot sector.` / `the module is built for another slot; it may have been copied under the wrong name.` / `the module is cut short on the floppy, or does not fit its slot.` / `the module's entry point is not inside it.` / `the module could not be checked.` / `the module names no slot this build has.` | Rebuild the floppy with `make jump` (13.2). |
| (same) | `this machine's memory map has no RAM at 15-16 MB, where falcon jump's drivers run.` | Not usable on this machine. |
| (same) | `the driver would not start.` | Report it with the serial log. |
| `looking for a CD drive` | `there is no CD drive on the IDE channels.` / `there is no disc in the CD drive.` | As it says. |
| `reading the disc's boot catalog` | `the disc has no El Torito boot catalog.` / `the disc boots only on UEFI.` / `the disc's boot image asks for hard-disk emulation, which this loader does not do.` | Not bootable this way. |
| `installing the CD driver` | `this machine will not give up the conventional memory the CD driver needs.` / `the CD driver did not answer through INT 13h.` | Report it with the serial log. |
| `bringing up the PC Card slots` | `there is no CompactFlash card in a PC Card slot.` | Seat the card. |
| `looking for a falcon stick` | `there is no USB storage on the ports.` / `none of the USB storage is a falcon stick.` | Use a root port, not a hub; only a falcon stick is booted. |
| `installing the driver` | `this machine will not give up the conventional memory the driver needs.` / `the driver did not answer through INT 13h.` | Report it with the serial log. |
| `starting the disk` | the MBR lines above | As there. |

On station-legacy the word `floppy` in these sentences reads `disk`.

### 12.3 Platform refusals

When Secure Boot is on, the platform (shim, or the firmware) decides what
runs; the loader only reports it (DESIGN Section 7). A refusal is status
`0x800000000000001A` (`EFI_SECURITY_VIOLATION`) at a `LoadImage` stage,
followed by the signature lines of 12.2. The pages:

| route | text |
|---|---|
| CHN, IMG | `the platform refused to load this payload.` then `Bless the media with the sign tool, turn Secure Boot off in firmware setup, or run mokutil --disable-validation from Linux.` |
| DIR, D3A | `the platform refused this ISO's kernel. Bless the ISO, or press c on its row to boot its own loader.` |
| BLS, BLB | `the blessed image is not signed with a key this machine has enrolled.` then `Re-bless the ISO with the MOK key enrolled here, enroll this stick's certificate with MokManager, turn Secure Boot off in firmware setup, or run mokutil --disable-validation from Linux.` |
| EFI | `this EFI payload is not signed with a key this machine has enrolled.` then `Bless it with the sign tool, which rewrites the payload file itself; or turn Secure Boot off in firmware setup, or run mokutil --disable-validation from Linux.` |
| POS, APP | `the UKI is not signed with an enrolled key.` then `Sign it with the MOK key this machine has enrolled, turn Secure Boot off in firmware setup, or run mokutil --disable-validation from Linux.` |
| WIN | `the platform refused to load this payload.` then `Windows media boots under Secure Boot only with Microsoft's Windows CA in db: enrol it, or turn Secure Boot off in firmware setup.` Windows media cannot be blessed. |
| station ESP, MEM | `the image is not signed with a key this machine trusts.` |

The ways out, all the platform's:

- **Sign it with a key the machine trusts.** Bless the ISO or payload
  (`b` on its row, or `falcon-boot-sign`), or sign a UKI with the MOK
  (11.1).
- **Enroll the key.** Enroll `falcon-boot.cer` with MokManager, or in db
  on a machine that trusts only its owner's keys (11.1).
- **Turn enforcement off.** Secure Boot off in firmware setup, or
  `mokutil --disable-validation` from Linux and confirmation at MokManager
  on the next boot.

With shim validation disabled and Secure Boot still on, a UKI (POS, APP,
BLS) can load and then fail inside. The page then gives:

    shim signature validation is disabled, so shim left the firmware's own
    LoadImage in place, and the firmware refuses the kernel inside a UKI while
    Secure Boot is on.

    Re-enable validation (mokutil --enable-validation from Linux, then confirm
    at MokManager), or turn Secure Boot off in firmware setup. Booting an ISO
    the ordinary way still works.

Before such a boot the POS route warns `warning: this image is very
unlikely to start on this machine; see the note after it exits.`

A row tagged for the other platform shows a page instead of booting, with
no route code:

| loader | tag | text |
|---|---|---|
| UEFI | `{BIOS ONLY}` | `this image boots only on a BIOS machine: it has no UEFI boot loader, and no kernel UEFI can start.` then `Start the machine from this stick in BIOS mode (legacy or CSM) instead. The stick needs its BIOS loader for that (mkstick bios).` |
| BIOS | `{EFI ONLY}` | `this image boots only on a UEFI machine: it has no BIOS boot image, and no kernel falcon-bios can start.` then `Start the machine from this stick in UEFI mode instead.` |
| BIOS | `{64-BIT ONLY}` | `this image needs a 64-bit processor, and this one has no 64-bit mode.` then `Start it on a machine with a 64-bit processor instead.` |

### 12.4 A boot that hangs on a black screen

Windows install media's UEFI loader draws `Press any key to boot from CD
or DVD...` on the graphics console, which the loader's text console does
not show. With no key it gives up and the loader's page reads
`the payload exited, status 0x8000000000000012`, followed by:

    the payload waited for a keypress and gave up. Windows media draws
    "press any key to boot from CD" on the graphics console, which this loader
    never sees. Boot it again and press a key while the screen looks empty.

Boot the row again and press a key as soon as the screen goes blank.

For any other blank screen, the route code at the top right still names the
route that is running: a kernel or loader has the machine and has not
drawn yet. Wait, then check serial (COM1) for the loader's last lines.

### 12.5 After the kernel starts (falcon-hook)

A direct boot, D3B, or a blessed Linux wrap carries falcon-hook in its
initrd. It logs to the kernel log, each line prefixed `falcon-hook: `;
read it with `dmesg | grep falcon-hook` from the booted system or the
distro's emergency shell. It never stops the boot: whatever fails, it
starts the distro's init, and the distro then fails in its own way (no
root medium found).

| line | meaning |
|---|---|
| `no ISO map from the loader; handing over unchanged` | No extent map in the initrd or in efivarfs. The ISO is not mapped; a distro that finds its medium by label fails to find it. |
| `ISO map read, extents: <n>` | The map was read; `<n>` is how many pieces the ISO is in on p2. |
| `loaded drivers with <path>` / `no modprobe in this initramfs; relying on built-in drivers` / `built modules.dep with /sbin/depmod` | How the disk drivers were loaded. |
| `found the ISO library by its record on <dev>` | The disk holding p2 was found. |
| `no disk carries the ISO library the map names` | No attached disk has p2 of this install at the map's LBA. The USB controller's driver is missing from the initramfs, or the stick was moved. |
| `no partition device; mapping over the whole disk` | The kernel shows no partitions for the stick; the ISO is mapped from the whole disk at p2's offset. |
| `/dev/mapper/falcon-iso is the ISO` | Success: the ISO is a read-only device-mapper device. |
| `label <label>` | The label the mapped ISO is published under. |
| `no device-mapper in this kernel or initramfs` | dm is missing; a loop device is tried next. |
| `creating the device failed: <rc>`, `loading the table failed: <rc>`, `the extent map cannot be expressed as a table` | dm refused the mapping; a loop device is tried next. |
| `no device-mapper, and the ISO is in pieces: <n>` | Loop needs a contiguous ISO. Copy it again onto a stick with more free space. |
| `no loop devices in this kernel or initramfs either` | Neither driver is available. |
| `loaded loop from the ISO's own packages` | The loop driver was taken from the ISO's own package pool. |
| `no free loop device: <n>`, `the loop device or the stick could not be opened`, `setting up the loop device failed: <rc>` | The loop fallback failed. |
| `the ISO is <node>` | Success through a loop device. |
| `mounted the ISO for <what>` / `could not mount the ISO for the installer: <rc>` | A distro pre-mount. |
| `nudged <what>` / `could not nudge <what>` / `the udev re-announce failed: <rc>` | A per-distro nudge. |
| `could not unmount <path>` | A mount it made stayed; harmless. |
| `starting <init>` | Handing over to the distro's init. |

When the distro then drops to a shell saying it cannot find its medium, the
line before `starting` says why. Pressing `c` on the row boots the ISO's own
loader instead.

## 13. Building and testing

One non-recursive Makefile at the repository root builds everything. Objects
land in `build/obj/<flavour>/`, binaries in `build/bin/`, fixtures and rig
artifacts elsewhere under `build/`. `make clean` removes `build/` and
`compile_commands.json`.

The build runs in the harness container (13.5). Outside it, the Makefile
reads no further than handing its goals, flags and command-line variables to
`tools/coverage/harness.sh make`, once per invocation. The image is the one
the checkout's Dockerfile describes, pulled from the project's registry:

    docker login registry.git.airies.net:443

once per machine, with a token that can read the registry. Without it the
first `make` builds the image locally (about 3.5 GB, several minutes).

Six goals act on the host and run there: `install`, `uninstall`, `run`,
`run-secboot`, `run-stick` and `run-stick-secboot`. The container first
builds what each needs (`install` the two programs, `run` the loader,
`run-secboot` the signed loader); `run*` needs QEMU and OVMF on the host.

| variable | effect |
|---|---|
| `FALCON_NATIVE=1` | build with the host's toolchain (13.1), not the container |
| `FALCON_HARNESS` | set inside the container; the Makefile builds there |

### 13.1 Toolchain

    make check-tools

prints one line per missing tool and `Toolchain OK.` when the required one
is there. It exits nonzero only when a required tool is missing. Run as
`make check-tools`, it checks the container; with `FALCON_NATIVE=1`, the
host.

Every tool, from `tools/deps.tsv` (the one list; `make check-deps` holds
the harness's Dockerfile and this table to it):

<!-- deps:begin -->
| tool | Debian package | group | needed for |
|---|---|---|---|
| `x86_64-w64-mingw32-gcc` (required) | gcc-mingw-w64-x86-64 | build | the UEFI images, winjump.exe and the x64 vdisk driver (or build in the harness image) |
| `aarch64-w64-mingw32-clang` | llvm-mingw (release) | build | the AArch64 UEFI images and the ARM64 winjump: llvm-mingw 20261020, ucrt, after gcc-mingw on PATH (its x86_64- and i686- wrappers are clang) (or build in the harness image) |
| `arm64 libc, libgcc and headers` | libc6-dev:arm64 libgcc-14-dev:arm64 | build | the arm64 Linux programs (falcon-hook, the fixture UKI's init), built by llvm-mingw's clang and ld.lld; dpkg --add-architecture arm64 (or the harness image) |
| `gcc` (required) | build-essential | build | host tools, tests and the BIOS loader (CC_HOST) |
| `make` (required) | make | build | the one build |
| `pkg-config` (required) | pkg-config | build | finds cmocka for the test binaries |
| `cmocka` (required) | libcmocka-dev | build | every test binary |
| `objcopy, nm` (required) | binutils | build | flat images (the BIOS stages, jump's modules) and build.mk's symbol checks |
| `xxd` (required) | xxd | build | falcon-hook and winjump embedded as C arrays |
| `gzip` (required) | gzip (base) | build | stage 3 and jump's modules are gzip members |
| `cpio` (required) | cpio | build | the initrds the apps' fixtures carry |
| `32-bit libgcc` | gcc-multilib | build | make bios: the i586 loader and falcon-hook32 |
| `i686-w64-mingw32-gcc` | gcc-mingw-w64-i686 | build | make bios: the 32-bit winjump and vdisk driver (or the harness image) |
| `mtools` | mtools | build | make jump and station install write FAT with mtools |
| `sd-stub` | systemd-boot-efi | build | the stub a UKI is built on (sign uki, the fixture UKI, the appliance) |
| `arm64 sd-stub` | systemd-boot-efi:arm64 | build | the stub an arm64 kernel's UKI is built on |
| `arm64 sbsign, sbverify` | sbsigntool:arm64 (download) libssl3t64:arm64 libuuid1:arm64 | build | the AArch64 appliance image's signer, unpacked to /opt/arm64-sbsigntool, with its arm64 libraries |
| `zstd` | zstd | build | the appliance image's initramfs |
| `cryptsetup` (required) | cryptsetup-bin | host tools | mkstick create: p3's LUKS2 |
| `openssl` (required) | openssl | host tools | mkstick keygen and create: the MOK keypair |
| `sbsign, sbverify, sbattach` (required) | sbsigntool | host tools | MOK signatures, their check, and stripping one |
| `mkfs.exfat` (required) | exfatprogs | host tools | p2 |
| `mkfs.ext4` (required) | e2fsprogs | host tools | p3 and the station partition |
| `mkfs.fat` (required) | dosfstools | host tools | p1 and a BIOS boot partition |
| `sgdisk` (required) | gdisk | host tools | GPT |
| `sfdisk` (required) | fdisk | host tools | MBR |
| `losetup, blockdev, partx` (required) | util-linux | host tools | images as block devices |
| `bsdtar` | libarchive-tools | host tools | reading archives in the fixtures and the appliance image |
| `ukify` | systemd-ukify | host tools | cross-checking sign uki's byte identity (cmp-ukify.sh) |
| `virt-fw-vars` | virt-firmware (pip) python3-venv python3-dev libcrypt-dev | tests and fixtures | MOK varstores for the Secure Boot rigs (mkstick varstore); the venv and crypt_r build it needs |
| `xorriso` | xorriso | tests and fixtures | the ISO fixtures and the rig's payload ISO |
| `mkudffs` | udftools | tests and fixtures | the UDF fixture |
| `wimlib-imagex` | wimtools | tests and fixtures | the WIM fixture |
| `7z` | 7zip | tests and fixtures | cross-checks the UDF fixture |
| `ntfs-3g` | ntfs-3g | tests and fixtures | the A21m station rig's disk |
| `gcovr` | gcovr | analyzers | make coverage |
| `cppcheck` | cppcheck | analyzers | make analyze |
| `clang-tidy` | clang-tidy | analyzers | make analyze |
| `objdump, readelf` | binutils | analyzers | tools/check-floor.sh |
| `qemu-system-x86_64` | qemu-system-x86 | rigs | the UEFI rigs |
| `qemu-system-i386` | qemu-system-x86 | rigs | the BIOS rigs (SeaBIOS on an i686) |
| `qemu-system-aarch64` | qemu-system-arm | rigs | the AArch64 UEFI rigs (TCG on an x86 host) |
| `AAVMF` | qemu-efi-aarch64 | rigs | the AArch64 rigs' firmware, plain and Secure Boot |
| `U-Boot` | u-boot-qemu | rigs | the devicetree rig: U-Boot's UEFI on virt |
| `xz` | xz-utils | harness | unpacks the llvm-mingw release into the harness image |
| `qemu-img, qemu-nbd` | qemu-utils | rigs | rig disks, overlays and the station fixture disks |
| `OVMF` | ovmf | rigs | the UEFI rigs' firmware, plain and Secure Boot |
| `swtpm, swtpm_setup` | swtpm swtpm-tools | rigs | the TPM rigs |
| `socat` | socat | rigs | the BIOS rigs' QEMU monitor |
| `magick` | imagemagick | rigs | the BIOS rigs' screendumps |
| `ninja` | ninja-build | rigs | building the PC Card rig's QEMU (build-qemu.sh) |
| `glib, pixman` | libglib2.0-dev | rigs | building the PC Card rig's QEMU |
| `pixman` | libpixman-1-dev | rigs | building the PC Card rig's QEMU |
| `python3` | python3 | harness | the scorecard and the corpus tools |
| `tcpdump` | tcpdump | harness | the harness's DHCP capture |
| `curl` | curl | harness | fetch-shims.sh, corpus-fetch.sh, build-qemu.sh |
| `aria2c` | aria2 | harness | corpus-fetch.sh |
| `git` | git | harness | build-qemu.sh fetches QEMU |
| `falcon-asm` | falcon-asm (source) | editor | an editor's language server for the GAS .S files: hover, highlighting and local labels (https://git.airies.net/vifair22/falcon-asm, `make install`) |
<!-- deps:end -->

The host compiler is `CC_HOST` (default `gcc`).

The build variant is `VARIANT` (default `dev`). It is the last field of
every version string: `<semver>_<YYYYMMDD.HHMM>.<variant>` for the stick,
`.station-<variant>`, `.station-legacy-<variant>` and `.jump-<variant>` for
the others. The semver is `release_version` at the repository root. A
build says `release` only when CI made it from master: master's pipeline,
or a tag on one of master's commits, whose release job publishes nothing
otherwise. Every other build, by hand or in a merge request's or a
release branch's pipeline, says `dev`.

### 13.2 Targets

`make` with no target is `make all`.

| target | builds |
|---|---|
| `all` | the stick's UEFI loader, `cli`, `winjump`, and `compile_commands.json` at the repository root |
| `cli` | `falcon-boot-mkstick`, `falcon-boot-sign`, and `falcon-boot-sign-static` (the appliance's) |
| `bios` | the stick's BIOS loader: `falcon-bios-stage1.bin`, `falcon-bios-stage2.bin`, `falcon-bios-stage3.bin` |
| `station` | the UEFI station `falcon-boot-station.efi` and its jumper `falcon-esp-jump.efi`, stages 1 and 2, and `falcon-bios-station-stage3.bin` |
| `station-legacy` | stages 1 and 2, `falcon-bios-station-legacy-stage3.bin`, and jump's drivers in `build/bin/mod/` |
| `devlink-uki` | `build/devlink/devlink-recovery.efi`, a `--bios-only` UKI named `DevLink recovery`, from `$(DEVLINK_IMAGES)/bzImage` and `rootfs.cpio.xz` (default `DEVLINK_IMAGES=$HOME/git/home/devlink-rescue/build/classic/images`) |
| `station-bundle` | `falcon-station-<version>.tar.gz`: both stage 3s, stages 1 and 2, `mod/`, a stripped static `falcon-boot-mkstick32`, `update.sh`, `VERSION`, `MANIFEST` (sha256 of the rest), and `MEMTEST.BIN` with `MEMTEST=<file>` |
| `jump` | `falcon-jump.img`, the 1.44 MB floppy; `MEMTEST=<file>` adds `MEMTEST.BIN`, `JUMP_INI=<file>` adds `JUMP.INI` |
| `appliance` | `build/appliance/appliance-uki.efi` (unsigned, what mkstick signs and stages), `appliance-uki-signed.efi` beside it (test MOK), and `falcon-boot-appliance-init` |
| `mkstick32` | `falcon-boot-mkstick32`, static i686 |
| `hook` | `falcon-hook` and `falcon-hook32` |
| `winjump`, `winjump32` | `winjump.exe`, `winjump32.exe` |
| `vdisk` | `falcon-vdisk-x64.sys`, `falcon-vdisk-x86.sys` |
| `sbat-check` | checks the stick loader has a `.sbat` section and prints it |
| `install`, `uninstall` | see below |
| `compdb` | `compile_commands.json` alone, for an editor or the analyzers; needs no toolchain |
| `clean` | removes `build/` and `compile_commands.json` |

Every path without a directory is in `build/bin/`. `bios`, `station`,
`station-legacy`, `jump`, `appliance` and `station-bundle` are not in `all`.

`appliance` and `appliance-aa64` take their kernel and modules from
Fedora's packages (`tools/fetch-fedora-kernel.sh`). Build `station-bundle` and `mkstick32` in the harness container
(13.5), whose image has the 32-bit static libc:

    tools/coverage/harness.sh make station-bundle MEMTEST=<file>

`station-bundle` refuses a `falcon-boot-mkstick32` that would not run on an
i586 (`tools/check-floor.sh --userland`).

`make install` puts `falcon-boot-mkstick` and `falcon-boot-sign` in
`$(DESTDIR)$(PREFIX)/bin`, building them first:

    make install                     # /usr/local/bin, mode 0755
    make install PREFIX=/usr         # /usr/bin
    make install DESTDIR=/tmp/pkg    # /tmp/pkg/usr/local/bin, for a package
    make uninstall                   # removes both, same PREFIX and DESTDIR

### 13.3 Tests

    make test

builds and runs every component's test binary (`build/bin/falcon-core-test`,
`-efi-`, `-cli-`, `-appliance-`, `-hook-`, `-bios-`, `-winjump-test`), runs
all of them, and fails if any failed. A suite that reads an image fixture
prints SKIP when the fixture is absent, and a skip is not a failure; three
(`udf edits`, `bootfs`, `wim`) fail instead, so a fresh build directory
needs `make test-fixtures` before `make test` is green.

    make test-fixtures

builds every image fixture those suites read. Several need a loop mount,
which the container makes as root; with `FALCON_NATIVE=1` they need
passwordless sudo:

| target | needs |
|---|---|
| `test-image` (the exFAT image, after `test-iso` and `test-img`) | sudo |
| `test-ext4`, `test-jbd2`, `test-luks` | sudo (`test-luks`: `cryptsetup`) |
| `test-udf` | sudo, `mkudffs` |
| `test-fat` | mtools |
| `test-iso9660` | `xorriso` |
| `test-uefiboot`, `test-biosboot` | `xorriso`, mtools |
| `test-wim` | `wimlib-imagex` |
| `test-trust` | a built loader, `sbsign` |
| `test-isoview` (not in `test-fixtures`) | `xorriso`; no sudo |

    make coverage

runs `make test` with `FALCON_TEST_STRICT=1`, which turns every skip into a
failure, then prints one `gcovr` summary over all the test binaries. Run
`make test-fixtures` first.

    make test-station-install

runs `mkstick station install`, `status` and `remove` end to end on image
files: MBR and GPT, BIOS and UEFI. No root.

    make test-tpm

runs falcon-core's TPM layer against `swtpm`, TPM 2.0 then 1.2, with state
under `build/tpm-rig/`. It skips with a sentence when `swtpm` or
`swtpm_setup` is missing. `make tpm-transcripts` re-records the exchanges
the unit suite replays (`falcon-core/tests/tpm_transcripts.h`).

    make analyze

builds `all` and `bios`, then runs `tools/analyze.sh`: cppcheck
(`--enable=warning,performance,portability`, or `CPPCHECK_ENABLE`), a gcc
`-fanalyzer` rebuild of every loader object, and clang-tidy with the root
`.clang-tidy`, each against `tools/analyze-baseline.txt`. It exits 0 when
every stage is clean or baselined. A missing analyzer is reported and
skipped. Run the script directly for its options:

    tools/analyze.sh [-j N] [--strict] [--update-baseline] [stage ...]

| option | effect |
|---|---|
| `stage` | `cppcheck`, `fanalyzer`, `tidy` (default all three) |
| `-j N`, `--jobs N` | parallelism (default `nproc`) |
| `--strict` | ignore the baseline; fail on any finding |
| `--update-baseline` | rewrite the baseline from this run and exit 0 |

The stack-usage check is not a stage: it is part of the normal compile
flags. `tools/analyze.md` has the suppression policy.

### 13.4 QEMU rigs

`falcon-efi/tools/run-qemu.sh` boots a loader under OVMF (or SeaBIOS, or
AAVMF) with a window, serial mirrored to the terminal and OVMF's debug port
to `build/ovmf-debug.log`:

    falcon-efi/tools/run-qemu.sh [--secboot|--bios] [--aa64] [--stick]
                                 [--headless] [qemu args...]

| option | effect |
|---|---|
| none | `build/bin/falcon-boot.efi` as `\EFI\BOOT\BOOTX64.EFI` on a vvfat ESP (`build/esp`), plus `build/test-exfat.img` when it exists |
| `--secboot` | Secure Boot OVMF: Fedora's shim 16.1 as the default loader, the signed loader as `grubx64.efi`, and the varstore `MOK_VARS`; needs `make sign` |
| `--bios` | SeaBIOS on i440fx: no varstore, and only the drives given after the options (and the stick with `--stick`); not with `--secboot` |
| `--aa64` | the AArch64 loader, `build/bin/falcon-boot-aa64.efi` as `BOOTAA64.EFI` (`build/esp-aa64`), on `qemu-system-aarch64`'s virt machine under AAVMF, with a USB keyboard and ramfb; TCG on an x86-64 host, so slow; the firmware's log is on the serial console; not with `--bios` |
| `--uboot` | `--aa64` under U-Boot's UEFI instead of AAVMF (`UBOOT_BIN`, default Debian's `/usr/lib/u-boot/qemu_arm64/u-boot.bin`): a devicetree machine, no pflash, no Secure Boot; not with `--secboot` |
| `--stick` | boot `build/test-stick.img` alone (`STICK_IMG` overrides) |
| `--headless` | no window; serial to `build/serial.log` (`SERIAL_LOG`), the QEMU monitor on `build/mon.sock` (`MON_SOCK`) |

| env | default |
|---|---|
| `STICK_IMG` | `build/test-stick.img` |
| `MOK_VARS` | `build/mok-test/OVMF_VARS.secboot.mok.fd`; `OVMF_VARS.secboot.moksb.fd` beside it adds `MokSBState` (validation disabled) |
| `VARS_FILE` | the writable copy, `build/OVMF_VARS.fd` or `build/OVMF_VARS.secboot.fd`; it is made only when absent, so set a new one whenever `MOK_VARS` changes |
| `QEMU_MEM` | `512` (MiB) |
| `OVMF_DIR` | searched: `/usr/share/edk2/OvmfX64`, `/usr/share/edk2/x64`, `/usr/share/edk2-ovmf`, `/usr/share/edk2-ovmf/x64`, `/usr/share/OVMF`, `/usr/share/edk2` |
| `AAVMF_DIR` | searched: `/usr/share/AAVMF`, `/usr/share/edk2/aarch64`, `/usr/share/edk2/ArmVirtQemu-AARCH64` |
| `VARS_FILE` with `--aa64` | `build/AAVMF_VARS.fd`, or `.qcow2` where the firmware is qcow2 |

A BIOS rig disk is passed as a drive:

    falcon-efi/tools/run-qemu.sh --bios -drive format=raw,file=build/test-bios.img

The make targets that wrap it:

| target | runs |
|---|---|
| `run` | `run-qemu.sh` |
| `run-secboot` | `run-qemu.sh --secboot`, after `sign` |
| `run-stick` | `run-qemu.sh --stick`; does not rebuild the stick |
| `run-stick-secboot` | `run-qemu.sh --stick --secboot` |
| `esp` | stages `build/esp` and copies the OVMF varstore to `build/OVMF_VARS.fd` |

**The test MOK.** `make mok-test` runs `falcon-efi/tools/mk-test-mok.sh`:
a throwaway keypair in `build/mok-test/` (`falcon-test-mok.key`, `.crt`,
`.cer`), `OVMF_VARS.secboot.mok.fd` (the OVMF Secure Boot varstore with
that certificate in MokList) and `OVMF_VARS.secboot.moksb.fd` (the same
plus `MokSBState`). It needs `openssl` and `virt-fw-vars`, from PATH or a
venv it makes at `build/shim/venv`. Existing files are kept; delete
`build/mok-test` for a new key. `make sign` signs the stick loader with it
as `falcon-boot-signed.efi` (after `sbat-check`), and the station's
`falcon-boot-station-signed.efi` is made the same way. Neither is a release
artifact.

**varstore.** A Secure Boot varstore trusting another certificate (a stick
mkstick made with its own MOK):

    falcon-boot-mkstick varstore <cert.crt> <out.fd> [--template <file>]
        [--virt-fw-vars <path>]

It copies `--template` (default `$OVMF_DIR/OVMF_VARS.secboot.fd`, with
`OVMF_DIR` defaulting to `/usr/share/edk2/OvmfX64`) to `<out.fd>`, adds the
certificate as a MOK with `--virt-fw-vars` (default
`build/shim/venv/bin/virt-fw-vars`), and prints:

    boot it with: MOK_VARS=<out.fd> VARS_FILE=build/vars-run.fd falcon-efi/tools/run-qemu.sh --stick --secboot

It is always rebuilt from the template.

**Rig fixtures.**

| target | makes | needs |
|---|---|---|
| `payload` | `build/test/payload.efi` | |
| `test-iso`, `test-img` | the payload as an El Torito ISO, and a GPT and superfloppy `.img` | sudo |
| `test-uki` | `build/test/fixture-uki.efi`, an sd-stub UKI, and its signed copy | the test MOK; the Arch ISO (`ARCH_ISO`) |
| `test-stick` | `build/test-stick.img`, the four-partition stick with the BIOS stages | sudo |
| `test-station` | `build/test-station.img`, a disk with the station installed | sudo |
| `test-bios-img` | `build/test-bios.img` (MBR) and `build/test-bios-gpt.img` (GPT) | no root |
| `test-bios-probe` | `build/memprobe.iso`, a medium that reports the memory map it is handed | `xorriso` |
| `test-appliance-stick` | `build/appliance/appliance-stick.img` | sudo |
| `test-appliance-stick-aa64` | `build/appliance/appliance-stick-aa64.img`, the same for AArch64 | sudo |

**Rigs.** Each boots headless and prints a PASS, FAIL or SKIP line per
case, judged from serial.

| target | boots | needs |
|---|---|---|
| `test-core` | the stick under both firmwares alone, beside a second stick, an MBR disk and another install's records; a 4Kn stick on NVMe; the station; the boot-timing variables | sudo; the fixture UKI and test MOK |
| `test-station-uefi` | the UEFI station's cases, Secure Boot off and on | `build/test-station.img` (made when absent) |
| `test-station-installed` | stations the installer put on GPT, 4Kn GPT and 4Kn MBR disks | sudo |
| `test-tpm-rig` | the TPM probe and the UEFI station's TOTP check, swtpm as TPM 2.0 (TIS, CRB) and 1.2, Secure Boot off and on | `swtpm` (else one SKIP line); `build/test-station.img` for the station cases |
| `test-aa64` | the AArch64 station's cases from the station, installed and TPM rigs, on virt under AAVMF, emulated | sudo; `swtpm` |
| `test-station-bios` | the BIOS station's chains, drive swap, STATION.INI and next boot under SeaBIOS | |
| `test-station-fixtures` | the BIOS station in front of the fixture disks of `falcon-bios/tools/fixtures/` (FreeDOS, Windows 98 SE) | sudo (`qemu-nbd`) |
| `test-station-legacy` | station-legacy on a qcow2 overlay of the A21m's disk, every row | sudo; the A21m image (`A21M_ORIG`, default `/home/vifair/a21m-image/DJSA-210.img`); the rig QEMU for its CF and menu cases; the devlink client (`DEVLINK_CLIENT`) for its DevLink case |
| `test-cfstation` | station-legacy on a CF card through jump's CF route, the A21m's disk behind it | the rig QEMU; the A21m image (`A21M_IMG`, default `~/a21m-image/DJSA-210.img`) |
| `test-station-bundle` | a station-legacy updated from the bundle by its own `update.sh`, and its refusals | sudo; memtest86+ at `MEMTEST` (default `build/memtest/mt86p_810_i586`); it builds the bundle unless `BUNDLE` names one |
| `test-jump` | every falcon jump route under SeaBIOS | the rig QEMU for the CF case; `MEMTEST` for the memtest case |
| `test-devlink` | DevLink recovery off the rig stick on an i586 with 32 MB, and its kernel on the rig QEMU with the PC Card bridge model attached, the CF card brought up as a disk | the ISO (`DEVLINK_ISO`) and client (`DEVLINK_CLIENT`); the rig QEMU for the bridge case; skipped without them |
| `test-appliance` | the appliance stick under Secure Boot, two appliance runs, then the stick checked on the host | `test-appliance-stick`; sudo |
| `test-appliance-aa64` | the same on QEMU's virt under AAVMF, emulated | `test-appliance-stick-aa64`; sudo |

The rig QEMU is `qemu-system-i386` with a model of the A21m's PC Card
bridge. Build it once:

    falcon-bios/tools/qemu-pcc/build-qemu.sh

It fetches the QEMU release matching the `qemu-system-i386` on PATH
(`QEMU_VERSION` overrides) and builds `build/qemu-pcc/build/qemu-system-i386`,
and that release's SeaBIOS with one patch for the bridge's header
(`falcon-bios/tools/qemu-pcc/seabios-cardbus.patch`) into
`build/qemu-pcc/share/`, which the rigs give it with `-L` before
`/usr/share/qemu`. A case that needs it and does not find it is a SKIP, or
for `test-cfstation` an exit.

**Test-only flags.** These point the TPM commands at the rig instead of the
machine. They are not for use on a real station.

| command | flag | instead of |
|---|---|---|
| `station totp enroll`, `reseal`, `remove`, `check` | `--tpm-socket <path>` | `/dev/tpmrm0` or `/dev/tpm0`: swtpm's socket |
| `station totp enroll`, `reseal` | `--event-log <file>` | the firmware event log from securityfs |
| `station totp enroll`, `reseal` | `--saved-log <file>` | the station's saved event log from efivarfs |
| `update` | `--tpm-socket <path>`, `--event-log <file>`, `--saved-log <file>` | the same, for the reseal |

With `--tpm-socket` the event log is not checked against the TPM's PCRs.

### 13.5 The harness

`tools/coverage/harness.sh` runs a command in the harness container
(`tools/coverage/Dockerfile`), privileged, with `/dev/kvm`, the host's
`/dev`, the repository at its own path (the working directory) and the
corpus read-only at `/corpus`:

    tools/coverage/harness.sh [options] [--] [command...]

| option | env | default |
|---|---|---|
| `--build` | | build the image even when it exists |
| `--image NAME` | `FALCON_HARNESS_IMAGE` | `$FALCON_HARNESS_REPO:` the first 12 hex digits of the Dockerfile's SHA-256 |
| | `FALCON_HARNESS_REPO` | `registry.git.airies.net:443/vifair22/falcon-boot/harness` |
| | `FALCON_HARNESS_PUSH` | `1` pushes the image when the registry lacks it (the pipeline sets it) |
| `--corpus DIR` | `FALCON_CORPUS` | `/srv/falcon/corpus` |
| `--build-dir DIR` | `FALCON_BUILD_DIR` | `/srv/falcon/build` when that directory exists, bound over the repository's `build/`; else empty, the repository's own |
| `--sticks DIR` | `FALCON_STICKS_DIR` | `/srv/build/cache/falcon/sticks` when it exists and the build directory is `/srv/falcon/build`, bound over `build/coverage/sticks`; else empty, the sticks stay in the build directory |

An absent image is pulled, and built when the pull fails. It needs
`docker`. The container sees `FALCON_HARNESS=1`, `MAKEFLAGS`, `VARIANT`,
`ARCH_ISO`, `A21M_ORIG`, `W98_IOS_CF`, `FALCON_KEEP`, `FALCON_UDFEDIT_KEEP`
and every `FALCON_CASE_*`, `FALCON_TEST_*` and `FALCON_TUI_*` set on the
host; no other variable of the host's. The corpus directory and
`/dev/kvm` are needed for the corpus run; with a command, a missing corpus
is not mounted and a missing `/dev/kvm` is a warning (a build works, a rig
does not). The case sticks are most of the corpus's disk use, so on the
binhost they sit on the cache disk and are mounted into the container; a
symlink in their place would not resolve inside it. The container runs as
root; every file root wrote under the repository is chowned to the caller
afterwards, whatever the command's exit status. With a command
it runs that command:

    tools/coverage/harness.sh make test
    tools/coverage/harness.sh bash

With none it runs `tools/coverage/run-corpus.sh`, which builds a case stick
per ISO, stages the current loader on each, boots every case and writes
`docs/coverage.md`. Results are under `build/coverage/runs/`; a case with a
result is skipped unless `--force`. Its options:

| option | default |
|---|---|
| `--states "..."` | `off ms own`; `moksb` (Secure Boot on, validation off), `bios`, `bios32` and the AArch64 columns `a64-off a64-ms a64-own` (virt under AAVMF, emulated, each wait `FALCON_CASE_A64_SCALE` (4) times as long) are asked for by name |
| `--routes "..."` | `dnd chain blessed` |
| `--jobs N` | 3, each case taking `FALCON_CASE_MEM` (4 GiB) |
| `--timeout S` | 300 seconds from the keypress to a verdict |
| `--force` | rerun cases that have a result |
| `--no-scorecard` | leave `docs/coverage.md` alone |
| `ID...` | every corpus row |

The corpus is `tools/coverage/corpus.tsv`, fetched and checked against its
pinned SHA-256s with:

    tools/coverage/corpus-fetch.sh [--local DIR]... [ID...]

Its arm64 rows run in the a64 columns alone. Two have no download to
fetch: `efi-shell-aa64-2025.02` is built, in the harness, by
`tools/coverage/mk-shell-iso.sh` (Debian's EDK II shell in an El Torito
image), and `memtest86-11-arm64` is `memtest86-usb.img` out of PassMark's
`memtest86-usb.zip`.

### 13.6 Shim binaries

    falcon-efi/tools/shim-spike/fetch-shims.sh

downloads four pinned shim packages (Fedora shim-x64 16.1-7 and 15.8-5,
openSUSE shim 16.1-4.2, Debian shim-signed 1.51+16.1-2), unpacks them with
`bsdtar`, and puts the binaries in `build/shim/bins/` under vendor-prefixed
names (`fedora-16.1-shimx64.efi`, `fedora-16.1-mmx64.efi`, ...). A package
already downloaded is not fetched again.

`mkstick create` takes shim from `--shim <dir>` (default `build/shim/bins`,
or a release's `/usr/lib/falcon-boot/shim/bins`, which holds the Fedora 16.1
pair alone),
and `station install` from `--shim <dir>` when it is given (it has no
default: a station has no shim unless asked): `shimx64.efi` if present, else
`fedora-16.1-shimx64.efi`. MokManager is the file beside it under the same
prefix and the name the shim launches it by (`fedora-16.1-mmx64.efi`), else
that name alone. The rig's `--secboot` uses the Fedora 16.1 pair.

## 14. Releases

A release is the tag `vX.Y.Z` on `master`, where `X.Y.Z` is
`release_version`. Its pipeline builds the files below from that commit and
attaches them to the GitLab release, from the project's package registry.

| file | what |
|---|---|
| `falcon-boot-X.Y.Z-x86_64.tar.gz` | mkstick and the sign tool, and everything mkstick installs |
| `falcon-boot-X.Y.Z-aarch64.tar.gz` | the same, with mkstick and the sign tool for an arm64 machine |
| `falcon-station-X.Y.Z.tar.gz` | the BIOS station bundle, for a station with only DevLink recovery (Section 7.8) |
| `falcon-jump-X.Y.Z.img` | the falcon jump floppy, 1.44 MB, ready to write (Section 9) |
| `SHA256SUMS` | the four files' SHA-256 |

Every loader in them is unsigned: `mkstick` signs one with the MOK of the
stick or station it installs to (Section 11.1). The signing appliance's UKI
is not in a release; `make appliance` builds it where the stick is made
(Section 4.3).

### 14.1 Checking a download

    sha256sum -c SHA256SUMS

compares the files with the checksums the release page also shows. The
tarball and the station bundle carry their own `MANIFEST`, which their
installers check (`sha256sum -c MANIFEST`), and which catches a file that
changed after it was unpacked.

### 14.2 The main tarball

One directory, `falcon-boot-X.Y.Z/`, holding the files a package installs,
under the paths it installs them to. The two tarballs differ in `usr/bin`
alone: the data is both machines' loaders, so a stick made on either
carries both chains.

| path | what |
|---|---|
| `usr/bin/falcon-boot-mkstick` | mkstick, x86-64 or arm64, linked static |
| `usr/bin/falcon-boot-sign` | the sign tool, x86-64 or arm64, linked static |
| `usr/lib/falcon-boot/bin/` | `falcon-boot.efi` (the stick), `falcon-boot-station.efi`, `falcon-esp-jump.efi`, the same three with `-aa64` for AArch64, the BIOS stages 1 and 2, stage 3 for the stick, the station and station-legacy, and `mod/` (jump's drivers) |
| `usr/lib/falcon-boot/shim/bins/` | Fedora's shim 16.1 and its MokManager for x86-64 and for AArch64, signed by Microsoft's UEFI CA (2011 for x86-64, 2023 for AArch64) |
| `usr/lib/falcon-boot/memtest/` | memtest86+ 8.10: `memtest-i586.bin` for `--memtest`, `memtest-x86_64.efi` for `--memtest-efi` |
| `usr/share/doc/falcon-boot/` | `README.md`, this Manual, `DESIGN.md`, `LICENSE`, `THIRD-PARTY.md` (shim's and memtest86+'s licences and sources) |
| `VERSION`, `MANIFEST` | the build's version string, and every other file's SHA-256 |

Its mkstick reads its defaults from `/usr/lib/falcon-boot` (Section 10), so
installed, it needs no path options:

    tar -xzf falcon-boot-X.Y.Z-x86_64.tar.gz
    cd falcon-boot-X.Y.Z && sha256sum -c MANIFEST
    sudo cp -r usr/. /usr/

Run from the unpacked directory instead, point it at the files:

    usr/bin/falcon-boot-mkstick station install /dev/X \
        --stage1 usr/lib/falcon-boot/bin/falcon-bios-stage1.bin ...

A stick or station already made is brought to the release with
`falcon-boot-mkstick update` (Section 10.7); its default `--from` is the
installed `/usr/lib/falcon-boot/bin`.

### 14.3 Packages

Each release is also a package, `falcon-boot-bin`, that installs the main
tarball's tree as it is; installing or upgrading it changes no disk, and a
stick or station is brought to it with `falcon-boot-mkstick update`
(Section 10.7) on each machine. Installing and upgrading print that
command, and the TOTP reseal for after the reboot where update's own is
refused.

**Arch.** The tag's pipeline builds it from `packaging/arch/PKGBUILD` into
the `falcon` repository on the dl host (`tools/release/publish-arch.sh`),
for `x86_64` and `aarch64` (Arch Linux ARM), which holds the current
version alone. The packages and the repository
database are signed with the falcon repository key,
`6405 F892 A108 F8FB 0576  5FF8 0EC4 411B 66DB 5626`. Trust it once:

    curl -sO https://dl.airies.net/arch/falcon/falcon.asc
    sudo pacman-key --add falcon.asc
    sudo pacman-key --lsign-key 6405F892A108F8FB05765FF80EC4411B66DB5626

then add the repository to `/etc/pacman.conf` and install:

    [falcon]
    SigLevel = Required DatabaseRequired
    Server = https://dl.airies.net/arch/$repo/$arch

    sudo pacman -Sy falcon-boot-bin

**Gentoo.** `app-misc/falcon-boot-bin` is in vifair-overlay. Its source is
the release in this project's package registry, which is private: the
binhost that builds it holds a read-only deploy token for `git.airies.net`,
and machines install the binary package it builds.

### 14.4 Building a release

    tools/coverage/harness.sh make release \
        MEMTEST=<memtest86+ i586> MEMTEST_EFI=<memtest86+ x86-64>

writes the four files to `build/release/`. It builds in the harness
container because the static programs carry its libc. It needs the shim
packages (`fetch-shims.sh`, Section 13.6) and memtest86+'s release binaries
(`mt86p_810_i586`, `mt86p_810_x86_64`), and refuses without them. The tag's
pipeline runs the same target with the fixture store's memtest86+, checks
that the tag is `release_version`'s, and publishes with
`tools/release/publish.sh`.

