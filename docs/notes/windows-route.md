# Booting a stock Windows install ISO from a file on exFAT

Research note, 2026-09-13. Decides HOW falcon-boot should make a stock Windows
installer ISO reach a working Setup when the ISO is a plain file on p2 (exFAT),
under the Section 7 invariants (we never sign, we never relax enforcement).

Nothing here is implemented. This note picks the approach and lists what still
has to be proved on the rig.

## 0. What our own rig already measured

Measured today against Win11 25H2 media, WinPE 10.0.26100:

- Stock WinPE mounts our exFAT partition natively and assigns it a drive
  letter. No driver load needed.
- Stock `boot.wim` has no PowerShell and no Storage module, so `Mount-DiskImage`
  scripting is impossible.
- `boot.wim` does contain `vhdmp.sys`, `cdrom.sys`, `exfat.sys`, `diskpart.exe`,
  `drvload.exe`, `wpeinit.exe`, `startnet.cmd`, DISM and wimgapi. There is no
  `winpeshl.ini`.
- Appending a valid fixed-VHD footer to the ISO and running
  `diskpart attach vdisk readonly` succeeded, but no volume appeared. Windows
  will not mount a partitionless fixed disk (superfloppy mounting is
  removable-media only) and the ISO carries no MBR/GPT in front of its UDF
  filesystem.

The last point killed the "no new code, just diskpart" idea. It did not kill the
inbox virtual-disk stack, which turns out to be the answer (Section 1).

### 0.1 Additional facts measured while writing this note

Against `/mnt/980pro/vtoy-backup/Windows/Windows 11 25H2.iso` (7,736,125,440
bytes, volume id `CCCOMA_X64FRE_EN-US_DV9`), on this machine:

- **The ISO's first 32768 bytes are entirely zero.** The ISO 9660 system area is
  free. (`head -c 32768 "$ISO" | tr -d '\0' | wc -c` -> 0.)
- **The ISO is UDF 1.02, 2048-byte blocks.** `7z l -slt` reports
  `Cluster Size = 2048`, `Sector Size = 2048`, `BlockSize: 2048`,
  `ImplementationId: *Microsoft CDIMAGE UDF::Windows NT`, partition at byte
  offset 622592.
- **Its ISO 9660 namespace is a single 135-byte stub file**, `/README.TXT` at
  LBA 25, reading 'This disc contains a "UDF" file system and requires an
  operating system that supports the ISO-13346 "UDF" file system
  specification.' That is the documented `oscdimg -u2` default. There is no
  ISO 9660 fallback view of the install files.
- `sources/boot.wim` is 593,121,462 bytes (allocated 593,123,328);
  `sources/install.wim` is 6,868,632,137 bytes, i.e. well over the FAT32 limit.
- **Every component the recommended route needs is already in the stock
  `boot.wim`**, in both image 1 (WinPE) and image 2 (Setup), WinPE build
  10.0.26100:
  - `Windows/System32/virtdisk.dll` (103,832 bytes, 10.0.26100.5074) - the
    Virtual Disk API. This is the single most important check in the note.
  - `Windows/System32/drivers/vhdmp.sys` (1,058,200) and
    `Windows/System32/drivers/vdrvroot.sys` (124,216), both with their INFs in
    `DriverStore/FileRepository` - the VHD/ISO provider and the Microsoft
    Virtual Drive Enumerator.
  - `Windows/System32/drivers/cdrom.sys` (221,184) and
    `Windows/System32/drivers/udfs.sys` (401,408) - the CD-ROM class driver and
    the UDF filesystem driver that will mount the attached ISO.
  - `Windows/System32/drivers/exfat.sys` (435,584).
  - `Windows/System32/winpeshl.exe` (57,344) in both images, and **no
    `winpeshl.ini` anywhere in either image**.
  - Image 2 has `setup.exe` at its root (`2/setup.exe`, 99,904 bytes) plus
    `2/sources/setup.exe`. So `winpeshl.exe` will run `X:\setup.exe`, which is
    the documented behaviour when no `winpeshl.ini` exists, and
    `2/Windows/System32/winpeshl.exe` is our injection target.

## 1. The reference implementation: what Ventoy actually does

This is the most important finding in the note, and the headline is:

**Ventoy's Windows support does not use a kernel driver at all on Windows 8 and
later. It injects a user-mode executable into `boot.wim` and that executable
calls the inbox Windows Virtual Disk API to mount the ISO as a virtual DVD.**

The ImDisk kernel driver appears only as a Windows 7 fallback.

### 1.1 The two halves

**Half one, in firmware (UEFI stage).** Ventoy publishes the ISO as a virtual
CD-ROM through `EFI_BLOCK_IO_PROTOCOL`. The implementation lives in
`VentoyProtocol.c`; the read path is `ventoy_read_iso_sector`, and it works off
three tables
(https://deepwiki.com/ventoy/Ventoy/2.2-storage-and-device-mapping):

- `ventoy_img_chunk` - the extent map. Virtual ISO sector ranges to physical
  disk LBA ranges, so a fragmented ISO file on the exFAT partition still reads
  as one linear CD.
- `ventoy_override_chunk` - the patch overlay. Specific virtual sectors are
  answered from a precomputed in-memory buffer instead of from the disk, which
  is how Ventoy serves a modified BCD, boot catalog or WIM "without changing the
  source ISO" on disk.
- `ventoy_virt_chunk` - purely virtual sectors that have no backing on disk at
  all, used to append new content that did not fit in the original image.

The firmware's own El Torito handling then finds the ISO's embedded ESP and the
firmware runs `\EFI\BOOT\BOOTX64.EFI`, which for a Windows ISO is Microsoft's
`bootmgfw.efi`.

This half is architecturally identical to what falcon-boot already has (DESIGN
Section 6 step 2, plus the extent-map machinery of Section 6.2). Our current
publisher has no override or virt layer; that is the only structural gap.

**Half two, inside WinPE.** Before the ISO is handed to firmware, Ventoy patches
`boot.wim` *in the virtual byte stream* (not on disk) so that the first
user-mode program WinPE runs is Ventoy's own. From
`GRUB2/MOD_SRC/grub-2.04/grub-core/ventoy/ventoy_windows.c`
(https://github.com/ventoy/Ventoy/blob/master/GRUB2/MOD_SRC/grub-2.04/grub-core/ventoy/ventoy_windows.c):

- `ventoy_collect_wim_patch()` scans the BCD in the ISO's ESP for WIM
  references, so it knows which WIM the boot manager will actually load.
- `ventoy_wimdows_locate_wim()` decompresses the WIM metadata and lookup table
  in memory.
- `search_replace_wim_dirent()` parses `Windows/System32/config/SYSTEM` inside
  the WIM to learn the configured PE shell, and replaces whichever of
  `pecmd.exe`, `peset.exe` or (the default fallback) `winpeshl.exe` that shell
  turns out to be.
- `ventoy_cat_exe_file_data()` builds the replacement by concatenating
  `vtoyjump32.exe` / `vtoyjump64.exe`, a `ventoy_os_param` struct, a
  `ventoy_windows_data` struct, and the *original* executable's bytes.
- `ventoy_update_before_chain()` recomputes the SHA-1 hashes in the WIM dirents
  and lookup table so the patched resource validates.
- The patched metadata and the new resource are published through
  `ventoy_windows_fill_virt_data()`, i.e. as override and virt chunks on the
  virtual CD. The ISO file on disk is never written.

`winpeshl.exe` is the right target for a stock install ISO: with no
`winpeshl.ini` present (which matches our measurement), `winpeshl.exe` runs
`X:\setup.exe` if it exists and otherwise `cmd /k startnet.cmd`
(https://oofhours.com/2020/12/03/windows-pe-startup-revisited/,
https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-create-apps).

### 1.2 What vtoyjump does at runtime

From `vtoyjump/vtoyjump/vtoyjump.c`
(https://github.com/ventoy/Ventoy/blob/master/vtoyjump/vtoyjump/vtoyjump.c):

1. `VentoyJump()` scans its own image for the embedded `ventoy_os_param`
   (GUID plus checksum check in `CheckOsParam()`), recovering the ISO's relative
   path (`vtoy_img_path`) and the identifying disk GUID.
2. `VentoyHook()` walks drive letters A-Z, reads each volume's backing physical
   disk (`GetPhyDiskUUID()` using `DeviceIoControl(..., IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, ...)`)
   and compares the on-disk UUID at offset 0x180 against `vtoy_disk_guid`. The
   match gives the drive letter, and the ISO path becomes
   `sprintf_s(IsoPath, sizeof(IsoPath), "%C:\\%s", Letter, param->vtoy_img_path)`.
   This is the step that depends on WinPE mounting the stick natively; on
   Ventoy that partition is exFAT, exactly like our p2.
3. `MountIsoFile()` dispatches on Windows version:
   - `IsWindows8OrGreater()` -> `VentoyMountISOByAPI()`, which is
     `OpenVirtualDisk(&StorageType, wFilePath, VIRTUAL_DISK_ACCESS_READ, 0, &OpenParameters, &Handle)`
     with a zeroed `VIRTUAL_STORAGE_TYPE` (so the provider is inferred), then
     `AttachVirtualDisk(Handle, NULL, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY | ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME, 0, &AttachParameters, NULL)`.
     The new drive letter is found by XORing `GetLogicalDrives()` before and
     after.
   - otherwise (or on failure) -> `VentoyMountISOByImdisk()`, which copies
     `/ventoy/imdisk/64/imdisk.sys` out of the stick, loads it with
     `LoadNtDriver()`, and runs `imdisk -a -o ro -f "<iso>" -m Y:`.
4. `is_exe_need_recover()` restores the original `winpeshl.exe` from the bytes
   appended to the jump binary (`SaveBuffer2File(LunchFile, Buffer + PeStart, FileSize - PeStart)`)
   and launches it. `winpeshl.exe` then finds `setup.exe`, and `setup.exe`
   finds `\sources\install.wim` on the freshly mounted virtual DVD.

`AttachVirtualDisk` is documented to attach "a virtual hard disk (VHD) or CD or
DVD image file (ISO)", with ISO support added in Windows 8
(https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-attachvirtualdisk).
That is precisely the `IsWindows8OrGreater()` branch above. Two flag rules
matter and both are documented on the enum
(https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/ne-virtdisk-attach_virtual_disk_flag):
`ATTACH_VIRTUAL_DISK_FLAG_NONE` "is not supported for ISO virtual disks.
ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY must be specified" - read-only is
**mandatory**, not a choice - and `PERMANENT_LIFETIME` "will decouple the
virtual disk lifetime from that of the VirtualDiskHandle ... even if all open
handles are closed", which is what lets the jump binary exit without tearing
the mount down.

The resulting device is a real CD-ROM class device. Its hardware ID is
`SCSI\CdRomMsft____Virtual_DVD-ROM_1.0_`
(https://learn.microsoft.com/en-us/answers/questions/3823068/microsoft-virtual-dvd-rom-driver),
so `cdrom.sys` attaches and `udfs.sys` mounts the ISO's UDF filesystem normally.
The stack is `vdrvroot.sys` (Microsoft Virtual Drive Enumerator, a boot-start
driver, https://batcmd.com/windows/11/services/vdrvroot/) plus `vhdmp.sys`.
Ventoy's own code confirms the class from the inside: its `VentoyMountY` path
attaches with `NO_DRIVE_LETTER`, calls `GetVirtualDiskPhysicalPath`, then
refuses to continue unless the path contains the substring `CDROM`
(`Pos = strstr(physicalDriveNameA, "CDROM"); if (!Pos) { Log("Not cdrom phy drive"); return 1; }`)
before assigning a letter with
`DefineDosDeviceA(DDD_RAW_TARGET_PATH, "Y:", "\\Device\\CdRomN")`. The inbox ISO
attach yields `\Device\CdRomN`, full stop.

`diskpart attach vdisk` does not expose this: it is a VHD/VHDX command. The
capability is in the API, not in the shipped CLI, which is exactly why Ventoy
ships a 100 KB executable instead of a script.

### 1.3 Linux vs Windows in Ventoy

- **Linux ISOs**: Ventoy publishes the same virtual CD, then does the hard part
  after `ExitBootServices` - a compressed cpio appended to the initrd carrying
  the extent map and a hook that rebuilds the device with device-mapper. This is
  the "Ventoy tax" DESIGN Section 6.1 already describes, and it is where
  Ventoy's line count lives.
- **Windows ISOs**: no post-`ExitBootServices` problem exists. The Windows boot
  manager reads `boot.wim` off the virtual CD *while still in boot services* and
  materialises it as a RAM disk (`boot.sdi` template plus the WIM appended to
  it; see https://ipxe.org/appnote/wimboot_architecture). By the time WinPE is
  running, the virtual CD is gone and nothing misses it. The media is only
  needed again later, for `\sources\install.wim`, and by then WinPE is a full
  OS that can mount the ISO for itself from the exFAT partition.

**Windows is the easy case, not the hard one.** It needs no runtime injection
framework, no device-mapper, no kernel module matching. It needs one boot-time
byte patch and one user-mode executable.

### 1.4 Secure Boot, and the part of Ventoy we must not copy

Ventoy's default Secure Boot policy is to "fully bypass secure boot", so that
all EFI files boot without any check; setting `VTOY_SECURE_BOOT_POLICY` in the
global control plugin switches it to honouring the firmware's policy
(https://github.com/ventoy/Ventoy/blob/master/SecureBoot.md,
https://www.ventoy.net/en/doc_secure.html). That bypass is exactly the class of
thing DESIGN Section 7.3 forbids and we will not reproduce it.

We do not need to. Under Ventoy's strict policy, Windows ISOs still boot,
because every component the platform verifies is already Microsoft-signed:

- `\EFI\BOOT\BOOTX64.EFI` in the ISO's ESP is `bootmgfw.efi`, signed by the
  Microsoft Windows Production CA and present in `db`.
- `winload.efi`, `ntoskrnl.exe` and every boot-start driver inside `boot.wim`
  are Microsoft-signed and verified by the Windows code-integrity path.
- `boot.wim` itself is data. It is loaded as a RAM disk, not as a signed image.
  Its *contents* are subject to kernel-mode code integrity for drivers, and to
  nothing at all for user-mode executables, because WinPE ships no user-mode
  code-integrity policy by default.

So the patch lands in the one place in the whole chain where no signature is
checked and none is expected to be: a user-mode program inside a RAM disk. We
change no enforcement, hold no key and sign nothing. This satisfies invariants 1
through 3 without argument.

Ventoy is GPL-3.0 (https://github.com/ventoy/Ventoy), which is compatible with
falcon-boot's licensing intent, but we should reimplement rather than vendor:
the useful part is the technique, and Ventoy's implementation is entangled with
its GRUB fork.

### 1.5 Known risk on current media

Ventoy has open reports of Windows 11 24H2 failing at disk discovery
(https://github.com/ventoy/Ventoy/issues/3010) and error 0x80070001-0x4002f
during setup (https://github.com/ventoy/Ventoy/issues/2887). Issue 3010 notes
that YUMI-exFAT 1.0.28, which uses Ventoy's own bootloader, did not reproduce
it, which points at a Ventoy regression rather than an architectural limit.
Treat it as a rig test, not a blocker.

## 2. Candidate matrix: getting a disk image file mounted inside WinPE

The question this section exists to answer is "what can attach an ISO file
living on exFAT as a device that Windows will mount UDF from, inside stock
WinPE". Section 1 already gives away the answer, so this is mostly a record of
why the alternatives lose.

| Candidate | License | Kernel driver signing | In stock WinPE? | CLI | Device class |
|---|---|---|---|---|---|
| **`vhdmp.sys` via `virtdisk.dll` `AttachVirtualDisk`** | Microsoft inbox | n/a, already loaded and MS-signed | **Yes, verified (Section 0.1)** | **None** - `diskpart` does VHD only, `Mount-DiskImage` is PowerShell and absent. Needs ~100 lines of our own code | **CD-ROM** (`SCSI\CdRomMsft____Virtual_DVD-ROM_1.0_`) -> `cdrom.sys` -> `udfs.sys` |
| `diskpart attach vdisk` | Microsoft inbox | n/a | Yes | `diskpart` script | Disk. VHD/VHDX only; measured dead for a footer'd ISO (Section 0) |
| ImDisk (`imdisk.sys`) | MIT-style grant, **plus GPLv2** floppy-emulation code (from VFD) and BSD code from FreeBSD `md`. The shipped binary contains the GPL part (https://github.com/LTRData/ImDisk) | **Attestation-signed.** Binary inspection of `imdisk.sys` 2.1.2 (`DriverVer=05/09/2026`): embedded signature `CN=Microsoft Windows Hardware Compatibility Publisher` -> `Microsoft Windows Third Party Component CA 2014` -> `Microsoft Root Certificate Authority 2010`. Loads with Secure Boot ON | No, must be shipped. But it is **non-PnP**, so `CreateService(SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START)` + `StartService` is the whole install - which is exactly Ventoy's `LoadNtDriver()` | `imdisk -a -o cd,ro -f "<iso>" -m Y:` (native, auto-starts its own service) | `FILE_DEVICE_CD_ROM` + `FILE_READ_ONLY_DEVICE | FILE_REMOVABLE_MEDIA` via `IoCreateDevice`, with `IOCTL_CDROM_READ_TOC` etc. **Bare device object, not a PnP node**: `udfs.sys` mounts it and `GetDriveType` says `DRIVE_CDROM`, but nothing enumerating `GUID_DEVINTERFACE_CDROM` sees it |
| Arsenal Image Mounter (`phdskmnt.sys`) | **AGPLv3** or paid commercial (https://github.com/ArsenalRecon/Arsenal-Image-Mounter) | **Attestation-signed.** `phdskmnt.cat` (`DriverVer=04/21/2026`) chains to `Microsoft Windows Hardware Compatibility Publisher`; the `.sys` is dual-signed with a nested WHCP signature beside Arsenal's own EV cert. Its `cli.inf` even says `; Just to specify files to sign through Microsoft attestant signing portal` | No, and worse: `Class=SCSIAdapter`, `ROOT\PhDskMnt`, **root-enumerated PnP**. `drvload`/`dism` stage it but do not create the devnode; `aim_ll --install` needs `newdev.dll`, whose presence in WinPE 26100 is unverified. Also in the blast radius of the documented WinPE 26100 PnP regression (every injected driver lands in `CM_PROB_REINSTALL`, https://learn.microsoft.com/en-us/answers/questions/5859511/winpe-adk-26100-28000-cm-prob-reinstall-on-all-inj) | `aim_ll -a -t file -f <iso> -o cd,ro -m Y:` - **native, no .NET** (the .NET dependency is `aim_cli` and the GUI, not `aim_ll`) | **True SCSI CD-ROM LUN.** `iodisp.cpp` sets `READ_ONLY_DIRECT_ACCESS_DEVICE` (PDT 0x05) with 2048-byte sectors; `scsi.cpp` implements `READ_TOC`, `GET_CONFIGURATION`, `READ_DISC_INFORMATION`. Architecturally the best of any candidate. (Its own `--help` still says "cd ... not supported"; that text is stale) |
| OSFMount | Proprietary freeware; redistribution granted only in a forum post; contains ImDisk's GPL floppy code | MS-signed since 2018, exact type unverified | No portable build, installer only | `OSFMount.com -a -t file -f ... -o cd -m Y:` | ImDisk fork, same non-PnP `FILE_DEVICE_CD_ROM` |
| WinCDEmu | **LGPL-3.0, the cleanest licence here** | **Dead.** Last release Dec 2017, driver signed by `CN=Sysprogs OU` via GlobalSign with no Microsoft chain - the legacy cross-signed path, removed from kernel trust starting April 2026 | No | `batchmnt.exe <image>` | PnP SCSI bus -> true CD-ROM |
| VirtualCloneDrive | **Redistribution explicitly prohibited** by EULA | unverified | No | undocumented `VCDMount.exe` | true CD-ROM |
| DAEMON Tools Lite | **Non-commercial only**, free build carries adware | unverified | No | `DTCommandLine.exe --mount --type scsi ...` | true CD-ROM |

Three things fall out of this table.

**The inbox option is strictly the best one.** It is already present, already
signed, already loaded, presents exactly the CD-ROM class that UDF needs, needs
nothing redistributed, and carries no licence at all.

**The reason to reject the third-party drivers is not that they cannot be
signed.** Both serious candidates are genuinely Microsoft-attestation-signed in
2026 and would load with Secure Boot ON. The reasons are redistribution (AGPLv3
or GPLv2 obligations on a binary blob we would ship on every stick) and WinPE
install friction - and for AIM specifically, root-enumerated PnP into a WinPE
build with a known PnP regression. Section 3's wall stops *self*-signed and
*cross*-signed drivers; it does not stop these two. Do not misremember this.

**The only thing the inbox option lacks is a CLI**, which is precisely the gap
Ventoy fills with a 100 KB executable and precisely the gap we can fill with our
existing mingw toolchain (Section 6.1).

If a driver is ever genuinely needed, the pick is **ImDisk, not AIM**, despite
AIM's emulation being better engineered: ImDisk's non-PnP design sidesteps both
the `newdev.dll` question and the 26100 `CM_PROB_REINSTALL` regression, and it
is the only candidate with a decade of WinPE field evidence (Ventoy's Windows 7
fallback, iVentoy's documented
`imdisk.exe -a -f Z:\iventoy.iso -o cd -m Y:`, Easy2Boot, RMPrepUSB).

## 3. Driver signing reality in WinPE

Short version: **a kernel-driver route is a dead end for us, and the reason is
not "hard", it is "structurally impossible".**

- WinPE is explicitly inside the kernel-mode lockdown. Microsoft: "In Windows 8
  and Windows Server 2012, including WinPE, the kernel has been locked down to
  prevent malware ... from circumventing Windows operating system security
  requirements for signed drivers."
  (https://learn.microsoft.com/en-us/windows/win32/w8cookbook/secured-boot-signing-requirements-for-kernel-mode-drivers)
- With Secure Boot ON, the accepted roots are Microsoft's own: "Microsoft Root
  Authority 2010, Microsoft Root Certificate Authority, Microsoft Root
  Authority". The legacy cross-signed path survives only if Secure Boot is off,
  or the end-entity cert predates 2015-07-29
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-).
  And that path is being retired outright: "With the April 2026 security update,
  drivers previously trusted by the cross-signed program are no longer trusted
  by default."
  (https://support.microsoft.com/en-us/windows/hardware/drivers/the-windows-driver-policy,
  and the rollout detail - phased audit-then-enforce across 24H2/25H2/26H1 and
  Server 2025 with a curated allow-list -
  https://techcommunity.microsoft.com/blog/windows-itpro-blog/advancing-windows-driver-security-removing-trust-for-the-cross-signed-driver-pro/4504818)
- `bcdedit /set testsigning on` is **refused**, not ignored, under Secure Boot:
  "TESTSIGNING requires disabling Secure Boot"
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/install/preproduction-driver-signing-and-install);
  the write fails with "The value is protected by Secure Boot policy and cannot
  be modified or deleted".
- `nointegritychecks` is documented as "Cannot be set when secure boot is
  enabled. This value is ignored by Windows 7 and Windows 8."
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/bcdedit--set)
- `dism /Add-Driver /ForceUnsigned` only overrides the *install* check, not the
  load check
  (https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/add-and-remove-drivers-to-an-offline-windows-image).
  Offline injection into `boot.wim` changes nothing about CI at load time.
- `drvload.exe`'s reference documents nothing about signing
  (https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/drvload-command-line-options);
  it is an ordinary PnP install and CI enforces at load.

**The point that matters most for falcon-boot: enrolling a certificate in the
UEFI `db`, or in shim's MOK, has *zero* effect on Windows kernel driver
signature verification.** UEFI `db`/`dbx` scope is "UEFI applications, operating
system loaders, and UEFI drivers" verified by *firmware*
(https://uefi.org/specs/UEFI/2.10/32_Secure_Boot_and_Driver_Signing.html); MOK
is a shim-internal construct the Windows kernel never reads. Windows CI carries
a hard-coded root list. The decisive proof is that both Microsoft-sanctioned
ways to change kernel trust require a *separate signed policy file on the ESP*
in addition to owning PK/KEK: the preproduction path needs
`db_MSFTtestSigningRoot.cer` in db **and** a signed
`\EFI\Microsoft\Boot\SecureBootPolicy.p7b`
(https://learn.microsoft.com/en-us/windows-hardware/drivers/install/preproduction-driver-signing-and-install),
and Custom Kernel Signers needs a WDAC policy signed with an OID
`1.3.6.1.4.1.311.79.1` cert deployed to
`\EFI\Microsoft\Boot\CiPolicies\Active\` plus a Windows reset, and is described
as an App Control policy "to override the default signing requirements built
into the Windows kernel"
(https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers).
If db enrollment affected Windows DSE, CKS would not need to exist.

So our own MOK, which is what makes the rest of falcon-boot work, buys us
exactly nothing inside Windows. A self-signed `.sys` cannot load with Secure
Boot ON by any supported means.

One nuance, worth knowing but not worth betting on: Geoff Chappell's
reverse-engineering of CI reports that from 1607 the *WHQL* tightening is
skipped when the `winpe` BCD element is present, which would keep a legacy
cross-signed driver loadable in WinPE even with Secure Boot on
(https://www.geoffchappell.com/notes/security/whqlsettings/index.htm). That is
third-party RE, covers only up to 1803, and its interaction with the April 2026
cross-sign retirement is unverified. Base KMCS (chain to a CI-trusted root) is
enforced regardless, so it never helps a self-signed driver.

Conclusion, stated carefully because it is easy to over-read: this section kills
**self-signed and cross-signed** drivers under Secure Boot, and with them any
idea of building our own `.sys`. It does **not** kill ImDisk or Arsenal Image
Mounter, both of which are verified attestation-signed in 2026 (Section 2) and
would load fine. Those two lose on redistribution and WinPE install friction
instead. What this section does settle for good is that we cannot sign a kernel
driver ourselves, at any price short of a Partner Center account and an
attestation submission, and that our MOK is worthless for the purpose.

## 4. The MBR-in-system-area variant

Idea under evaluation: write a protective MBR into the ISO's first 32 KB
"system area" (the isohybrid trick), append the fixed-VHD footer we already
proved works, and let Windows mount the resulting partition. Verdict: **spec-legal
and cheap to try, but very likely blocked by UDF block size, and it buys the
wrong thing anyway.**

### 4.1 The system area is genuinely free, and on Windows media it is empty

- ECMA-119 (ISO 9660) reserves the first 16 logical sectors (16 * 2048 = 32768
  bytes) as the System Area, unspecified by the standard; volume descriptors
  start at sector 16
  (https://ecma-international.org/wp-content/uploads/ECMA-119_6th_edition_december_2025.pdf,
  https://wiki.osdev.org/ISO_9660).
- ECMA-167, the base standard UDF builds on, says the same: "32768 bytes of the
  Volume space shall not be used for the recording of ECMA-167 structures"
  (https://www.ecma-international.org/wp-content/uploads/ECMA-167_3rd_edition_june_1997.pdf).
- isohybrid exploits exactly this: "ISO9660 sort-of ignores the contents of the
  first 32kiB of its image. That's what allows to cram an MBR ... into an ISO
  image ... without breaking the El Torito boot."
  (https://wiki.syslinux.org/wiki/index.php?title=Isohybrid)

**Verified locally against `Windows 11 25H2.iso` (7,736,125,440 bytes) on this
machine:** the first 32768 bytes are entirely zero (`head -c 32768 | tr -d '\0'
| wc -c` returns 0). The system area is free real estate. A Windows ISO stays
valid and bootable after such a patch, because nothing reads those bytes.

### 4.2 But Windows will not mount the result

Two walls, the second one probably fatal.

**Wall 1, the partition table shape.** Windows does not mount a partitionless
fixed disk; the "superfloppy" allowance is scoped to removable media only
("Removable media must be MBR, GPT, or 'superfloppy' ... Hard disk drives on
external buses such as SCSI or IEEE 1394 are not considered removable",
https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/windows-and-gpt-faq).
That is our measured `diskpart attach vdisk` result, explained. The fix is
known and is what udftools does for UDF-on-hard-disk: an MBR with one partition
that **starts at sector 0** and spans the whole device, so "UDF filesystem can
be mounted either from whole disk device ... or from first partition"
(https://github.com/pali/udftools/blob/master/doc/mkudffs.8,
https://github.com/JElchison/format-udf). A GPT cannot do this: GPT owns LBA
1-33 and no partition can start at LBA 0. So MBR only.

This also settles the device-class question in our favour: `udfs.sys` is **not**
CD-ROM-only. It mounts UDF from disk-class volumes fine. The constraint is the
volume, not the class.

**Wall 2, block size.** Windows mounts UDF only when the UDF block size equals
the device's logical sector size. `mkudffs(8)`: "In most cases, operating
systems are unable to mount UDF filesystem if UDF block size differs from
logical sector size of the device. Typically hard disks have sector size 512
bytes and optical media 2048 bytes." A Windows ISO's UDF uses **2048-byte
blocks** (verified locally: `7z l -slt` on the 25H2 ISO reports
`Cluster Size = 2048`, `Sector Size = 2048`, `BlockSize: 2048`, UDF 1.02). A VHD
attached through `vhdmp.sys` presents 512 or 4096 byte logical sectors, never
2048. Predicted result: no mount even with a correct sector-0 MBR. Nobody
appears to have published a test of this exact combination, so it is an
inference from two sourced statements, not a measurement.

### 4.3 And it modifies the user's ISO

The whole point of falcon-boot's ISO path is that the file the user dropped on
stays byte-identical and SHA-256 verifiable. Patching the system area breaks
that for the one family where users are most likely to be checking hashes
against Microsoft's published values. Even if Wall 2 did not exist, this route
costs the trust property for a result the Section 6 route gets for free.

**Rejected**, with a cheap rig experiment kept in the pocket (Section 6.3) in
case the block-size prediction is wrong, because if it *were* wrong this would
be the only route with no injected code at all.

## 5. Alternative architectures

| Route | How | Drag-and-drop byte-identical ISO? | Secure Boot ON? | Linux-only prep? |
|---|---|---|---|---|
| Extract to partition (Rufus) | Copy the ISO tree onto FAT32 (with `install.wim` split into `.swm` under 4 GB) or NTFS + Rufus' UEFI:NTFS shim | No, ISO is exploded | Yes (Rufus' UEFI:NTFS binaries and its ntfs-3g-derived drivers are MS-signed since 3.17) | Rufus is Windows-only, but `bsdtar`/`wimlib` reproduce it on Linux |
| wimboot (iPXE) | MS-signed loader synthesises a virtual FS with `bootmgr`, `BCD`, `boot.sdi`, `boot.wim`, hands off | Partly: gets WinPE up, still needs the ISO mounted for `install.wim` | Yes, release binaries are MS-signed (third-party UEFI CA, expires 2026-06-27) | Yes |
| bootmgr ramdisk / VHD native boot | BCD `ramdisksdipath` + `ramdisk=[boot]\...` | No, boots a WIM or VHD, not an ISO | Yes | n/a |
| Windows To Go | Full Windows on the stick | No | Yes | Requires NTFS, not exFAT |
| Injected user-mode exe in WinPE (vtoyjump shape) | Patch the in-RAM `boot.wim`, jump binary mounts the ISO from inside Windows | **Yes, fully** | **Yes, nothing verified is changed** | **Yes** |
| iSCSI / iPXE `sanboot` | Remote block device, re-established in-OS from iBFT | n/a | n/a | No server on a stick; also needs NIC drivers stock media lacks |
| EFI virtual CD alone (what we have) | `EFI_BLOCK_IO` CD-ROM over the ISO's extents | Yes | Yes | Yes |
| Easy2Boot `.imgPTN` | Rewrite the stick's partition table to point at a prepared partition image | No, you build the `.imgPTN` first | Yes | Windows tooling |
| grub4dos `map --mem` | Whole image into RAM as an int13 device | Yes | No, BIOS-only | n/a |

Sources for the row claims:
https://github.com/pbatard/uefi-ntfs,
https://github.com/pbatard/rufus/blob/master/res/uefi/readme.txt,
https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/split-a-windows-image--wim--file-to-span-across-multiple-dvds,
https://ipxe.org/wimboot,
https://ipxe.org/appnote/wimboot_architecture,
https://github.com/ipxe/wimboot/blob/master/RELEASE.md,
https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/deployment/windows-to-go/windows-to-go-frequently-asked-questions,
https://ipxe.org/cmd/sanboot,
https://easy2boot.xyz/partition-images/,
https://www.skamilinux.hu/grub4dos/files/boot.htm.

Two findings from this survey matter more than the table.

**5.1 The EFI virtual CD alone gets us further than expected, then stops for a
specific reason.** `bootmgfw.efi` and `winload.efi` run entirely in boot
services and read `boot.wim` off our virtual CD happily; WinPE boots from its
RAM disk. What fails is Setup, afterwards, looking for `\sources\install.wim`
on a device that no longer exists. Easy2Boot names the symptom exactly: Setup
"is looking for the very large install.wim or install.esd ... on all drives in
the system. If it cannot find this file then you will see this message"
(https://easy2boot.xyz/faq/windows-iso-error-a-media-driver-your-computer-needs-is-missing/).
The generic form of the problem is the same one DESIGN Section 6.1 describes
for Linux, and Ventoy's own docs say the virtual CD "becomes inaccessible to the
booting OS" once `ExitBootServices` runs
(https://deepwiki.com/ventoy/Ventoy/2.2-storage-and-device-mapping).

**5.2 Do not build on "bootmgfw reads our exFAT partition".** bootmgr carries
its own filesystem readers over block I/O rather than using the firmware's
Simple File System: "Bootmgr contains a lightweight NTFS file system library
(Bootmgr also supports other file systems, such as FAT, El Torito CDFS, and
UDFS, as well as WIM and VHD files)"
(https://www.microsoftpressstore.com/articles/article.aspx?p=2201310). exFAT is
not in that list. It does appear to have arrived around Windows 10 1809 -
Ventoy's VHD plugin doc states that for "Windows 10 v1803 and earlier ... the
partition storing VHD(X) files must be formatted as NTFS" while "Windows 10
v1809+ ... can also be stored in the exFAT partition", and flags "some
compatibility problem in bootmgr"
(https://www.ventoy.net/en/plugin_vhdboot.html). Undocumented by Microsoft,
version-gated, quirky. Our route must not depend on it, and the Section 6 route
does not: bootmgr only ever reads our virtual CD.

**5.3 The ISO 9660 fallback does not exist for Windows media.** A retail
Windows ISO is built with oscdimg `-u2`, which "produces an image that contains
only the UDF file system. Any system that cannot read UDF sees only a default
text file"
(https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/oscdimg-command-line-options).
Verified locally: the 25H2 ISO's entire ISO 9660 namespace is one 135-byte file,
`/README.TXT`, containing 'This disc contains a "UDF" file system and requires
an operating system that supports the ISO-13346 "UDF" file system
specification.' So `cdfs.sys` mounting the ISO 9660 view would yield a readme,
not `\sources\boot.wim`. Anything we build must go through UDF.

## 6. Recommendation

### 6.1 Primary: Ventoy's shape, with all preparation moved to Linux bless time

Reimplement the vtoyjump mechanism, but split it along our own line: everything
that needs judgement happens in `falcon-cli` on Linux at bless time, and the loader
only applies a data-driven byte map.

**Why this one.** It is the only route that keeps a byte-identical ISO, needs no
Windows host, needs no kernel driver, needs no signature anywhere, and changes
no enforcement. The patch lands in the single place in the Windows boot chain
where nothing is verified and nothing is meant to be: a user-mode executable
inside a RAM disk, downstream of an MS-signed `bootmgfw.efi` -> `winload.efi` ->
signed-kernel chain that we leave completely intact. Invariants 1, 2 and 3 hold
without an argument, and unlike Ventoy we need no security-policy bypass,
because every component the platform actually verifies is already Microsoft
signed. It is also the only route Windows *needs*: Windows has no
post-`ExitBootServices` media problem (Section 1.3), so none of the DESIGN
Section 6.2 injection framework applies here.

**Loader side (new, but small and generic).** The virtual-media publisher grows
two layers that Ventoy already proves are sufficient:

- an *override* list: `(byte offset in virtual image, length, replacement
  bytes)`, answered from a sidecar instead of from p2;
- an *append* region: virtual sectors past the end of the ISO, backed by a
  sidecar file on p2.

Both are pure data. No WIM parsing, no UDF parsing, no Windows knowledge in the
loader. They are also reusable: the same two layers are what the "overlay a
modified grub.cfg" idea in DESIGN Section 7.2 would need, should that ever get
its own design discussion.

**Bless side (Linux userland, `falcon-cli`).** For a Windows family ISO the bless
tool:

1. Locates `\sources\boot.wim` inside the ISO's UDF filesystem and records its
   extent and its UDF File Entry location.
2. Extracts `boot.wim`, and uses wimlib (`wimupdate`) to replace
   `\Windows\System32\winpeshl.exe` with our jump binary, keeping the original
   as e.g. `\Windows\System32\winpeshl_orig.exe`. wimlib appends by default:
   "`--rebuild` ... Rebuild the entire WIM rather than appending the updated
   data to the end of it"
   (https://manpages.debian.org/unstable/wimtools/wimlib-imagex-update.1.en.html),
   so the patched WIM is the original bytes plus a tail plus a rewritten header.
   wimlib is LGPLv3+/GPLv3+ dual licensed and implements XPRESS, LZX and LZMS
   including solid resources (https://wimlib.net/).
3. Emits the sidecar: the appended tail, plus the override entries. Following
   Ventoy's own list in `ventoy_windows_fill_override_data_udf()`, the overrides
   are: the UDF Partition Descriptor's sector count
   (`grub_udf_get_last_pd_size_offset()`), the boot.wim File Entry's information
   length (8 bytes at `fe_entry_size_offset`), the File Entry's short allocation
   descriptors (a `{length, position}` array, extended with one entry pointing at
   the appended sectors), and the WIM header at the file's start.
4. Records the p2 partition identity and the ISO's relative path so the jump
   binary can find the ISO from inside WinPE.

**Jump binary.** A freestanding-ish Win64 console executable, roughly 100 lines:
find the stick's drive letter, build `<letter>:\<relative iso path>`, call
`OpenVirtualDisk` + `AttachVirtualDisk(ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME)`, then `CreateProcess` the saved
original `winpeshl.exe` and exit. `PERMANENT_LIFETIME` is what keeps the mount
alive after our process exits.

We already have the toolchain. Verified on this machine:
`/usr/lib/mingw64-toolchain/x86_64-w64-mingw32/include/virtdisk.h` and
`.../lib/libvirtdisk.a` exist, and the header defines
`VIRTUAL_STORAGE_TYPE_DEVICE_ISO` (1), `VIRTUAL_DISK_ACCESS_READ`,
`OPEN_VIRTUAL_DISK_VERSION_1`, `ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY` and
`ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME`. The same
`x86_64-w64-mingw32-gcc` that builds the EFI loader builds this, so the Windows
route adds zero new toolchain dependencies.

**Trust-model accounting, stated so it is not rediscovered later.** We do modify
what WinPE runs, in RAM, at boot. That is a real thing to document in the
Manual. What we do not do: sign anything, hold a key, change enforcement, touch
the user's ISO on disk, or require a Windows machine. The modification is
confined to the blessed-entry path, is computed by a tool the user runs on a
host they control, and is inert on any machine that boots the ISO some other
way.

This is a DESIGN.md decision, not an implementation detail. Overwriting a
Microsoft-signed `winpeshl.exe` inside a vendor's `boot.wim`, even only in the
bytes we serve, is the same *class* of act as blessing: the user deliberately
authorising a modification to third-party media on media they own. It belongs
in Section 7 next to blessing, with the same framing and the same Manual
treatment, rather than appearing as a surprise in the ISO path.

### 6.2 Fallback: whole-file patched `boot.wim` sidecar

Same architecture, same runtime behaviour, blunter implementation. Instead of a
delta plus UDF File Entry surgery, the bless tool writes a **complete** patched
`boot.wim` and pads it to exactly the original file's UDF-allocated length, so
the override is a single contiguous extent substitution and nothing about the
UDF metadata or the virtual image size changes.

**Why this is the fallback and not the primary.** It costs about 570 MB of p2
per blessed Windows ISO (measured: the 25H2 ISO's `sources/boot.wim` is
593,121,462 bytes, allocated 593,123,328), and it requires the patched WIM to
come out no larger than the original, which means leaning on recompression
(`wimoptimize --solid`) rather than on arithmetic. In exchange it removes every
piece of byte-level UDF and WIM-header surgery from the critical path, which is
where the primary's risk actually lives. If the primary's override list turns
out to be fiddly or version-fragile across Windows releases, switch to this and
lose only disk space.

Both share the jump binary, the bless-time wimlib work, and the loader's
override layer, so the fallback is not a different project; it is a smaller
override list.

### 6.3 What is explicitly rejected

- **Writing our own kernel driver.** Section 3, absolutely and permanently: our
  MOK is invisible to Windows CI, self-signed cannot load under Secure Boot, and
  `testsigning` is refused rather than ignored.
- **Shipping somebody else's kernel driver.** Not because it would fail to
  load - ImDisk and Arsenal Image Mounter are both verified
  attestation-signed - but because it means redistributing a GPLv2 or AGPLv3
  kernel blob on every stick to solve a problem the inbox `vhdmp.sys` path
  already solves, and in AIM's case walking into a known WinPE 26100 PnP
  regression. Held in reserve, not rejected outright: if a rig test ever shows
  `AttachVirtualDisk` unusable, **ImDisk** is the pick (non-PnP, so
  `CreateService` + `StartService` is the entire install, exactly as Ventoy
  does it), and the route stays Secure-Boot-clean because its signature is
  genuine.
- **MBR in the ISO system area** (Section 4). Spec-legal and the system area is
  verified empty, but Windows almost certainly will not mount 2048-byte-block
  UDF off a 512-byte-sector VHD, and it modifies the user's ISO.
- **Extraction to a partition** (Rufus shape). Reproducible on Linux with
  `wimlib` and `bsdtar`, but it abandons drag-and-drop for exactly the family
  where `install.wim` is 6.9 GB and would need splitting.
- **`bootmgfw` reading p2 directly.** Undocumented, version-gated at 1809,
  quirky (Section 5.2).
- **Ventoy's `VTOY_SECURE_BOOT_POLICY=0` bypass.** DESIGN Section 7.3. We never
  need it for Windows.

### 6.4 Open questions that need a rig test

Ordered by how badly a "no" hurts.

*(The one that mattered most - is `virtdisk.dll` in stock `boot.wim` - was
answered while writing this note: yes, along with `vhdmp.sys`, `vdrvroot.sys`,
`cdrom.sys` and `udfs.sys`. See Section 0.1.)*

1. **Does `AttachVirtualDisk` on a 7.7 GB ISO sitting on exFAT actually produce
   a mounted UDF volume inside WinPE?** `virtdisk.dll` being present is
   necessary, not sufficient: the API also needs `SE_MANAGE_VOLUME_PRIVILEGE`
   (WinPE runs as SYSTEM, so fine) and needs `vdrvroot`/`vhdmp` actually
   started. Write the jump binary first and run it by hand from the WinPE
   command prompt, before touching a single WIM byte. This de-risks the entire
   route for an afternoon's work and is the first thing to do. The specific
   failure to watch for is `OpenVirtualDisk` returning
   `ERROR_VIRTDISK_PROVIDER_NOT_FOUND`, which is the return code Ventoy itself
   treats as "not supported, fall back to ImDisk". Note also two documented
   constraints on the host volume: it must not be NTFS-compressed or
   EFS-encrypted. Neither applies to exFAT, but do not lose them if the layout
   ever changes.
2. **Does our existing virtual CD boot a stock Windows ISO to the WinPE splash,
   unmodified, Secure Boot ON?** This should already work today and is the
   cheapest possible check of the whole premise. Expected failure mode is Setup
   saying a media driver is missing, *after* WinPE is up. Do this one and the
   one above on the same rig run: boot the ISO, get to the error, drop to the
   command prompt with Shift+F10, and run the jump binary from p2 by hand.
3. **Does a wimlib-patched `boot.wim` still boot?** Test by replacing the file
   in a rebuilt ISO before doing it through override chunks, so a failure is
   attributable.
4. **How do we find `\sources\boot.wim`'s extent from Linux?** `xorriso` cannot
   help: a retail Windows ISO's ISO 9660 namespace is a one-file stub (Section
   5.3). `7z` reads the UDF but reports no offsets. Candidates: loop-mount plus
   `filefrag`, or a small read-only UDF File Entry walk in the bless tool (UDF
   1.02, 2048-byte blocks, anchor at sector 256; tractable, maybe 200 lines).
5. **Which executable does the target ISO's WinPE actually run?** We measured no
   `winpeshl.ini`, so `winpeshl.exe` is the target, but Ventoy reads
   `Windows/System32/config/SYSTEM` to find it because some WinPE builds use
   `pecmd.exe` or `peset.exe`. Decide whether we support only stock Microsoft
   media (probably yes for MVP) and fail loudly otherwise.
6. **Does the 24H2/25H2 disk-discovery failure Ventoy users report
   (https://github.com/ventoy/Ventoy/issues/3010) reproduce with our
   implementation?** Evidence points at a Ventoy regression, not an
   architectural limit, but it must be checked on real media.
7. **Block-size prediction for Section 4** - if someone wants to spend an hour
   disproving it, patch a sector-0-spanning MBR into the system area of a copy
   and see whether Windows mounts it. A surprise here would remove all injected
   code from the Windows route, so it is worth one experiment even at low odds.

## 7. Rig findings (2026-09-25): Windows 7 and Vista, BIOS solved, EFI limited

The corpus was run through both loaders, and the pre-8 Windows story split
cleanly by firmware, because the medium behaves differently under each:

- On **BIOS**, falcon-bios serves the ISO as a resident INT 13h CD that stays
  in memory across the whole boot. bootmgr loads boot.wim into a WIM RAM disk,
  WinPE runs from RAM, and winjump reaches winpeshl and serves Setup its source
  through the falcon-vdisk driver. **Windows 7 (x86 and x64) and Vista x86 now
  boot here end to end** once four separate bugs were fixed (Section 7.2).
- On **EFI**, the loader serves the ISO as a firmware Block I/O CD that is
  destroyed at ExitBootServices. Windows 7 and Vista WinPE wait on that device
  in early kernel init and hang at "Starting Windows", before winpeshl ever
  runs. That is the one case the RAM-disk work in 7.1 is about, and OVMF blocks
  the only fix for it.

The earlier draft of this section blamed the EFI vanishing-CD hang for the BIOS
failures too. That was wrong: on BIOS the medium does not vanish, and the BIOS
failures had four unrelated causes, all now fixed. This section is corrected to
match.

### 7.1 EFI: the vanishing-CD hang before winpeshl

The loader serves the ISO as a virtual CD over EFI Block I/O on p2
(falcon-efi/src/iso), gone at ExitBootServices exactly as on real hardware, so
the corpus exercises the media-vanishes case rather than a QEMU-persists
shortcut.

- **Windows 8 and up (8.1, 10, 11, Server 2025, Macrium WinPE): pass.** Their
  WinPE reaches winpeshl, winjump attaches the ISO with `AttachVirtualDisk`,
  and Setup reads `\sources\install.wim` back through it (the harness measures
  the p2 reads: "read 255 MiB of install.* through the ISO attached in
  Windows"). This is the whole route working end to end.

- **Windows 7 x64 on EFI: WinPE hangs before winpeshl.** It reaches the
  "Starting Windows" splash and stays there to the 300 s timeout; Setup's UI
  never appears, so winpeshl - and therefore winjump, and therefore
  falcon-vdisk - never runs (measured clean, off state, 2026-09-25). The hang
  is in early kernel init, where the boot-start storage and CD drivers come up
  and wait on the boot device that vanished at ExitBootServices. Windows 8+
  tolerates the missing device and finishes to winpeshl; Windows 7 does not.
  This does not happen on BIOS (Section 7.2), where the resident INT 13h CD
  never vanishes.

  Consequence, and it is a real one: **loading the driver from winpeshl is too
  late for Windows 7 and Vista under EFI.**

  Confirmed with a persistent CD (bench rig, 2026-09-25): booting the same
  windows-7-x64 ISO as an ordinary QEMU CD-ROM that never vanishes, Win7's
  WinPE reaches the shell and Setup's "Enter your language" screen with no
  trouble. So Win7's WinPE is not the problem and needs neither winjump nor the
  driver when the media stays: the whole EFI failure is that our EFI-served CD
  vanishes at ExitBootServices and Win7, unlike 8+, will not finish booting
  without it.

  **The boot-start driver was built and tested, and it does not work
  (2026-09-25).** falcon-vdisk was given a boot-start mode (present a CD from a
  small embedded ISO, announce it once the boot drivers are up via
  IoRegisterBootDriverReinitialization), injected into a windows-7-x64 boot.wim
  as a `Boot Bus Extender` service (wimlib to add the `.sys`, hivex to register
  it), and that ISO run through the loader in the corpus. Win7 froze at
  "Starting Windows" exactly as before: a substitute CD-ROM the driver creates
  does not satisfy whatever is waiting. The wait is not "a CD must exist"; it is
  the storage path itself. The firmware presented the virtual CD as a SATA
  device (`Sata(0x0)` in the boot log), and that device is gone at
  ExitBootServices; Win7's boot-start storage driver blocks on the device it
  booted from, and a software CD from a later driver is a different device.

  So the fix is **a persistent boot medium**, the way Ventoy keeps the medium
  itself alive for Windows rather than letting the firmware CD evaporate, not a
  driver that re-creates one. On UEFI that means presenting the medium as
  something Windows keeps across ExitBootServices (an EFI RAM disk registered
  in the ACPI NFIT), which holds the boot medium in memory.

  **The persistent RAM disk was built and tested in three shapes, and OVMF
  blocks every one (2026-09-25).** A loader path (winram.c, since reverted)
  read a medium whole into pages and registered it with the firmware's
  EFI_RAM_DISK_PROTOCOL, then connected it and looked for the medium's own
  filesystem to boot \bootmgr.efi off. The common wall across all three shapes:
  OVMF's RamDiskDxe publishes a Block I/O for the RAM disk, but the firmware's
  filesystem and partition drivers do **not** bind to it, so the medium's own
  filesystem never mounts.

  1. **Persistent CD (EFI_PERSISTENT_VIRTUAL_CD), the ISO read whole.** The
     firmware processes El Torito on it - the EFI boot image
     (efi/microsoft/boot/efisys.bin) mounts, and \EFI\BOOT\BOOTX64.EFI (cdboot)
     loads from it - but CDFS does not bind to the RAM disk's Block I/O, so the
     CD's main ISO 9660 / UDF filesystem never mounts. cdboot then cannot map
     back to the CD and returns NO_MAPPING (0x8000000000000011); \bootmgr.efi,
     which lives on the main filesystem, is absent on every mounted volume
     (NOT_FOUND). Folding the whole boot chain into the El Torito EFI image so
     the one filesystem that does mount holds everything is capped out: the
     boot catalog's sector-count field is 16-bit, so that image cannot exceed
     ~32 MB, and boot.wim is ~168 MB.

  2. **Persistent hard disk (EFI_PERSISTENT_VIRTUAL_DISK), a GPT with one FAT32
     ESP holding the boot chain and boot.wim.** PartitionDxe does not process
     the RAM disk's GPT, so no ESP child handle appears and FatDxe never mounts
     it; \bootmgr.efi is NOT_FOUND on every volume. A raw FAT superfloppy (no
     partition table) registered the same way did not mount either.

  3. **Non-persistent disk (EFI_VIRTUAL_DISK) in loader-owned memory**, the most
     permissive config, run to rule out the memory type and the NFIT
     persistence flag as the cause. Same result: NOT_FOUND at "booting
     \bootmgr.efi", no filesystem mounted. So the block is PartitionDxe/FatDxe
     declining to bind to the RAM disk, not the persistence attribute.

  This is a firmware limit of the corpus, not of the approach: real firmware
  may bind a filesystem to the RAM disk where OVMF does not, and the bench
  already showed Win7 boots from a medium that stays. But the corpus (OVMF)
  cannot validate it, so Windows 7/Vista under UEFI remains a real-hardware
  question. What is proven here: none of the loader/userland shapes tried
  (boot-start driver, CD RAM disk, hard-disk RAM disk persistent or not) boots
  them under OVMF, and the cause of each is understood - the storage path waits
  on the vanished boot device, or the firmware will not mount the RAM disk.

- **Windows XP x64 and Vista x64 do not boot under UEFI at all**, failing at
  "finding the medium's EFI System Partition": XP has no UEFI support and Vista
  x64's is marginal. A firmware/architecture limit, the exit goal's allowed
  exception, not the hang above.

### 7.2 BIOS: Windows 7 and Vista boot (the four fixes)

On BIOS the medium does not vanish, so pre-8 WinPE reaches winpeshl and winjump
runs. Four separate bugs then stood between it and a served install source,
each found by extracting winjump's own `X:\falcon-winjump.log` from inside
WinPE on binhost and reading the served ISO's two filesystems by hand.

1. **The "No disk in drive A:" dialog (winjump, commit 83d247f).** Windows 7
   and Vista WinPE carry a floppy driver, so their drive set includes an empty
   A:. winjump's drive scan queried it, and Windows raised a modal hard-error
   dialog attributed to winpeshl's replacement, freezing the GUI with no shell
   behind it. Windows 8+ WinPE has no floppy driver and so no A:, which is why
   they never hit it. Fixed by `SetErrorMode(SEM_FAILCRITICALERRORS)` before
   the scan.

2. **The missing extent map (falcon-bios, commit 8831820).** Windows 7 and
   Vista WinPE cannot read the exFAT the ISO lives on and have no
   AttachVirtualDisk, so winjump falls to the falcon-vdisk driver, which serves
   the ISO from its raw disk extents. That needs the extent map, and the BIOS
   winjump config carried only `iso=` and `size=`, no `map=`; the EFI loader had
   carried it since the driver route was added.

3. **UDFS declining to mount (falcon-vdisk, commit 02934b0).** A Windows
   install DVD is a UDF/ISO 9660 bridge: the ISO 9660 tree is a stub with only
   a readme, the real sources live in the UDF tree. UDFS reads its anchor at the
   disc's last sector, so it needs the medium's true size; the driver answered
   no size query, so CDFS mounted the ISO 9660 stub instead and Setup found no
   install source. Fixed by answering READ_TOC's lead-out,
   IOCTL_DISK_GET_LENGTH_INFO, IOCTL_STORAGE_READ_CAPACITY and
   IOCTL_CDROM_GET_LAST_SESSION, each reporting the ISO's size.

4. **The harness could not see it (tools/coverage, commits 7342ee7, d2479cc).**
   `screen_kind` knew only Windows 8+'s flat dark-blue Setup; Windows 7 is a
   blue gradient and Vista a green-to-blue one, so both read as "graphical" and
   the install.* probe never fired. The Enter nudges walked Setup onto the
   license page before it could. And the probe read once, too early for a
   Windows 7 DVD whose UDF mount lands a few seconds after the volume answers.
   Fixed: the classifier matches the gradients, the nudges stop once Setup is
   up, and the probe re-opens a prompt and retries the read.

Measured on BIOS (2026-09-25): windows-7-sp1-x86 reads 338 MiB of install.*,
windows-7-x64 405 MiB, windows-vista-x86 349 MiB, each through the ISO the
driver serves; windows-server-2025, a modern Windows that had been failing BIOS
for the harness reason alone, reads 160 MiB and passes too. Windows 7 x64 needs
no driver signing: its WinPE loads the unsigned falcon-vdisk.

**windows-vista-sp1-x64 boots (2026-09-25, BIOS):** it reads 750 MiB of
install.* through the ISO winjump attaches, a full PASS. The road there is
worth recording, because two intuitive approaches are dead ends the code now
avoids.

The medium is the reason it needed its own path: **Vista SP1 x64's ISO is
ISO 9660 only, with no UDF filesystem** (Windows 7 x64 and Vista x86's are
UDF/ISO 9660 bridges). Both loaders read and patch boot.wim through `udf_mount`,
so on this medium `udf_mount` failed, `/sources/boot.wim` was never found, the
patch was skipped, and Vista booted its stock 18,432-byte winpeshl with no
winjump. So `win_boot` gained an ISO 9660 branch beside the UDF one: mount with
the iso9660 driver, read boot.wim, build the same winjump tail with
`wim_patch_build`.

Growing the file is where the two dead ends live. A UDF File Entry is natively
multi-extent, so the UDF path just appends the tail as a second extent; an
ISO 9660 file is one extent unless a "not final" record is followed by a
continuation. The first attempt did exactly that - made boot.wim multi-extent
by inserting a continuation record and re-packing its directory - and it
failed: **bootmgr's ISO 9660 reader loads a single extent but not a
multi-extent file.** winload stopped with `0xc0000001`, "the application is
missing or corrupt": it read only the first extent and never saw the patched
lookup table in the tail. Multi-extent is off the table for a boot CD.

So boot.wim is instead **relocated whole** to one new single extent past the
ISO's end, its directory record rewritten in place to point there (same record
length, only the extent and data-length fields change). The naive version of
that copied all ~148 MB into the append region and served it from memory over
INT 15h AH=87h, a block move per read: WinPE booted but so slowly it missed the
probe's timeout. The shipped version does not copy the bulk. The relocated
extent's sectors **redirect** to where the original boot.wim already lies on
the disk: `vcd_add_run` adds a run mapping the appended CD sectors, run for
run, to the original's disk location, so they are served over INT 13h like any
other ISO sector. Only two things reach memory - the one patched header sector
at the relocated file's start (a `vcd_overlay_at` conventional-memory override
over the header the redirect otherwise serves from disk), and the tail: the
original's partial last block followed by the patch tail, the same ~1 MB the
UDF path appends. Served that way boot.wim loads at disk speed.

One more edit was needed, and it is the reason the multi-extent attempt's
`0xc0000001` was so confusing: **bootmgr reads the boot CD through the ISO 9660
primary tree, not Joliet.** The iso9660 driver prefers Joliet (long names), so
relocating only the record it found left bootmgr reading the un-relocated
original from the primary tree - which booted, with the original winpeshl and
no source. `iso9660_mount_primary` mounts the primary tree with plain names so
`win_boot` relocates the record in both trees; whichever bootmgr reads now
points at the patched file. (A subtle bug hid behind this the whole time: the
header sector was built in the same scratch buffer the record edits reused, so
it was clobbered before it was staged and the relocated file's sector 0 was
served as a directory sector - the same `0xc0000001`. It is now its own
buffer.)

This is BIOS-specific, and for this ISO that is the whole story: Vista SP1
x64's El Torito catalog carries one entry, an x86 no-emulation image, and no
UEFI one, so on UEFI the loader fails at "finding the medium's EFI System
Partition" (0x800000000000000e) before boot.wim is ever read. The medium does
not boot UEFI at all - Vista's x64 UEFI support predates the install-media
convention - so there is nothing for a UEFI patch path to do here, and it is
the corpus's only ISO 9660-only Windows medium (Windows 7 x64 and Vista x86 are
UDF/ISO 9660 bridges). Should a UEFI-bootable ISO 9660-only Windows ISO ever
turn up, `iso9660_mount_primary` and the tail assembly are in falcon-core and a
UEFI path could reuse them over `iso/overlay.h`'s sidecar redirect.

### 7.3 32-bit Windows, characterized in full (2026-09-25)

The corpus runs a BIOS machine in two CPU columns: `bios` (the 64-bit host CPU
in legacy mode) and `bios32` (a Pentium III, 32-bit only, with SSE but no SSE2
and no NX). A Windows verdict is PASS only when the screen reaches "Windows
Setup" (WinPE booted, winpeshl ran, winjump ran) and then a Shift-F10 prompt
reads `\sources\install.*` back off a drive letter - which exists only if
winjump attached the ISO. So a PASS is end-to-end proof that the jump binary
ran and served Setup its source.

Every 32-bit (x86) Windows medium in the corpus, with its root cause:

| ISO | bios (host CPU) | bios32 (Pentium III) | root cause |
|---|---|---|---|
| windows-10-22h2-x86 | PASS | FAIL (checking the processor: no SSE2) | winjump32 proven on bios. On the P3, Windows 10 needs NX/SSE2 the P3 lacks: an allowed architecture exception, and the CPU check now names it. |
| windows-8.1-x86 | PASS | FAIL (checking the processor: no SSE2) | winjump32 proven on bios. P3 correctly rejected: 8.1 needs SSE2/NX. Allowed architecture exception. |
| windows-7-sp1-x86 | PASS (338 MiB) | PASS (539 MiB) | Boots on the P3 (2026-09-25). The CPU check used to reject it for lacking SSE2, which Windows 7 does not require; it is now gated to Windows 8 and later by winpeshl's PE OS version, and 7 (6.1) passes. |
| windows-vista-x86 | PASS (349 MiB) | PASS (581 MiB) | Boots on the P3 (2026-09-25), same fix as Windows 7: Vista (6.0) is below the 8+ gate, so NX/SSE2 are not asked of it. |
| windows-2000-pro-x86 | FAIL (no userland, graphical) | FAIL (no userland, graphical) | No WinPE at all; text-mode setup, a wholly separate route not yet built (Section 7.4). |
| windows-xp-pro-sp3-x86 | FAIL (no userland, graphical) | FAIL (no userland, graphical) | Same as 2000: text-mode setup. |

What this settles:

- **The 32-bit jump binary (falcon-winjump32.exe) works, on 32-bit hardware.**
  10-x86, 8.1-x86, 7-sp1-x86 and vista-x86 all pass `bios drop`, and 7-sp1-x86
  and vista-x86 also pass `bios32 drop` (a real Pentium III): winjump32 running
  as winpeshl, serving the ISO (through AttachVirtualDisk on 8.1/10, through the
  driver on 7/Vista), and Setup reading install.* through it.
- **The CPU check is gated to Windows 8 and later** by winpeshl.exe's PE
  optional-header OS version (6.2 = Windows 8). PAE is still asked of every
  Windows; NX and SSE2 only of 8+, which reset with nothing on screen without
  them. Vista (6.0) and 7 (6.1) boot on a processor with neither, which is what
  a Pentium III is.
- **8.1-x86 and 10-x86 on the Pentium III are genuine architecture exceptions:**
  those releases require SSE2 and NX a P3 does not have, and the check names the
  missing feature rather than letting bootmgr reset in silence.
- **The remaining 32-bit non-boots are the Windows 2000 / XP text-mode
  installer**, which has no WinPE and needs a different route entirely (not yet
  built).

### 7.4 Windows 2000 and XP: text-mode setup (0x7B solved; trace build boots)

2000 and XP have no WinPE. They boot a text-mode setup: the CD's El Torito boot
image loads setupldr, which reads txtsetup.sif, the kernel, the HAL and the
boot-start drivers off the CD through INT 13h, then hands to the NT kernel.
falcon-bios serves all of that as its INT 13h virtual CD, and it works up to
the handover: both windows-2000-pro-x86 and windows-xp-pro-sp3-x86 reach the NT
kernel and bugcheck 0x7B INACCESSIBLE_BOOT_DEVICE (0xC0000034,
OBJECT_NAME_NOT_FOUND), measured 2026-09-25.

The cause is the same shape as Windows 7: the running kernel re-accesses its
boot device through its own storage stack, not INT 13h. atapi.sys enumerates
real ATAPI hardware; the INT 13h virtual CD is not a device it binds to, so the
CD's ARC path does not resolve and the boot device is missing. Unlike 7 and
Vista there is no winpeshl to run winjump from, so the driver cannot be loaded
from userland.

The route, when built, is firadisk/winvblock-shaped: a driver that presents the
ISO as a device the NT kernel binds to, injected into text-mode setup by
patching the served ISO. The pieces, in build order, with the obstacles found:

1. A driver text-mode setup loads. falcon-vdisk already presents a CD-ROM from
   the ISO's raw disk extents. Settled 2026-09-25: the existing x86 build (at
   _WIN32_WINNT 0x0600) needs no XP-specific build. Its imports, dumped from
   the .sys, are ExAllocatePoolWithTag, ExFreePoolWithTag, the Exf interlocked
   list pair, IoBuildDeviceIoControlRequest, IoCreateDevice/DeleteDevice,
   Io{Create,Delete}SymbolicLink, IoGetDeviceObjectPointer, Iof{Call,Complete},
   KeInitialize{Event,Semaphore}, KeReleaseSemaphore, KeWaitForSingleObject,
   MmMapLockedPagesSpecifyCache, Ob{f}{Reference,Dereference}Object,
   Ps{Create,Terminate}SystemThread, RtlInitUnicodeString, ZwClose/Create/Read,
   DbgPrint - every one exported by Windows 2000 (NT 5.0) and XP (NT 5.1)
   ntoskrnl, MmMapLockedPagesSpecifyCache being the newest at NT 5.0. So the
   SYSTEM_POWER_STATE_CONTEXT compile wall (wdm.h at below 0x0600) is moot: the
   0x0600 binary is XP-safe as it stands. **Settled 2026-09-25 on real
   windows-xp-pro-sp3-x86 media: XP loads the driver through txtsetup.sif's
   [SCSI.Load], the firadisk way, no other section needed.** The x86 .sys,
   injected into a copy of the ISO and booted under QEMU (BIOS, `-machine pc`),
   printed its DriverEntry marker on COM1 and text-mode setup reached the
   "Welcome to Setup" screen unharmed, so the load path and the driver's entry
   both work. What remains is not whether it loads but what it does once loaded
   (piece 3).
2. Patch the served ISO for the XP layout. Add the .sys and rewrite
   i386/txtsetup.sif ([SCSI.Load], [SCSI], [SourceDisksFiles]; the driver loads
   as a legacy [SCSI.Load] entry like firadisk, so no [HardwareIdsDatabase]
   line is needed) and, for a real install, i386/dosnet.inf. Two edits:
   - **Adding the .sys is built (iso9660_append_record) and proven on the real
     ISO (2026-09-25).** The i386 directory does NOT need relocating: its last
     block (block 155 on windows-xp-pro-sp3-x86) has 550 free bytes and the
     entries end at "_DEFAULT.PI_", so a driver named to sort just past it
     ("_FALCON.SYS") appends in that block's slack, in order. The .sys data
     appends past the ISO's end, exactly the Vista boot.wim tail. Two things the
     first host test missed and the real ISO forced:
       - **The i386 directory's data length is not a multiple of the block
         size.** It is 263642 bytes: 128 full blocks and a 129th with 1498
         bytes of records and 550 free. iso9660_append_record floored that to
         128 and rejected the non-aligned length outright; it now rounds up to
         the partly-filled last block. A block-aligned synthetic directory (the
         only shape the unit test had) never exercised this.
       - **The directory's own record data length must grow by the record's
         length.** The new record sits at the old data-length boundary, so a
         data-length-strict reader (setupldr, libisofs) stops just before it and
         never sees the file. Growing i386's record in the root directory by the
         record length (still no relocation, block count unchanged) is what
         makes _FALCON.SYS appear; without it a primary-tree read does not list
         it. The earlier "keeps its size, its own record does not change" was
         wrong on this one field.
   - **txtsetup.sif's edit is built (txtsetup_inject) and proven.** It grows by
     three short lines (one per section), each modelled on atapi.sys's own form
     - the injected [SourceDisksFiles] line is byte-for-byte atapi's
     (`= 100,,,,,,4_,4,0,0,,1,4`). The ~480 KB file is grown and served by the
     same relocation as boot.wim: single extent moved past the ISO's end, its
     directory record rewritten to the new extent and length, the PVD volume
     space size grown to cover the appended blocks.
3. Give the driver the ISO's extents without winjump to hand them over, and a
   boot-start mode. The current falcon-vdisk gets its backing by IOCTL from
   winjump (SET_BACKING, a disk path + extents); text-mode setup has no winjump,
   so DriverEntry must instead read a baked-in extent map and FIND the stick
   itself (scan \Device\HarddiskN for the ISO's disk, since the NT device path
   is not known at patch time), then serve the ISO from the stick's real disk
   through the NT disk stack. atapi.sys/disk.sys must enumerate the stick before
   the driver reads it, so the [SCSI.Load] order matters. This is the crux and
   the remaining novel work.

The pieces interlock: nothing boots until all are in, so the first end-to-end
test is late. The first useful milestone came earlier and is now done: pieces 1
and 2 were proven together against the real ISO (2026-09-25). A host prototype
(the eventual win_xp_boot's ISO surgery, run over a file instead of the served
virtual CD) injected _FALCON.SYS and the three txtsetup.sif lines in place -
append past the end, rewrite the sif's record, grow the PVD, add the .sys record
in i386's slack, grow i386's own record - and the result booted XP text-mode
setup under QEMU: the driver's COM1 marker proved [SCSI.Load] loaded it, and
setup reached "Welcome to Setup" unharmed. The x86 driver carries an opt-in
COM1 trace at DriverEntry (VDISK_SERIAL_TRACE) for exactly this.

Since then win_xp_boot and piece 3's disk-finding are built and rig-proven
(2026-09-25), and the route is one barrier short of booting.

 - **win_xp_boot** (falcon-bios/src/win.c) does the prototype's ISO surgery over
   the served virtual CD in memory (vcd_overlay_append for the appended sif and
   driver, i9_relocate for the sif's record, iso9660_append_record for the .sys,
   the PVD and i386 record edits), routes 2000/XP media to it (now FAMILY_WINDOWS,
   detected by i386/txtsetup.sif + i386/setupldr.bin), and embeds the x86 driver.
   Rig-proven: the loader injects and serves the ISO, XP text-mode setup boots
   from the served virtual CD, and loads the driver through [SCSI.Load].

 - **Piece 3's disk-finding is built and proven.** win_xp_boot bakes the ISO's
   extent map into the served .sys at a magic-tagged struct; the driver decodes
   it at load on a detached thread and, retrying while the disk stack comes up,
   finds the stick by the GPT partition GUID (extmap.c + winjumpcore.c linked
   into the .sys, the same match winjump does in userland) and sets the backing.
   Rig-proven: both COM1 markers (SCSILOAD_OK and BACKING_OK) appear, so the
   novel "find the stick with no winjump" work is done.

 - **The boot-device binding is SOLVED (2026-09-25); XP boots.** After the
   driver resolved its backing it still bugchecked 0x7B (OBJECT_NAME_NOT_FOUND):
   the kernel resolves its boot device through an ARC name, but no real device
   registers one for the INT-13h-served medium, so the driver must. The name is
   NOT cdrom(0): setupldr records it as the served BIOS drive number, read on the
   rig from the registry as SystemBootDevice = multi(0)disk(0)cdrom(224) (224 =
   0xE0, the drive falcon-bios serves on). bind_boot_device reads that value and
   points \ArcName\<it> at the CD, which clears the 0x7B. Two more things the
   route needs, both learned on the rig: the CD reports its size from the baked
   map the instant it is created, so text-mode setup's CD-ROM scan counts it
   before the backing thread finishes (a no-media device is skipped -> "Setup
   cannot find a CD-ROM drive"); and the backing must stay on its own thread, not
   block DriverEntry, or the boot-start sequence stalls, disk.sys never
   enumerates the stick, and the volume comes back unmountable (0xED). With all
   three, XP text-mode setup boots from the driver-served CD: the trace build
   reaches "Welcome to Setup" reliably. The shipped build is not yet reliable -
   it reaches setup's file-loading on some runs and "Setup cannot find a CD-ROM
   drive" on others: setup's CD-ROM scan still races the backing thread finding
   the stick, and only the trace build's serial delays pace it past the race
   every time. Closing that race for the shipped build (e.g. registering the CD
   so the scan reads it only once it is backed) is the remaining polish; the
   0x7B boot-device binding itself is solved.

The rig: a falcon-bios stick with the ISO on p2 (mk-case-stick.sh), booted
under QEMU BIOS with the loader disk; the menu's Enter selects the ISO, and
setupldr's "press any key to boot from CD" is answered with more Enters. A
VDISK_TRACE=1 build turns on the driver's COM1 markers to read the sequence.
