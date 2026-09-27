# Station rework: the audit (2026-09-26)

The station (DESIGN Section 13) is being re-scoped, and the partition
identification under every build replaced. This note is the read-only audit
done before the design, so that the redesign is done once: every assumption
in the tree the new model breaks, with where it is, and a proposal to be
decided together. Section 8 records what was decided with the user on 2026-09-26; it
supersedes the Proposal columns where they differ. DESIGN is rewritten from
section 8.

## 0. The model being designed for

| Build | Job | Firmware |
|---|---|---|
| stick | boots media: ISOs, blessing, Windows installers | UEFI, BIOS |
| station | is the machine's boot loader: Windows 98-11, Linux as UKIs on a shelf, memtest86+ | UEFI, BIOS |
| station legacy | the station plus jump's drivers and routes (CD, CF, USB) | BIOS |
| jump | the floppy, for a machine falcon is not installed on | BIOS |

- Entries are found automatically; `STATION.INI` adds a default, a timeout,
  order, and entries on other disks (Windows on a second disk).
- Linux is the UKIs on a shelf, as the personal OS is today, not at p3.
- MBR and GPT disks both. The A21m (one NTFS partition at LBA 63, 62 free
  sectors, nothing free at the end) is the first MBR target.
- Identification: the boot disk is known from the firmware (BIOS: DL; UEFI:
  the loaded image's device). Falcon's partitions carry an identity record
  (magic, role, install ID); the partition type only narrows the search.
  Other disks are named by GPT disk GUID or MBR disk signature plus a
  partition number, never by a BIOS drive number or firmware order.
- No legacy paths: a superseded format or route is removed, and the user's
  media is rewritten.

## 1. Partition tables and identification

| # | Where | Today | Breaks | Proposal | Decision |
|---|---|---|---|---|---|
| A1 | `falcon-core/src/part/part.c:166-287` | GPT only; no MBR reader anywhere in the tree | every falcon partition on an MBR disk, in both loaders and every tool | an MBR reader (primary entries; extended/logical to decide) beside GPT, one entry point that returns either | |
| A2 | `part.h:168-180` `gpt_part` | the only partition record; `number`, `unique`, `name` are GPT's; BIOS pos, scan, win, bsd, vcd all take `const gpt_part *` | an MBR partition cannot reach any route | a table-neutral partition record (start, length, type as GUID or byte, number, disk id) | |
| A3 | `part.c:38-48`, `part.h:185-203` | the header's DiskGUID is not decoded or kept | naming a disk by GPT disk GUID | keep it in the table | |
| A4 | `part.c:166-287` | LBA 0 never read: no protective or hybrid MBR check | a disk with a stale GPT behind a real MBR parses as GPT here and as MBR in UEFI firmware | read LBA 0 first and decide the table the way the firmware does | |
| A5 | `part.c:14-35, 124-155` | roles are table facts: ISO = basic data type + GPT name `FALCON-ISO`; shelf = shelf type GUID | MBR has no GUID or name; identity-record matching | the identity record decides; type narrows | |
| A6 | `falcon-efi/src/discover/partefi.c:170-199, 203-264` | PartitionInfo and the HARDDRIVE node are read only when GPT; an MBR partition gets no role | UEFI station on an MBR disk | read the MBR arm too, or stop depending on the firmware's role facts once the record decides | |
| A7 | `partefi.c:319-358` `part_role_present`, `part_scan_accept` | "does this role exist" is machine-wide; found anywhere, only identified partitions are accepted | an unstamped or MBR station disk with a falcon stick plugged in: the stick's shelf wins | per disk, and moot once the record decides | |
| A8 | `falcon-efi/src/pos/pos.c:215-330`, `apps/station/bios_main.c:155-166` | boot disk's shelf, else any other disk's (DESIGN 13.3); one passphrase tried against every shelf found | ESP on the Windows disk and the shelf on disk B: the station's shelf and a stick's are both "others" | the station's shelf is named by install ID in its config, not found by rule | |
| A9 | `apps/stick/efi_main.c:283-311` | the ISO library is every ISO-role volume on the machine | two sticks' p2 merge into one menu | the boot disk's library only, as the shelf | |
| A10 | `apps/stick/bios_main.c:641-663` | shelf and ISO library may come from different disks | the mix the appliance refuses | one disk, the boot disk | |
| A11 | `falcon-appliance/src/stick.c:90-222` | the first disk in /sys order with both roles; p2/p3 nodes by number; a content fallback on any disk | two sticks: blesses written to the wrong stick | the loader names its disk (credential), or the record's install ID | |
| A12 | extent map: `media/extmap.h:17-31`, `falcon-hook/src/hook.c:299-475`, `falcon-winjump/src/winjumpcore.c:13-45`, `falcon-efi/src/direct/direct.c:243-268`, `falcon-bios/src/iso/scan.c:176`, `win/win.c:237`, `falcon-hook/tools/mkextmap.c` | the ISO partition is named by GPT unique GUID + start LBA; direct.c puts the MBR signature there and the hook's match fails silently | any ISO on an MBR stick | disk id + start LBA (or the install ID), one format for both tables | |
| A13 | `falcon-cli/src/mkstick/create.c:525-557` | "already partitioned" asks only `gpt_parse` | an MBR disk (a Windows disk) reads as empty and is zapped without `--force` | fix regardless of the rest: refuse any disk with a valid MBR table or a filesystem at LBA 0 | |
| A14 | `falcon-cli/src/mkstick/inspect.c:590-697` | status needs a GPT, counts 4 partitions, knows p4 by number | an MBR stick or station | status reads the records | |
| A15 | `part.c` primary header only | no fallback to the backup GPT | a damaged primary header hides every partition | decide: read the backup when the primary fails its CRC | |

## 2. Disks, the boot device, sector sizes

| # | Where | Today | Breaks | Proposal | Decision |
|---|---|---|---|---|---|
| B1 | `apps/station/bios_main.c:125-151`, `apps/jump/bios_main.c:183-201` | disks are 0x80 .. 0x80 + BDA count; the boot disk found by DL equality | DL outside that range (USB emulation): the boot disk is never probed and is listed as another | always probe DL itself | |
| B2 | `apps/stick/bios_main.c:44-51, 756-770` | only 0x80 and 0x81 | disks at 0x82 and up are never seen | every counted disk, as the station | |
| B3 | `apps/station/bios_main.c:79-91, 231-237` | rows labelled and chosen by BIOS drive number | the number shifts when a stick is plugged in | label and target by disk id (A3) and what is found on it | |
| B4 | `falcon-bios/src/mbr/mbr.c:35-49` | a row for any disk whose LBA 0 ends 0x55AA | a GPT data disk, a table with no active partition, a UEFI-only Windows disk: each fails after the jump | offer a disk or partition only when there is boot code to run (active entry with a boot sector, or known loader code) | |
| B5 | `mbr.c:20-23, 69`; DESIGN 13.4 | DL is the disk's current number; no drive swap | DOS, Win9x and NTLDR's `rdisk(N)` expect 0x80 | the drive swap (13.6 rejected it; now required) | |
| B6 | `falcon-bios/src/disk/disk.c:23-35, 250-262` | AH=48h's total sectors discarded | no end-of-disk check; reads past a BIOS's 28-bit limit wrap | keep the size; refuse reads past it and past the addressing the BIOS offers | |
| B7 | `disk.c:98-139, 206-233` | no extensions: CHS, 1024 cylinders (about 8 GB) | anything past 8 GB on such a BIOS | place what falcon reads early on the disk, and say so when it is not reachable | |
| B8 | `falcon-bios/src/start/boot.S:58-80` | stage 1 reads by AH=42h only: no AH=41h check, no CHS path | a BIOS without extensions cannot boot falcon off its hard disk | CHS fallback in stage 1 (12 bytes free; boot_fd.S has the code) | |
| B9 | `falcon-core/src/luks/luks.c:783-797`; `fs/ext4.c:235-287`; `fs/exfat.c:78-90`; `fs/fat.c:246-275`; both loaders' read callbacks (`disk.c:178`, `discover.c:21-35`) | drivers read 512 or 1024 bytes; callbacks take whole sectors only | every unlock and every plain mount on a 4Kn disk, both firmwares | a re-blocking layer under the callback, once, in core | |
| B10 | `falcon-efi/src/discover/discover.c:21-35` | `IoAlign` never checked | a controller with IoAlign > 1 refuses the reads | aligned buffers, or honour IoAlign | |
| B11 | `discover.h:40` `DISCOVER_MAX` 32 | handles past 32 skipped, in firmware order | the boot disk's partitions on a busy machine | boot disk first, or no fixed cap | |
| B12 | `discover.c:85-96, 135-137` | boot disk = the ESP's disk | the station's shelf on another disk (ESP on the Windows disk) | the config names the shelf (A8) | |
| B13 | `falcon-bios/src/hw/ata/atadisk.c:55, 142`; `atacore.c:56-61` | ATA.MOD/PCC.MOD disks: LBA28, 512 bytes | legacy build on a disk over 128 GiB behind a module | LBA48 when the station legacy build serves disks | |

## 3. Fixed places on the disk

Current sizes: stage 1 428 of 440 bytes; stage 2 28 sectors (14100 bytes
with its CRC); stage 3 compressed: stick 386 sectors, station 93, jump 78.

| # | Where | Today | Breaks | Proposal | Decision |
|---|---|---|---|---|---|
| C1 | `falcon-bios/src/start/biosdefs.h:53` `STAGE2_LBA` 34 | GPT's number | the MBR layout | stage 2 right after LBA 0 on both tables? (on GPT it must stay past the entry array) | |
| C2 | `biosdefs.h:57` `STAGE2_SECTORS` 64; `boot.S` reads all 64; `mkstick` zeroes the run | from LBA 1 the run reaches LBA 63-64: the Windows partition's boot sector | stage 1 reads stage 2's real length, written in by the installer (boot_fd.S does this) | |
| C3 | `biosdefs.h:160, 164` `STAGE3_LBA` 98, `STAGE3_MAX_SECTORS`; `load3.c:63, 72-74` | compile-time | stage 3 in a partition found at run time | stage 2 finds stage 3: in the falcon partition's reserved sectors, located by the table and the record | |
| C4 | `biosdefs.h:175` `NEXTBOOT_LBA` 2047; `marker.c:45-48, 154-167`; the writer is archusb's `archusb-boot-next` (external, `dd` to 2047, unconditional) | the gap before 2048 | inside Windows' NTFS on the A21m | move the marker (inside LBA 1-62, or a file in the falcon partition) and change archusb's writer with it | |
| C5 | literals: `falcon-bios/tools/run-jump-rig.sh:60-64, 242-257`, `falcon-cli/src/mkstick/inspect.c:533-534`, `bioscore.c:14` `GPT_FLOOR_LBA` | the LBAs written three more times | the next change to them | one source (biosdefs.h) or the records | |
| C6 | `falcon-bios/build.mk:366-381` `test-bios.img` | no partition table; the coverage harness's BIOS boot disk | the new stage 2 lookup has nothing to find | the rig disks get a table and a falcon partition | |
| C7 | `falcon-cli/src/mkstick/bios.c:217-627`, `bioscore.c:68-81, 132-135` | GPT only; floor 34; stage 3 straight after stage 2; `--reserve` is EF02 slot 5 | every MBR install | the installer (D7) | |
| C8 | `boot.S:193` | "LBA 34 does not hold stage 2" | text | follows C1 | |

## 4. The station's scope

What the station has (`apps/station`): UEFI POS (unlock, chooser, credfs,
shim), NXT (BootOrder rows, BootNext), Firmware Setup, Escape; BIOS POS and
MBR (other disks' LBA 0).

| # | Piece | Today | Proposal | Decision |
|---|---|---|---|---|
| D1 | POS, both loaders | stick-shaped: p3 names, appliance on the chooser, bless-list credential (`manifest.c`), boot-disk-wins with fallback; BIOS has no plaintext shelf and no pbkdf2 check | keep the unlock and chooser; drop the stick-isms; the shelf named by install ID | |
| D2 | NXT, Firmware Setup | as they are | keep; NXT also gives autodetected Windows rows on UEFI | |
| D3 | MBR chain | other disks only; skips the boot disk | keep for other disks; add D4 | |
| D4 | partition boot sector chain (BIOS) | does not exist: no MBR table, no active entry, no MBR copy at 0:0600, no DS:SI to the entry, no hidden-sectors check | new route: every Windows 98-7 on BIOS on the station's own disk | |
| D5 | Win9x | IO.SYS wants 0x80; FAT32 VBR reads its own sector 2 through hidden sectors and picks LBA or CHS by partition type (to verify on hardware) | D4 plus B5 | |
| D6 | UEFI Windows | NXT's firmware entry only; `bootmgfw.efi` through loadops not built | both, per entry (BitLocker's PCR 7 argues for the firmware entry by default) | |
| D7 | installer | none: the Manual's hand procedure starts `sgdisk -Z` (wipes the disk); `mkstick bios` GPT only | `mkstick station install/remove`: beside an existing OS, both tables, backup and uninstall (UEFI: files and boot entry; BIOS: LBA 0, the gap, the partition) | |
| D8 | STATION.INI | nothing; `jumpini.c` is closed over jump's five routes | a general parser (keys, sections, strict errors as jump's); where it lives follows the layout | |
| D9 | countdown | BIOS `screen_menu_timed` exists; the UEFI menu has none | add to UEFI `menu` | |
| D10 | memtest86+ | BIOS `linux.c` does it (jump); the station has no row; UEFI: no route, and an unsigned `.efi` under Secure Boot needs a MOK signature | a row on both; UEFI memtest blessed with the MOK at install | |
| D11 | filesystems | the station links ext4 only (`fs_reg_ext4.c`) | ext4 + FAT (a new registry file) | |
| D12 | legacy glue | `pci_scan`, `boot_cd`, `boot_usb`, `boot_cf`, `boot_module_disk`, `is_falcon_stick` are statics in `apps/jump/bios_main.c` | lift into a falcon-bios module both jump and legacy link | |
| D13 | modules on a hard disk | `jmod_load` reads any FAT volume; slots 15-16 MB, gz staging at 6 MB | read from the falcon partition; check the station's memory map against the slots | |
| D14 | DESIGN 13.6 | rejects a config file, `mkstick install`, a drive swap, chaining `bootmgfw` by default | all four reopened; 13.2's layout and 13.4's model are replaced | |
| D15 | stale docs | 12.5 says the BIOS station is not built; 12.5.1 draws boot-disk-only but the code falls back; the 13.1 table omits `osrel.c`, `nextboot.c`, `manifest.c` | fixed in the rewrite | |

## 5. Tools and rigs

| # | Where | Today | Proposal | Decision |
|---|---|---|---|---|
| E1 | `tools/coverage/states.sh:21-30, 152-161`, `run-case.sh:145-153` | BIOS columns boot `test-bios.img` as 0x80 and find the case stick's ISO library as the second disk | narrowing to the boot disk breaks this: the case stick carries its own stages, or the harness keeps a two-disk case on purpose | |
| E2 | `falcon-efi/tools/mk-test-station.sh` | fresh GPT, `mkstick bios --reserve` | rig per matrix cell (section 6) | |
| E3 | `falcon-efi/tools/mk-test-stick.sh` | GPT, no BIOS stages | a full stick booting under BIOS, scripted | |
| E4 | `falcon-bios/tools/mk-stick32-pos.sh` (removed, decision 22), `mk-test-blessed-stick.sh`, appliance proofs | partitions by number (`${LOOP}p2`, `${DEV}4`) | by record or label | |
| E5 | `falcon-cli/src/sign/main.c:130` | help says `PARTLABEL=falcon-isos` | text | |

## 6. The matrix

Every case a station can meet, and what proves it. An empty Rig cell is a
gap to close before the build is called done.

| Firmware | Table | Sectors | CPU | Build | Installed | Rig today | Rig needed |
|---|---|---|---|---|---|---|---|
| BIOS | MBR | 512 | i686 | station | Windows 98/2000/XP alone on the disk | none | SeaBIOS + a Windows boot-sector fixture on an MBR disk |
| BIOS | MBR | 512 | i686 | legacy | the A21m: Windows 2000 + CD/USB/CF | none | as above + jump's rig cases |
| BIOS | MBR | 512 | x86-64 | station | Windows 7 + Linux UKIs | none | |
| BIOS | GPT | 512 | x86-64 | station | Linux UKIs + Windows on a second disk | mk-test-station (POS only) | + second disk |
| UEFI | GPT | 512 | x86-64 | station | Windows 10/11 + Linux UKIs, one disk | mk-test-station (POS only) | + a bootmgfw fixture |
| UEFI | GPT | 512 | x86-64 | station | Windows on a second disk | none (NXT measured by hand) | |
| UEFI | MBR | 512 | x86-64 | station | Windows 7-11 on MBR | none | |
| any | GPT | 4096 | x86-64 | station | Linux UKIs | none | a 4Kn disk image under both firmwares |
| BIOS | GPT | 512 | any | stick | full four-partition stick | none scripted | E3 |
| any | MBR | 512 | any | stick | a stick on an MBR table | none | if sticks get MBR at all (decide) |

Out of scope, stated so they are not gaps: Windows 98/ME/2000/XP under UEFI;
the legacy build under UEFI; a station on a CD or floppy.

## 7. Decisions to walk

1. The identity record: where it lives per partition type (FAT boot sector,
   LUKS2 header label or token, ext4 label, exFAT), its fields.
2. The layouts, MBR and GPT: where stage 2, stage 3, the config, memtest and
   (legacy) the drivers live; the falcon partition's type and size.
3. MBR extended/logical partitions: read them or not.
4. Sticks on MBR: supported or not.
5. The next-boot marker's new home, and archusb's writer.
6. The drive swap: how, and when it is used.
7. `STATION.INI`: syntax, keys, where it lives, how an entry names another
   disk.
8. The installer: commands, what it backs up, uninstall.
9. UEFI Windows: firmware entry, `bootmgfw.efi`, or per entry.
10. 4Kn: fix in core now (B9), or scope it out.
11. Order of work: core (A, B9) first, then the station, then legacy.

## 8. Decisions (2026-09-26)

Identification
1. Every falcon partition carries an identity record: magic, version, role,
   install ID. One install ID per stick or station, shared by all its
   partitions and random (128 bits); the loader takes the partitions that
   carry the boot partition's ID, on any disk. The partition type only
   narrows the search. (A5-A11, B12)
2. Where the record is: FAT (station partition, ESP, jump's floppy) in the
   boot sector's unused boot-code area; LUKS2 shelf in a falcon token in the
   header's JSON; exFAT ISO library and a plain ext4 shelf in a `/.falcon`
   file at the root, the same format carried in a file.
3. Partition tables: an MBR reader beside GPT, logical partitions read,
   chaining offered for primaries only. A table-neutral partition record
   replaces `gpt_part`. The GPT DiskGUID is kept. A disk with any 0xEE entry
   is GPT (Linux's rule; the Voyager's hybrid stays a Windows-only view). A
   primary header that fails its CRC falls back to the backup. (A1-A4, A15)
4. The extent map carries the ISO library's install ID plus extents;
   falcon-hook and winjump find the partition by record. (A12)
5. The appliance is told its stick's install ID by the loader, in its
   credential; the content fallback goes. (A11)
6. Sticks stay GPT; nothing creates an MBR stick, though the loaders read
   MBR disks.
7. mkstick create refuses a disk with a valid MBR table too. (A13)

Layout
8. One BIOS layout for every build: stage 1 in LBA 0's boot code; stage 2
   before the first partition (the EF02 gap on GPT, LBA 1 on MBR), its real
   length written into stage 1 by the installer; stage 3 in the reserved
   sectors of a FAT partition, found by stage 2 through the table and the
   record. On a stick that partition is the ESP (a --bios-only stick gets a
   small falcon FAT partition, until decision 22 removes --bios-only); on a
   BIOS station it is the station
   partition; on jump it is the floppy's volume. STAGE3_LBA and
   NEXTBOOT_LBA go. (C1-C3, C5)
9. The next-boot marker is a sector in that FAT partition, found by the
   record; the loader clears it with one sector write. A falcon-cli command
   writes it and replaces archusb's `dd` writer. (C4)
10. BIOS station: the station partition holds stage 3, STATION.INI,
    memtest, DevLink recovery and (legacy) the drivers. On MBR it is hidden FAT32
    (0x1C). UEFI station: the machine's ESP, `\EFI\falcon\`; no extra
    partition.

Station
11. Scope: Windows 98-11 (BIOS: a partition boot-sector chain and other
    disks' MBRs; UEFI: the firmware entry by default, `bootmgfw.efi`
    through the shim-verified path per entry), Linux as UKIs on the shelf
    (LUKS2 or plain, both loaders), memtest86+ (UEFI: signed with the MOK by
    the installer).
12. Autodetection: BIOS lists primaries whose boot sector is a known loader
    and other disks whose MBR has boot code and an active entry; UEFI lists
    every active boot entry but falcon's own. STATION.INI (one [section] per
    entry, [general] for default, timeout, order) adds, hides and reorders;
    another disk is named by GPT disk GUID or MBR signature plus partition
    number. The installer writes a signature on a disk that has none.
13. Chaining another disk always swaps it to 0x80; STATION.INI turns the
    swap off per entry. (B5)
14. Installer: `mkstick station install|remove|status`. It needs free
    space and never resizes a filesystem; it backs up everything it
    writes. It warns when the station partition or shelf would sit past
    8 GB or 128 GiB; the loader keeps each disk's size and refuses reads
    past what the BIOS addresses. (B6, B7, D7)
15. Stage 1 gains a CHS fallback. (B8)
16. 4Kn is fixed in core: a re-blocking layer under the read callback, and
    a 4Kn rig disk under both firmwares. (B9)
17. The legacy build is `apps/station-legacy`; jump's route glue moves into
    a falcon-bios module both link; ATA.MOD/PCC.MOD gain LBA48. (D12, B13)
18. Rigs: a copy of the A21m's disk image (Windows 2000) under SeaBIOS, and
    synthetic boot sectors that report DL, DS:SI and the entry. The corpus
    case sticks carry their own stages and boot as the only disk. (E1)
19. Order: core (1-9, 15, 16, the stick rewrite), then the station (UEFI
    and BIOS, the installer), then legacy on the A21m. Each merged and
    rig-proven before the next.

Pulled in, the same day
20. The audit's small fixes go in with the core step: mkstick create
    refusing an MBR disk (A13, also decision 7), sign's `PARTLABEL` help
    (E5), boot.S's DAP comment ("built on the stack"; it is in .text),
    bioscore's size comment, DESIGN 12.5/12.5.1/13.1 staleness (D15).
21. DevLink recovery (devlink-rescue's 32-bit kernel and initrd, the
    image that repo builds as "classic") is wrapped as a
    UKI by falcon's sign tool and is a Linux entry on the station shelf and
    the stick's; the BIOS loader unwraps it as it does any UKI.
22. The 32-bit stick is retired; every machine goes through the 64-bit
    stick. Removed with it: falcon-bios/tools/mk-stick32-pos.sh,
    docs/notes/stick32.md, `mkstick create --bios-only` (so every stick has
    an ESP, which holds its BIOS stage 3) and `--p4-kdf-memory`. The bios32
    corpus column stays: it tests falcon-bios on a Pentium III.
23. Left on their own tracks: the loader replaying a dirty ext4 shelf, the
    corpus harness's fresh-directory failures.
