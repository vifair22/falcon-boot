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
| BIOS | `DIR` | `CHN` | no boot entry in the ISO's configuration the loader can take, or its kernel is not a Linux kernel the loader can start (DOS, BSD, anything else) |

A route falls through only before it has committed to the medium. A
failure after that is reported on screen and the key returns to the menu.

A code seen on an error screen names the route that failed, which is the
first thing to report with it. `docs/DESIGN.md` Section 12 draws each
route.

### 1.4 Stationary build

The build for a machine's own disk (`docs/DESIGN.md` Section 13):
`falcon-boot-station.efi` for UEFI, and `falcon-bios-station-stage3.bin` as stage 3
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

### 1.5 falcon jump

The boot floppy (`docs/DESIGN.md` Section 14), `build/bin/falcon-jump.img`
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
root port of any of them, as the BIOS disk after the machine's
own, and needs 16 MB like the CD. A USB keyboard stops working once it runs:
jump takes the controller from the BIOS, so the machine wants a PS/2 one. The CF card (PC Card) row is there when the floppy
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

## 2. Installing the stationary build by hand

The stationary build (`docs/DESIGN.md` Section 13) is a machine's own boot
loader on its internal disk. There is no tool for this install: every step
below is run by hand, from a Linux live system, against a disk whose
contents are about to be replaced. Read the whole section before the first
command. `/dev/X` stands for the target disk and `/dev/X1`..`/dev/X3` for its
partitions (`/dev/nvme0n1p1` and so on for NVMe).

### 2.1 What it needs

| item | where it comes from |
|---|---|
| `falcon-boot-station.efi`, `falcon-bios-station-stage3.bin` | `make station` |
| `falcon-bios-stage1.bin`, `falcon-bios-stage2.bin` | `make bios`; the stick's, unchanged |
| `falcon-boot-mkstick` | `make`; used here for `keygen`, `ident` and `bios` |
| Fedora shim 16.1 and its MokManager | `falcon-efi/tools/shim-spike/fetch-shims.sh`, into `build/shim/bins/` |
| a MOK key pair | an existing one, or `falcon-boot-mkstick keygen <dir>` |

One MOK pair can serve several machines; each machine enrolls its
certificate once.

Every falcon partition carries an identity record with one install ID for
the whole install (`docs/DESIGN.md` Section 3.1). Draw it once and use it
in 2.3 and 2.6:

```sh
ID=$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')
```

### 2.2 Layout

| part | type GUID | size | holds |
|---|---|---|---|
| p1 | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` (ESP) | 512 MiB | shim, MokManager, the loader |
| p2 | `9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF` (falcon shelf) | 1 GiB | LUKS2, pbkdf2 keyslot, ext4: `/uki/*.efi`, the MOK pair |
| p3 | `CA7D7CCB-63ED-4C53-861C-1742536059CC` (Linux LUKS) | the rest | LUKS2, the OS's root |

The shelf is found by its identity record, never by its number. The first
partition must start at LBA 2048: BIOS stage 2 lives in the sectors before
it. The ESP is the `boot` partition: BIOS stage 3 and the next-boot marker
live in its reserved sectors, so it is formatted with room for them (`-R`,
the count `mkstick create` uses).

```sh
sgdisk -Z /dev/X
sgdisk -n 1:2048:+512M -t 1:EF00 -c 1:ESP \
       -n 2:0:+1G -t 2:9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF -c 2:FALCON-UKI \
       -n 3:0:0 -t 3:CA7D7CCB-63ED-4C53-861C-1742536059CC -c 3:root /dev/X
mkfs.fat -F 32 -n ESP -R "$(falcon-boot-mkstick ident --reserved-sectors \
    --stage3 build/bin/falcon-bios-station-stage3.bin)" /dev/X1
```

### 2.3 The shelf

pbkdf2 with the pinned iteration count, because the loader unlocks it
(DESIGN Section 3). The passphrase must be the root's too: it is handed to
the initramfs, which opens p3 with it.

The shelf's record is a `falcon` token in its LUKS2 header.

```sh
cryptsetup luksFormat --type luks2 --pbkdf pbkdf2 \
    --pbkdf-force-iterations 200000 /dev/X2
printf '{"type":"falcon","keyslots":[],"version":1,"role":"shelf","install_id":"%s"}' \
    "$ID" | cryptsetup token import /dev/X2
cryptsetup open /dev/X2 shelf
mkfs.ext4 -L FALCON-UKI /dev/mapper/shelf
mkdir -p /mnt/shelf
mount /dev/mapper/shelf /mnt/shelf
mkdir /mnt/shelf/uki
install -m 0600 falcon-mok.key /mnt/shelf/
install -m 0644 falcon-mok.crt /mnt/shelf/
```

The OS mounts the shelf at `/efi`, and its kernel hook builds the UKI into
`/efi/uki/` and signs it with the MOK pair there. The archusb repository
has the hook, the initramfs's credential drop-in, and the cmdline that
names p3 and root; the loader's half of the handoff is DESIGN Section 5.1.

### 2.4 The root

Default LUKS2 (argon2id), the same passphrase, and whatever the OS puts
inside. The loader never touches it.

```sh
cryptsetup luksFormat --type luks2 /dev/X3
```

### 2.5 UEFI

Shim loads its next stage and MokManager from its own directory, so all
three go in `\EFI\falcon\`, and the firmware gets an entry of its own
rather than the removable-media path.

```sh
mkdir -p /mnt/esp
mount /dev/X1 /mnt/esp
mkdir -p /mnt/esp/EFI/falcon
cp build/shim/bins/fedora-16.1-shimx64.efi /mnt/esp/EFI/falcon/shimx64.efi
cp build/shim/bins/fedora-16.1-mmx64.efi   /mnt/esp/EFI/falcon/mmx64.efi
sbsign --key falcon-mok.key --cert falcon-mok.crt \
       --output /mnt/esp/EFI/falcon/grubx64.efi build/bin/falcon-boot-station.efi
efibootmgr --create --disk /dev/X --part 1 --label falcon \
           --loader '\EFI\falcon\shimx64.efi'
mokutil --import falcon-mok.cer
```

`mokutil --import` asks for a one-time password; the next boot stops in
MokManager, where Enroll MOK and that password finish the enrollment.
`falcon-mok.cer` is the DER form `keygen` writes beside the pair
(`openssl x509 -in falcon-mok.crt -outform DER -out falcon-mok.cer`
makes it from the PEM).

A Windows install on another disk needs nothing: its firmware entry,
Windows Boot Manager, is a row (`NXT`, Section 1.4).

### 2.6 BIOS

Stages 1 and 2 are the stick's, stage 3 is the stationary one. The ESP
gets its `boot` record first; `bios` finds it by that record and puts stage
3 in its reserved sectors. `--reserve` claims stage 2's gap as a BIOS boot
partition named `FALCON-BIOS`, so no partitioning tool later puts a
partition over it.

`--reserve` rewrites the partition table, so nothing on the disk may be
mounted:

```sh
umount /mnt/esp /mnt/shelf
cryptsetup close shelf
falcon-boot-mkstick ident /dev/X --write-boot 1 --id "$ID"
falcon-boot-mkstick bios --reserve \
    --stage1 build/bin/falcon-bios-stage1.bin \
    --stage2 build/bin/falcon-bios-stage2.bin \
    --stage3 build/bin/falcon-bios-station-stage3.bin \
    --backup bios.bak /dev/X
```

The backup is every sector `bios` wrote, as it was; `--remove --backup
bios.bak` puts them back. A second disk with its own boot code is a row
(`MBR`, Section 1.4).

## 3. falcon-boot-mkstick

The stick maker (`docs/DESIGN.md` Section 8). `falcon-boot-mkstick
<command> --help` prints a command's options. Every command takes
`--verbose` and `--quiet`.

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
sector carries a `boot` record. It writes:

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
it again.

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

  and zeros to the end of the sector. The loader honours it for ten
  minutes after it was written. The disk is the one whose partitions carry
  a `boot` record; `--disk` names it when more than one does, or names an
  image file. The sector is written only when it is empty or already a
  marker. `--clear` zeros it.

This replaces archusb's `archusb-boot-next`, which wrote the BIOS marker to
LBA 2047 with `dd`; a pending marker there is no longer read.
