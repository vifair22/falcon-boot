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
| `falcon-boot-mkstick` | `make`; used here only for `keygen` and `bios` |
| Fedora shim 16.1 and its MokManager | `falcon-efi/tools/shim-spike/fetch-shims.sh`, into `build/shim/bins/` |
| a MOK key pair | an existing one, or `falcon-boot-mkstick keygen <dir>` |

One MOK pair can serve several machines; each machine enrolls its
certificate once.

### 2.2 Layout

| part | type GUID | size | holds |
|---|---|---|---|
| p1 | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` (ESP) | 512 MiB | shim, MokManager, the loader |
| p2 | `9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF` (falcon shelf) | 1 GiB | LUKS2, pbkdf2 keyslot, ext4: `/uki/*.efi`, the MOK pair |
| p3 | `CA7D7CCB-63ED-4C53-861C-1742536059CC` (Linux LUKS) | the rest | LUKS2, the OS's root |

The shelf is found by its type GUID, never by its number. The first
partition must start at LBA 2048: the BIOS stages live in the sectors
before it.

```sh
sgdisk -Z /dev/X
sgdisk -n 1:2048:+512M -t 1:EF00 -c 1:ESP \
       -n 2:0:+1G -t 2:9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF -c 2:FALCON-UKI \
       -n 3:0:0 -t 3:CA7D7CCB-63ED-4C53-861C-1742536059CC -c 3:root /dev/X
mkfs.fat -F 32 -n ESP /dev/X1
```

### 2.3 The shelf

pbkdf2 with the pinned iteration count, because the loader unlocks it
(DESIGN Section 3). The passphrase must be the root's too: it is handed to
the initramfs, which opens p3 with it.

```sh
cryptsetup luksFormat --type luks2 --pbkdf pbkdf2 \
    --pbkdf-force-iterations 200000 /dev/X2
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

Stages 1 and 2 are the stick's, stage 3 is the stationary one. `--reserve`
claims the gap as a BIOS boot partition named `FALCON-BIOS`, so no
partitioning tool later puts a partition over the stages.

`--reserve` rewrites the partition table, so nothing on the disk may be
mounted:

```sh
umount /mnt/esp /mnt/shelf
cryptsetup close shelf
falcon-boot-mkstick bios --reserve \
    --stage1 build/bin/falcon-bios-stage1.bin \
    --stage2 build/bin/falcon-bios-stage2.bin \
    --stage3 build/bin/falcon-bios-station-stage3.bin \
    --backup lba0-gap.bak /dev/X
```

The backup is LBA 0 and the gap as they were; `--remove --backup
lba0-gap.bak` puts them back. A second disk with its own boot code is a
row (`MBR`, Section 1.4).
