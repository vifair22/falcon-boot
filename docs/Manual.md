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

The passphrase prompt (`POS`, on every loader and station) takes five wrong
passphrases a boot, however often the row is chosen again. After each of
the second, third and fourth the next attempt waits 4, 8 and 16 seconds
(the first costs nothing); the fifth
powers the machine off (`docs/DESIGN.md` Section 5.2). Neither is said on
screen: the wait runs under `unlocking...`, and every refusal is

    wrong passphrase.

On the UEFI station the TOTP code above the prompt is redrawn when it
rolls.

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

The BIOS loader is one image for every i586 or later processor,
and boots 32-bit and 64-bit kernels alike. On a processor with no 64-bit
mode, a 64-bit kernel is refused before it starts, on `DIR` or `POS`:

    direct boot failed while starting the kernel

    the kernel is 64-bit and this processor has no 64-bit mode

(`personal OS boot failed while starting the kernel` on `POS`). A key
returns to the menu. A processor older than i586 stops before the
menu, on screen and serial:

    falcon-bios: this processor is older than i586, which this loader needs
    press any key to reboot.

### 1.3 Fallthrough

| loader | shown first | then | when |
|---|---|---|---|
| UEFI | `D3A` | `D3B` | the ISO's kernel has no `.sbat`, so its shim would not run it; also no boot entry the loader can take, or no shim on the ISO |
| UEFI | `DIR` or `D3B` | `CHN` | no boot entry in the ISO's configuration the loader can take, or (`D3B`) its grub.cfg cannot be replaced in place |
| both | `WIN` | `CHN` | Windows media with nothing to patch |
| UEFI | `BSD` | `CHN` | the medium is not one the memory-disk route applies to |
| BIOS | `BSD` | `DIR` | as above |
| BIOS | `DIR` | `CHN` | no boot entry in the ISO's configuration the loader can take, or its kernel is not a Linux kernel the loader can start (DOS, BSD, anything else) |

A route falls through only before it has committed to the medium. A
failure after that is reported on screen and the key returns to the menu.

A code seen on an error screen names the route that failed, which is the
first thing to report with it. `docs/DESIGN.md` Section 11 draws each
route.

### 1.4 Stationary build

The build for a machine's own disk (`docs/DESIGN.md` Section 12):
`falcon-boot-station.efi` for UEFI, and `falcon-bios-station-stage3.bin` as stage 3
behind the stick's stages 1 and 2 for BIOS.

UEFI opens at its list, with `STATION.INI`'s default highlighted and, when
it sets a timeout, "Booting in 5 s. Any key stops the countdown." in the key
line (`docs/DESIGN.md` Sections 12.3 and 12.7):

| code | route | reached by |
|---|---|---|
| `POS` | a UKI from a shelf of this install, as on the stick | `Personal OS (encrypted)` (the prompt, then the chooser), or a plaintext shelf's own row per UKI |
| `NXT` | a firmware boot entry: BootNext is set to it and the machine restarts | its row, labelled with the entry's own description |
| `ESP` | an image on an ESP, loaded by device path: Windows Boot Manager, or a `STATION.INI` entry's `efi` | a `method = efi` entry, or `Windows Boot Manager (disk gpt:1a2b3c4d, partition 1)` for an ESP no firmware entry names |
| `MEM` | memtest86+, `\EFI\falcon\memtest.efi` on the station's ESP | `Memory test (memtest86+)` |

Every entry the firmware's own boot menu offers is a row (active, not
hidden, boot category), except the one this boot came through and the
station's own (`falcon`). A `FalconNextBoot` naming an entry boots it with
no list. `ESP` says, before it loads `bootmgfw.efi`:

    BitLocker will ask for the recovery key if the volume is sealed.

A `STATION.INI` that cannot be taken is said once, before the list, which
then waits with no default:

    STATION.INI line 2: unknown key 'bogus'.

    The menu will wait with no default.

A page left waiting five minutes with no key powers the machine off, on
every loader (`docs/DESIGN.md` Section 7.9); `[general] poweroff = <seconds>`
changes that on the station, and `poweroff = 0` turns it off.

`STATION.INI` names a found row by its generated name: `memtest`,
`nxt-<4 hex>` for a firmware entry, `uki-<stem>` for a plaintext UKI,
`mbr-<signature>[-p<n>]` or `gpt-<8 hex>[-p<n>]` for a disk or partition,
and `pos` for `Personal OS (encrypted)`. A section of that name changes the
row; `default`, `order` and next boot take it. To keep one prompt on the
list when a `uki` section sets the default:

    [general]
    default = linux
    timeout = 5

    [linux]
    uki = gentoo.efi

    [pos]
    hide = yes

BIOS opens at the passphrase prompt; Escape there goes to the list.

BIOS opens at its list the same way, `STATION.INI` read from the station
partition's root (`docs/DESIGN.md` Sections 11.5.2 and 13):

| code | route | reached by |
|---|---|---|
| `POS` | a UKI off a shelf carrying the station's install ID, as the stick's personal OS | a plaintext shelf's UKI rows, or an encrypted shelf's `Personal OS (encrypted)` row (the prompt, then the chooser) |
| `VBR` | a primary partition's boot sector: the disk's LBA 0 at 0000:0600 with the entry marked active, DS:SI at it, the boot sector at 0000:7C00 with its BPB's hidden sectors and drive number made to agree | a row such as `Windows (NTLDR) (disk mbr:cdcccdcc, partition 1)` |
| `MBR` | another disk's own boot code, started from 0x7C00 | a row such as `Boot code (disk mbr:1a2b3c4d)` |
| `MEM` | memtest86+, `MEMTEST.BIN` in the station partition, as jump starts it | the Memory test row |

A partition is a row when its boot sector names a known loader (BOOTMGR,
NTLDR, WINBOOT.SYS, FreeDOS's KERNEL.SYS, IO.SYS or IBMBIO.COM, GRUB,
SYSLINUX); another disk is a row when its LBA 0 has boot code and an active
entry. `STATION.INI` adds the rest. A chain of a disk that is not 0x80
swaps that disk with 0x80 for everything it boots (`swap = no` in its
section turns that off), and leaves 1 KB of conventional memory taken: the
BDA's count at 0x413 reads 1 KB less.

### 1.5 falcon jump

The boot floppy (`docs/DESIGN.md` Section 13), `build/bin/falcon-jump.img`
from `make jump`. Written to a floppy with `dd if=build/bin/falcon-jump.img
of=/dev/fd0`; it is an ordinary FAT12 volume after that, and its files are
changed with `mcopy` or by mounting it.

| code | route | reached by |
|---|---|---|
| `MBR` | a disk's own boot code, started from 0x7C00 with DL naming that disk | its row, `Disk 0x80` and so on |
| `MEM` | memtest86+, `MEMTEST.BIN` on the floppy, started through the x86 boot protocol | the Memory test row |
| `CDR` | the disc in an IDE CD drive, through `ATA.MOD` and the stick's virtual CD as drive 0xE0 | the CD drive row |
| `USB` | a falcon stick on a USB port, through `EHCI.MOD`, `UHCI.MOD` or `OHCI.MOD`, as the next BIOS disk | the USB stick row |
| `PCC` | a CompactFlash card in a PC Card slot, through `PCC.MOD`, as the next BIOS disk | the CF card (PC Card) row |

Every BIOS disk whose first sector carries the boot signature is a row, the
CD drive row is there when the floppy holds `ATA.MOD` (which `make jump`
copies on) and the machine has an IDE controller, and the Memory test row
when the floppy holds a `MEMTEST.BIN`. The CD
route boots the disc in the first drive on the legacy IDE channels that has
one: a no-emulation disc (every Linux, BSD and Windows 2000-on disc), which
needs 16 MB of memory since the driver stays resident at 15 MB, or a
floppy-emulation disc (DOS and Win9x boot discs, vendor utility CDs), which
is read into memory above 64 MB and so needs more than that. The USB stick row is there when the floppy holds
`EHCI.MOD`, `UHCI.MOD` or `OHCI.MOD` and the machine has that controller; the route
boots the first falcon stick (an ISO library or a shelf in its GPT) on a
root port of any of them, or behind a hub on one, as the BIOS disk after the
machine's own, and needs 16 MB like the CD. A USB keyboard works once the
route has started, on a root port or behind one hub (a hub inside the
keyboard included), for the stick's loader and whatever it starts that
reads the keyboard through the BIOS; DOS under EMM386 does not see it. A
hub or a keyboard on a USB 2.0 (EHCI) port is handed to that port's USB
1.1 companion controller, where the hub runs at full speed. When the route
comes back to jump's menu, the USB keyboard stops with it until a reset,
and only a PS/2 one answers. The CF card (PC Card) row is there when the floppy
holds `PCC.MOD` and the machine has a CardBus bridge; the route boots the
first CompactFlash card in a slot as a disk, its own MBR whatever is on it,
and needs 16 MB like the others. While it reads the card's configuration it
uses memory at 0xD0000, and the card's registers sit at I/O 0x1E0-0x1EF. `make
jump MEMTEST=<file>` copies one on; it has to be a build carrying the x86
boot protocol header (memtest86+ 6 and later), and the 32-bit build for a
processor with no 64-bit mode.

`JUMP.INI`, when the floppy has one, sets a default and a countdown:

    default=hdd        ; hdd, memtest, cd, usb or cf
    timeout=5          ; seconds; any key stops the countdown

`hdd` is the first disk row, `cd` the CD drive row, `usb` the USB stick row, `cf` the CF card row. Without the file, or with a default whose row
is not on the menu, the menu waits. A line the file cannot take is named on
screen before the menu, which then waits with no default. `make jump
JUMP_INI=<file>` copies one on.

## 2. Installing the station

The station (`docs/DESIGN.md` Section 12) is falcon as a machine's own boot
loader, beside the systems already on its disks. `falcon-boot-mkstick
station install` puts it there (3.6); this section is the order a person
does it in. `/dev/X` is the machine's disk.

### 2.1 What it needs

| item | where it comes from |
|---|---|
| `falcon-bios-stage1.bin`, `falcon-bios-stage2.bin` | `make bios` |
| `falcon-bios-station-stage3.bin`, `falcon-boot-station.efi` | `make station` |
| `falcon-boot-mkstick` | `make` |
| Fedora shim 16.1 and its MokManager (UEFI, unless `--no-shim`) | `falcon-efi/tools/shim-spike/fetch-shims.sh`, into `build/shim/bins/` |
| a MOK key pair (UEFI) | the shelf's (`/falcon-mok.key`, `/falcon-mok.crt`), or `--key` and `--cert` |
| `mkfs.fat`, mtools; `sgdisk` on GPT; `sbsign`, `openssl` on UEFI; `cryptsetup` or `debugfs` for a shelf | the distribution |

### 2.2 Free space

The installer resizes nothing. A BIOS station needs a partition of its own,
64 MiB by default (288 MiB on a disk of 4096-byte sectors), in space no
partition holds, and on MBR a free primary slot; it refuses and says how
much it needs when there is none. On a disk
one Windows partition fills (the A21m's), shrink it first, from any Linux:

```sh
ntfsresize -n -s <new size in bytes> /dev/X1    # a dry run
ntfsresize -s <new size in bytes> /dev/X1
echo '63,<new size in sectors>,7,*' | sfdisk -N 1 /dev/X
```

Windows runs chkdsk once on its next boot. A UEFI station needs no new
partition: it goes into the machine's ESP, which must not be mounted.

### 2.3 Install

```sh
falcon-boot-mkstick station install /dev/X --dry-run
falcon-boot-mkstick station install /dev/X --backup station.bak
```

The dry run prints the plan: where the station partition goes, the stage
sectors, the `STATION.INI` it will write and any disk signature it will
give. Both firmwares are installed where the disk allows it; `--bios` or
`--uefi` asks for one. `--shelf` names the shelf the station boots Linux
from, which is stamped with the station's install ID. Keep the backup file
off the disk it describes.

With Secure Boot on, the first boot through the new firmware entry stops in
MokManager: enroll `\EFI\falcon\falcon-boot.cer` (or run `mokutil --import`
on it before rebooting).

`--no-shim` installs no shim and no MokManager: the firmware entry names
`\EFI\falcon\grubx64.efi`, and with Secure Boot on the firmware's db must
hold `falcon-boot.cer`. Enroll it from firmware setup before the first boot.

### 2.4 Check and remove

```sh
falcon-boot-mkstick station status /dev/X
falcon-boot-mkstick station remove --backup station.bak
```

`status` says what is installed and what each menu offers. `remove` puts
the old boot code back and deletes falcon's files and firmware entry; the
station partition stays until it is deleted. `remove --full` also puts the
partition table back as it was.

## 3. falcon-boot-mkstick

The stick maker (`docs/DESIGN.md` Section 8). `falcon-boot-mkstick
<command> --help` prints a command's options. Every command takes
`--verbose` and `--quiet`.

`make install` puts it and `falcon-boot-sign` in `$(PREFIX)/bin`, building
them first:

    make install                     # /usr/local/bin, mode 0755
    make install PREFIX=/usr         # /usr/bin
    make install DESTDIR=/tmp/pkg    # /tmp/pkg/usr/local/bin, for a package
    make uninstall                   # removes both, same PREFIX and DESTDIR

### 3.1 create

    falcon-boot-mkstick create <target> [--size <size>] [--force] ...

Makes the four-partition stick of DESIGN Section 3 on a block device or an
image file. It draws one random 128-bit install ID and writes it into every
partition's identity record (DESIGN Section 3.1):

| partition | record |
|---|---|
| p1, ESP, FAT32 | role `boot`, in its boot sector at byte 0x1DC and in the FAT32 backup boot sector; stage 3 at sector 32, the marker at sector 8 |
| p2, exFAT | `/.falcon`, role `library` |
| p3, LUKS2 | a `falcon` token in the header, role `shelf` (`cryptsetup token export --token-id 0 /dev/X3` shows it) |
| p3, `--p3-plain` | `/.falcon`, role `shelf` |
| p4 | none |

p1 is formatted with `mkfs.fat -R <n>`: 32 sectors, then room for BIOS
stage 3 of twice the size of `--stage3` (default
`build/bin/falcon-bios-stage3.bin`), rounded up to a whole MiB and never
less than 1 MiB. The stick's stage 3 is about 200 KB, so the room is 1 MiB
(2080 reserved sectors in all). A later stage 3 up to that size installs
without reformatting p1. With no stage 3 built, the 1 MiB floor is used.

A target that already carries an MBR or a GPT partition table, or a table
that does not read cleanly, is refused without `--force`. The GPT names
(`FALCON-ESP`, `FALCON-ISO`, `FALCON-UKI`, `FALCON-ROOT`) are for people;
nothing reads them.

### 3.2 bios

    falcon-boot-mkstick bios <target> [--backup <file> | --no-backup]
        [--reserve] [--remove] [--dry-run] [--force]
        [--stage1 <file>] [--stage2 <file>] [--stage3 <file>]

Installs falcon-bios on a disk whose `boot` partition exists (DESIGN
Section 3.3). It finds that partition the way stage 2 does: the first ESP-
or falcon-boot-typed partition (GPT) or FAT-typed one (MBR) whose boot
sector carries a `boot` record naming a stage 3 sector (an ESP holding a
UEFI station's record, stage 3 at 0, is passed over). It writes:

| what | where |
|---|---|
| stage 1 | LBA 0, bytes 0-439, with stage 2's first LBA (LE32 at 0x1B2) and length in sectors (LE16 at 0x1B6); the disk signature, table and `0x55AA` are kept |
| stage 2 | LBA 34 on GPT, LBA 1 on MBR, its real length; it must end before the first partition |
| stage 3 | the boot partition's reserved sectors, from the sector its record names; it must end inside them |
| the marker sector | the one the record names, zeroed |

It refuses a disk whose sectors are not 512 bytes, a stage 2 that would
reach the first partition, and a stage 3 that does not fit the reserved
sectors (it prints the `mkfs.fat -R` count that would fit).

Every sector it writes is saved first to `--backup` (default
`falcon-bios-backup-<target>.bin` in the current directory). `--remove`
restores that file, LBA 0 last; with no backup it clears what an install
wrote (the stage 2 run LBA 0 names, stage 3 by its header's length, the
marker, LBA 0's boot code). `--reserve` (GPT only) claims stage 2's gap
as a BIOS boot partition, slot 5, named `FALCON-BIOS`; `--remove` deletes
a `FALCON-BIOS` partition in whatever slot it is.

### 3.3 status

    falcon-boot-mkstick status <target> [--no-esp]

Read-only. Reads the table (MBR or GPT) and every partition's record with
the loaders' own code, and reports:

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

It exits nonzero when anything fails.

### 3.4 ident

    falcon-boot-mkstick ident <target>
    falcon-boot-mkstick ident <target> --write-boot <n> --id <hex|new>
    falcon-boot-mkstick ident --reserved-sectors [--stage3 <file>]

With a target alone, prints one line per partition, tab-separated: number,
role (`boot`, `library`, `shelf`; `-` for none; `bad` for a record that
does not decode), install ID (`-` for none) and first LBA. Scripts find a
partition by its role this way; the rig and corpus scripts do
(`tools/rig-part.sh`). No root is needed on an image file.

`--write-boot <n>` writes the `boot` record, with the given install ID or a
new one, into partition n's FAT boot sector and its FAT32 backup, and prints
the ID. The partition must be ESP-, falcon-boot- or FAT-typed, and
formatted with enough reserved sectors. The other two records are text: a
`/.falcon` file holding `falcon 1 <role> <32 hex digits>`, and the LUKS2
token of 3.1.

`--reserved-sectors` prints the `mkfs.fat -R` count of 3.1 for `--stage3`.

### 3.5 next-boot

    falcon-boot-mkstick next-boot [--sector] [--disk <dev>] <value>
    falcon-boot-mkstick next-boot [--sector] [--disk <dev>] --clear
    falcon-boot-mkstick next-boot [--sector] [--disk <dev>] --print

Asks the loader to start one entry on the next boot instead of drawing the
menu (DESIGN Section 6.3.1). The value is a path on p2, as the menu lists
it, or `:personal` for the personal OS. It must be printable ASCII; the
loader clears the marker before acting on it and matches it against what
it finds, as a menu pick would be. Needs root.

- **UEFI** (the machine booted with UEFI: `/sys/firmware/efi` exists): sets
  the `FalconNextBoot` variable (vendor GUID
  `50614485-081b-4b7c-8b54-2abeffdf2fb3`; non-volatile, boot service and
  runtime access) through efivarfs. `--clear` deletes it.
- **BIOS** (no `/sys/firmware/efi`, or `--sector`): writes the marker
  sector of the disk's `boot` partition, at the sector its record names:

      falcon-next-boot 1
      <the RTC's reading, /sys/class/rtc/rtc0/since_epoch>
      <the value>

  and zeros to the end of the sector. The marker is one whole sector of the
  disk: 512 bytes, or 4096 on a 4Kn disk, the text in its first 512. The
  loader honours it for ten minutes after it was written. Without `--disk`
  every disk is looked at and exactly one must carry a `boot` record. With
  `--disk <dev>` (a disk or an image file) only that disk is looked at,
  whatever else is attached, and it is an error if it has no `boot` record.
  The sector is written only when it is empty or already a marker.
  `--clear` zeros it.

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

This replaces archusb's `archusb-boot-next`, which wrote the BIOS marker to
LBA 2047 with `dd`; a pending marker there is no longer read.

### 3.6 station

    falcon-boot-mkstick station install <disk> [--bios] [--uefi]
        [--size <MiB>] [--shelf <n|disk:n|partition>] [--esp <n>]
        [--memtest <file>] [--memtest-efi <file>] [--key <file> --cert <file>]
        [--passphrase-file <file>] [--legacy [--uki <file>]... [--mods <dir>]]
        [--also <disk>]... [--no-scan] [--backup <file>] [--force] [--dry-run]
        [--no-shim | --shim <dir>]
    falcon-boot-mkstick station remove [<disk>] --backup <file> [--full]
        [--force] [--dry-run]
    falcon-boot-mkstick station status <disk> [--also <disk>]... [--no-scan]

Installs the station on a machine's disk (DESIGN Section 12.9). One new
install ID is drawn and written into every record the station has: the
station partition's, the ESP's and the shelf's.

**BIOS.** What it writes:

| what | MBR disk | GPT disk |
|---|---|---|
| station partition | a free primary slot, type `0x1C` (hidden FAT32), in free space | the falcon boot type `D0ACB249-F895-4E0D-AB0D-CFBB6F48AE36`, named `FALCON-STATION` |
| stage 2 | LBA 1, before the first partition | the `FALCON-BIOS` partition (EF02), made at LBA 34 when the disk has none |
| stage 1 | LBA 0's boot code, table and signature kept | the same |

The station partition goes in the highest free run that holds `--size`
(64 MiB, at least 40), on a MiB boundary where the run allows it and a
4 KiB one where it does not. It is formatted FAT32 with one sector a
cluster and the reserved sectors `bios` uses (3.1): its boot record at byte
0x1DC, the marker at reserved sector 8, stage 3 from 32.

**Sector sizes.** Disks of 512- and 4096-byte sectors, MBR or GPT. Every
LBA above is the disk's own sector: on a 4Kn disk stage 2 is still at LBA
34 on GPT (byte 139264) and LBA 1 on MBR, the reserved sectors, the marker
and stage 3 are 4096-byte sectors, and the station partition's FAT has
4096-byte sectors. FAT32 needs 65525 clusters, and a cluster cannot be
smaller than a sector, so the station partition there is 288 MiB by default
and 272 MiB at least, as a 4Kn disk's ESP is 260 MB. An image file is taken
as 512-byte sectors; a 4Kn image is written through a loop device made with
`losetup --sector-size 4096 -P`. A loop device is an image's: nothing else
of this machine is scanned for it and no firmware entry is made. Its files are
`STATION.INI`, `MEMTEST.BIN` (`--memtest`), and with `--legacy`
(station-legacy, whose stage 3 is `falcon-bios-station-legacy-stage3.bin`)
jump's drivers from `--mods` (default `build/bin/mod`) as `*.MOD` and each
`--uki` in `/uki`.

**UEFI.** Into `\EFI\falcon\` on the disk's ESP (`--esp` names it):
`shimx64.efi`, MokManager under the name shim launches it by, the station
loader under shim's next-stage name (`grubx64.efi`) signed with the MOK,
`falcon-boot.cer`, `STATION.INI` and `memtest.efi` (`--memtest-efi`, signed
with the MOK). The ESP's boot sector gets the station's boot record, with
stage 3 and marker 0. On a block device of a machine that booted with UEFI
a firmware entry named `falcon` is created for `\EFI\falcon\shimx64.efi`;
for an image it prints the `efibootmgr` line to run on its machine. The MOK
pair is `--key` and `--cert`, or read off the shelf (a LUKS2 shelf with
`--passphrase-file` or a prompt). With `--no-shim` there is no
`shimx64.efi` or MokManager, and the entry names `\EFI\falcon\grubx64.efi`.

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
section with `uki = <file>` instead of its generated name: before the
shelf is unlocked the UEFI loader has no row named `uki-<name>`, and a
`uki` section is the row that prompts for the shelf and boots that file.
Nothing found is no default and no countdown, and the file is one comment
line.
Each disk the file names that has signature 0 is given a random one.

**Refusals.** No partition table; sectors other than 512 or 4096 bytes; a
disk already carrying a falcon boot record; no free space for the station
partition (it names the size it needs and the largest free run); no free
primary slot; no room before the first partition for stage 2; a mounted
ESP; `--size` above 4096, or under 40 (272 on a 4Kn disk). It warns when the station partition or
the shelf ends past 8 GB or 128 GiB.

**The backup** (`--backup`, default `falcon-station-backup-<disk>.bin`;
refused when it exists, without `--force`) is written before anything else
and holds, in the disk's sectors: LBA 0; from the table's end to the first partition (stage 2's
area); on GPT the primary and backup tables; the station partition's
reserved sectors and 8 MiB after them; the ESP's boot sector and its FAT32 backup; each other
disk's LBA 0 before its signature. Then, in order: the shelf's stamp, the
station partition, its files, record and stages, the ESP, other disks'
signatures, and LBA 0 last.

**remove** reads the backup (its disk, or `<disk>` when named now), checks
that a partition there carries the backup's install ID (`--force` skips
that), and puts back LBA 0's boot code, stage 2's area and the ESP's boot
sectors; deletes `\EFI\falcon` and the `falcon` firmware entry it made;
deletes the `FALCON-BIOS` partition it made. The station partition stays.
`--full` restores every run: the partition tables whole (the station
partition's entry goes), the station partition's first sectors, and the
other disks' signatures.

**status** is read-only: the install ID and every partition's record, the
BIOS stages where stage 1 and the station partition's record put them, the
ESP's files (`grubx64.efi` is the UEFI install, shim before it or not, and
its signature is checked against `falcon-boot.cer` when `sbverify` and
`openssl` are installed), each `STATION.INI` as the loaders parse it (or the line it
fails at), the shelf carrying the install ID, and the rows each menu shows
with the default marked. It exits nonzero when a stage or file is missing.

**totp** (UEFI; DESIGN Section 12.15):

    falcon-boot-mkstick station totp enroll <disk> [--pcrs 0,2,4,7]
        [--owner-auth-file <file>] [--label <name>]
    falcon-boot-mkstick station totp reseal <disk> [--pcrs ...]
        [--owner-auth-file <file>]
    falcon-boot-mkstick station totp remove <disk> [--owner-auth-file <file>]
    falcon-boot-mkstick station totp check <disk>

Run from a system the station started. The PCR values come from the
firmware's event log, and on a TPM 1.2 from the log the station saved at
boot (`FalconEvLog`) when there is one, alone or after the one Linux reads;
enrollment says which. It refuses when the log has no measurement of
the station's loader (the one `\EFI\falcon\shimx64.efi` starts, else
`\EFI\falcon\grubx64.efi`), or does not account for the PCRs the TPM or
the station read. A station older than the log save saves none: boot the
new station once before enrolling.

What the code is sealed to changes with what is in the chosen PCRs:
firmware (0, 2), the station image (4), Secure Boot's keys and the
authority that verified the station (7), and with 5, the GPT and the boot
variables, so an `efibootmgr` change or a repartition moves it too. After
any of these the station warns instead of showing the code; `reseal` makes
a new one.

`check` runs the station's own read of the seed from the running system and
changes nothing. The TPM refusing on the PCRs is the good answer there: the
index, the storage key and (on 1.2) its secret are right. Any other refusal
is printed with the step that got it, as the station's own line prints it
(`TPM error 0x00000001 in EstablishTransport.`).

### 3.7 update

    falcon-boot-mkstick update <disk> [--from <dir>] [--backup-dir <dir>]
        [--passphrase-file <file>] [--key <file> --cert <file>]
        [--owner-auth-file <file>] [--dry-run] [--yes]

Brings a stick or a station to the loaders in `--from` (default
`build/bin`), in place (DESIGN Section 12.14). What the disk is comes from
its records:

| disk | found by | written |
|---|---|---|
| stick | a boot record, with a library and a shelf of its install ID | the loader on p1; stages 1-3 when LBA 0 holds stage 1 |
| BIOS station | a boot record naming stage 3, with `STATION.INI` | stages 1-3 (`falcon-bios-station-stage3.bin`) |
| station-legacy | the same, with any `*.MOD` on the partition | stages 1-3 (`falcon-bios-station-legacy-stage3.bin`) and the drivers |
| UEFI station | the ESP's boot record, with a loader in `\EFI\falcon` | that loader |

A disk with both stations gets both halves. Anything else is refused, and
so is a disk with a partition it writes mounted.

The UEFI loader is signed with the install's MOK. update unlocks the shelf
carrying the install ID (the passphrase is asked for once, or read from
`--passphrase-file`) and reads the key into memory; it is never written to
a file. `--key` and `--cert` name a keypair instead. The signed loader
must verify against the certificate before it is written, and is read
back after. `sbsign` and `sbverify` must be installed; mtools writes the
ESP.

Before writing, it prints the plan and asks for `yes` (`--yes` skips the
question; `--dry-run` prints the plan and stops). What each half replaces
is saved to `--backup-dir` (default `.`) first:

| backup | undo |
|---|---|
| `<disk>-bios-<stamp>.bin` | `mkstick bios <disk> --remove --backup <file>` |
| `<disk>-stick-loader-<stamp>.efi`, `<disk>-station-loader-<stamp>.efi` | copy it back over the loader with `mcopy` |

It keeps every record, `STATION.INI`, `/uki`, shim, MokManager, the
certificate and the firmware's boot entries. It clears a pending next-boot
marker.

A UEFI station with `[totp]` is resealed to the new loader in the same
run, and prints a new secret for the authenticator (`--owner-auth-file` as
for `station totp`). Run it from the system the station started. If the
reseal fails, the loader is still updated; run `station totp reseal` after
the next boot.


## 4. Signing, and what lives where

`docs/DESIGN.md` argues all of this: its Section 4.1 for the signing rules,
4.2 for the `sbsign` traps, 4.3 for the ESP and 5.3 for the shelf. What
follows is the short operational form.

### 4.1 What carries which signature

| File | Signatures |
|---|---|
| p1 `/EFI/BOOT/BOOTX64.EFI` (shim) | its two Microsoft ones, **plus ours** |
| p1 `/EFI/BOOT/mmx64.efi` (MokManager) | its Fedora one, **plus ours** |
| p1 `/EFI/BOOT/grubx64.efi` (the loader) | **ours only** |
| p3 `/uki/*.efi` | **ours only** |

"Ours" is the stick's own MOK pair, `/falcon-mok.key` and `/falcon-mok.crt`
on p3.

Keeping the vendor signatures on shim and MokManager is what boots the stick
on an ordinary machine, whose db trusts Microsoft. Adding ours is what boots
it on a machine that trusts only its owner's keys, where shim is refused by
the firmware before it can consult MOK at all. On such a machine, enrol
`falcon-boot.cer` in **db**, not only as a MOK, and never sign the stick with
that machine's own db key.

```sh
sbsign --key /efi/falcon-mok.key --cert /efi/falcon-mok.crt \
       --output NEW.efi FILE.efi
sbverify --list NEW.efi | grep '^signature'
```

Check the second line every time. `sbsign` appends and never replaces, so a
second run leaves two identical signatures and nothing complains.
`sbattach --remove` deletes one entry per call. `sbverify --cert` returns
success on a multiply-signed binary for a certificate that signed nothing in
it, so it cannot answer "who signed this": list the signers, or compare
certificate fingerprints with `openssl x509 -noout -fingerprint -sha256`.

### 4.2 Neither partition is a storage location

p1 holds four files and p3 holds five kinds of file. Both lists are closed,
and there being room is not a reason to add to them.

```
p1  EFI/BOOT/BOOTX64.EFI  EFI/BOOT/grubx64.efi  EFI/BOOT/mmx64.efi
    falcon-boot.cer

p3  falcon-mok.key  falcon-mok.crt  uki/*.efi  vmlinuz-*  lost+found
```

Two things on that list surprise people:

- `vmlinuz-*` on p3 is **required**. It is the `ALL_kver` target mkinitcpio's
  pacman script refreshes, and the UKI is rebuilt from it. Removing it breaks
  kernel upgrades silently.
- A UKI is only offered by the loader if its name ends in `.efi`. A rollback
  saved as `something.efi.prev` is unbootable weight.

Files, captures and backups go on p2. Check both with:

```sh
find /mnt/p1 -mindepth 1        # expect exactly the four above
ls -A /efi                      # expect exactly the five kinds above
```
