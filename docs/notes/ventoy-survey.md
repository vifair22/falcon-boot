# Ventoy support survey (2026-09-18)

What Ventoy supports, how each case works, and what it means for falcon-boot.
Read from the source, github.com/ventoy/Ventoy at 6568972 (2026-08-06), not
from the website. Paths below are relative to that tree.

Verdicts marked **(K)** rest on knowledge of the distro's initramfs rather
than on anything in Ventoy's source, and are unverified until an ISO is
blessed and booted. Everything else was read in the code.

## 1. How Ventoy boots, and why most of it does not transfer

Every UEFI boot goes GRUB -> `ventoy_x64.efi`, which exposes the image file as
a virtual CD or disk and `LoadImage`s the image's own `\EFI\BOOT\BOOTX64.EFI`.
On top of that virtual device it patches at runtime, three ways
(`EDK2/.../Application/Ventoy/VentoyProtocol.c`):

- rewrites ISO9660/UDF directory records in flight, so a file's size and
  start sector point elsewhere;
- serves extra "virtual sectors" from RAM past the end of the ISO;
- wraps `OpenVolume` so a read of the initrd returns it with `ventoy.cpio`
  appended.

For Linux, that cpio runs first (`rdinit=/vtoy/vtoy`, `IMG/cpio/ventoy/init`),
unpacks the distro's own initrd, picks one of 58 per-distro hooks, builds a
device-mapper linear device `/dev/mapper/ventoy` over the ISO's extents, and
sed-patches the distro's init scripts to use it before `exec`ing them.

Almost every non-Linux feature is built on the first two mechanisms. That is
the runtime patching falcon-boot excludes (DESIGN Sections 6.2 and 8), so
each needs an offline equivalent or is out of reach.

## 2. Secure Boot: nothing reusable

`SecureBoot.md`, `EDK2/.../Application/VtoyShim/VtoyShim.c`,
`GRUB2/MOD_SRC/grub-2.04/grub-core/ventoy/ventoy.c`:

- Chain: a third-party Rocky Linux shim, falling back to `fbx64.efi`, which is
  Ventoy's own `VtoyShim` signed with the Ventoy MOK the user enrolls.
- VtoyShim replaces `Security2->FileAuthentication` and
  `Security->FileAuthenticationState` with its own functions, and calls shim
  internals at hardcoded offsets from the `SHIM_IMAGE_LOADER` pointer.
- **Default policy is "bypass"** (`g_sb_policy` zero-initialised,
  `VTOY_SB_POLICY_BYPASS 0`): the Security2 hook returns `EFI_SUCCESS` for
  every image, so any unsigned EFI runs.
- In bypass mode it also hooks `GetVariable`. `MokSBState` reads as 1
  (validation disabled), and the Microsoft bootmgr SVN entry in `dbx` is
  rewritten, which defeats SVN-based bootmgr revocation.
- The opt-in "check" policy (`VTOY_SECURE_BOOT_POLICY=1`) tries the firmware
  check, then shim `Verify`, and refuses otherwise. It also disables GRUB's
  `linux` loader, which removes GRUB2 mode and every IMG distro path.

This is loader-side relaxation of enforcement, which invariant 3 forbids,
plus lying to the OS about MOK and dbx state. falcon-boot's answer to the same
problem is blessing.

## 3. Linux

### Coverage claims

`README.md` claims 1300+ tested ISOs and "90%+ distros in distrowatch.com",
with the full list only on ventoy.net. The README names about 356 Linux
distributions (truncated with "......").

### The one idea Ventoy shares with us

Ventoy itself uses a cmdline-only path in two places, and only after checking
that p2 is exFAT and the distro's initramfs carries an exFAT module
(`ventoy_iso_scan_check` in `IMG/cpio/ventoy/hook/ventoy-os-lib.sh`):

- Fedora: appends `iso-scan/filename=<path>` and nothing else
  (`hook/rhel7/ventoy-hook.sh:126`).
- Porteus: native `from=<path.iso>` (`hook/debian/porteus-hook.sh`).

For everything else it uses the dm device and a patched init, which is our
`needs_injection` path (stubbed, not built).

### Verdicts by family

CMDLINE-OK means the stock initramfs has a find-ISO-from-a-file option, so a
blessed UKI with a cmdline template works. NEEDS-INJECTION means it has none.

| Family (Ventoy hook) | Covers | Native option | Verdict | falcon-boot today |
|---|---|---|---|---|
| debian/default (casper) | Ubuntu, Kubuntu, Mint, Pop, Zorin, elementary, Kylin | `iso-scan/filename=` | CMDLINE-OK | row exists |
| debian/default (live-boot) | Debian live, Kali, Tails, VyOS, TrueNAS SCALE | `findiso=` | CMDLINE-OK (Tails, VyOS, TrueNAS: probably) | row exists |
| debian/default (d-i) | Debian netinst/DVD installers | none in the CD initrd (only hd-media has iso-scan) (K) | NEEDS-INJECTION | classifies unknown (no `/live`), so bless refuses it |
| rhel7 (Live) | Fedora Workstation and spins | `root=live:CDLABEL= iso-scan/filename=` | CMDLINE-OK, used by Ventoy | row incomplete, see Corrections below |
| rhel7 (Anaconda) | Fedora netinst/Server, RHEL, Rocky, Alma, Oracle 7-10 | `inst.stage2=hd:<dev>:<path.iso>` (K) | CMDLINE-OK in principle; EL8/9 kernels likely lack exFAT | not handled |
| rhel7 (FCOS) | Fedora CoreOS | none | NEEDS-INJECTION | no |
| arch | Arch, EndeavourOS, SystemRescue, archiso derivatives | `img_dev= img_loop=` | CMDLINE-OK | rows exist |
| manjaro | Manjaro | miso `img_dev= img_loop=` (K) | PROBABLY | no |
| arch (artix), hyperbola, parabola | Artix, Hyperbola, Parabola | archiso-fork loop hook (K) | PROBABLY | no |
| gentoo | Gentoo (dracut and genkernel), Calculate | dracut: our template; genkernel: `isoboot=` (K) | CMDLINE-OK | row added 2026-09-18, booted |
| suse | openSUSE/SLE installer, openSUSE live | `install=hd:<iso>?device=<dev>`; live `isofrom_device= isofrom_system=` (K) | CMDLINE-OK | no |
| debian/porteus | Porteus | `from=<path.iso>` | CMDLINE-OK, used by Ventoy | no |
| debian/slax, minios | Slax, MiniOS | livekit `from=` (K) | PROBABLY | no |
| debian/antix | antiX, MX | `fromiso=` (K) | CMDLINE-OK | no |
| debian/knoppix | Knoppix | `bootfrom=<dev>/<path.iso>` (K) | CMDLINE-OK | no |
| pmagic | Parted Magic | `iso_filename=` (K) | CMDLINE-OK | no |
| nixos | NixOS | stage-1 `findiso=` (K) | PROBABLY; check exFAT | no |
| android, primeos, phoenixos, debian/bliss | Android-x86 and forks | `iso-scan/filename=` (K) | PROBABLY | no |
| gobo | GoboLinux (dracut) | dracut iso-scan, if in the initramfs | PROBABLY | no |
| deepin | Deepin, UOS | live-boot `findiso=` (K) | PROBABLY | no |
| openEuler | openEuler, Kylin Server | Anaconda `inst.stage2=hd:` | PROBABLY | no |
| alt | ALT Linux | propagator `automatic=...` (K, uncertain) | PROBABLY | no |
| tinycore | Tiny Core | `iso=` (K) | PROBABLY | no |
| slackware | Slackware installer / liveslak | installer: none; liveslak `livemedia=<dev>:<iso>` (K) | installer NEEDS-INJECTION, live OK | no |
| debian/pve | Proxmox VE installer | none | NEEDS-INJECTION | no |
| alpine | Alpine | none (nlplug-findfs by device) | NEEDS-INJECTION | no |
| xen | XenServer / XCP-ng installer | none, and it is a Xen multiboot2 boot | NEEDS-INJECTION + not a UKI shape | no |
| clear, guix, crux, photon, lunar, pisilinux, zeroshell, smoothwall, t2, kiosk, nutyx, and about 15 small others | as named | none | NEEDS-INJECTION | no |
| mageia, pclos, fatdog, ploplinux, berry, chimera, ewe, cdlinux, kaos, devuan, puppy | as named | not established | UNKNOWN | no |

### Corrections to our model

1. **exFAT is a precondition, not an assumption.** Every cmdline verdict
   depends on the distro's initramfs mounting exFAT to find p2, which is why
   Ventoy checks it before trusting iso-scan. The bless tool should look for
   `exfat.ko*` in the extracted initramfs (or exfat built into the kernel) and
   refuse with a clear message rather than sign a wrap that cannot boot. The
   `initramfs_has_exfat` column in `falcon-core/src/media/family.h` already
   exists for this; it is recorded per family, never checked per ISO.
2. **The Debian row only covers live images.** A netinst/DVD ISO has no
   `/live`, so it classifies as unknown and bless refuses it without
   `--cmdline`, which is the right answer until injection exists. It deserves
   its own fingerprint (`/install.amd`) so the refusal can say why.
3. **The Fedora row is incomplete.** `iso-scan/filename=%ISO%` alone does not
   tell dmsquash-live what to mount; the ISO's own cmdline carries
   `root=live:CDLABEL=<label> rd.live.image`. With `%LABEL%` now available
   (added for Gentoo), the row becomes
   `root=live:CDLABEL=%LABEL% rd.live.image iso-scan/filename=%ISO%`.
   Unverified until a Fedora ISO is blessed and booted.

## 4. Windows

| Ventoy feature | Mechanism | Files | For us |
|---|---|---|---|
| Install ISO | Rebuilds boot.wim in RAM, replacing the init exe's lookup entry with `vtoyjump64.exe` + params + the original exe, served as virtual sectors. LZMS WIMs refused. | `GRUB2/.../ventoy/ventoy_windows.c` | Done, offline, by winjump + wimlib, which also handles LZMS. |
| vtoyjump extras | Win11 LabConfig/BypassNRO registry keys, unattend via registry, injection archive + `ventoy_autorun.bat`, ImDisk fallback for Win7 | `vtoyjump/vtoyjump/vtoyjump.c` (`Windows11Bypass`, `MountIsoFile`) | Parity candidates for winjump: Win11 bypass and unattend. ImDisk only matters for Win7. |
| "Press any key to boot from CD" | UEFI: ConIn `ReadKeyStroke` hook returns one keypress | `VentoyProtocol.c` `ventoy_wrapper_read_key` | Runtime hook; the user presses a key. |
| WinPE ISOs (Hiren's, Strelec, 2k10, DLC) | Same WIM rebuild; the exe to replace comes from `SYSTEM\Setup\CmdLine` in the WIM's SYSTEM hive (PECMD, PESET, winpeshl), with per-distro WIM lists | `ventoy_windows.c` `parse_registry_setup_cmdline`; `grub.cfg` `distro_specify_wim_patch*` | Medium: extend prep to read the hive (hivex) and patch several WIMs per ISO. |
| WIMBOOT mode, `.wim` file boot | iPXE wimboot with virtual files / a user-supplied template ISO | `grub.cfg` `vtoy_wimboot_func`, `wim_common_menuentry` | Not needed; our offline prep does the same job. |
| VHD(x) boot | User-supplied `ventoy_vhdboot.img`; patches the BCD and winload in RAM; warns when the host partition is not NTFS | `ventoy_vhd.c` | Hard: native VHD boot needs NTFS and p2 is exFAT. |

## 5. Unix

| OS | Mechanism | For us |
|---|---|---|
| FreeBSD 9-15 and derivatives (GhostBSD, pfSense, OPNsense, TrueNAS CORE, XigmaNAS, MidnightBSD, HardenedBSD) | Redirects a sacrificial file's directory record to a prebuilt per-version `geom_ventoy.ko.xz` and appends `ventoy_load="YES"` plus the ISO's extents to `loader.conf` the same way. The module builds a GEOM provider over the extents. | Hard: one kernel module per FreeBSD major, built in a FreeBSD VM, and FreeBSD's `loader.efi` is unsigned, so it needs blessing too. |
| DragonFly | Replaces the initrd with a custom mfs init that runs dmsetup over the extents | Hard, same reasons. |
| NetBSD | Detected, then "not supported" (`INSTALL/grub/grub.cfg`) | n/a |
| OpenBSD, illumos | No code path | n/a |

Files: `GRUB2/.../ventoy/ventoy_unix.c`, `Unix/ventoy_unix_src/FreeBSD/geom_ventoy_src/`.

## 6. Hypervisors and everything else

| Item | Ventoy | For us |
|---|---|---|
| VMware ESXi | No dedicated code; it falls through to a plain chainload of the ISO's `mboot.efi`, which loads every module into RAM | Probably works today, verified through db if mboot.efi carries the Microsoft third-party CA signature. Worth one test. |
| XenServer / XCP-ng | Linux path with `install.img` as initrd plus the `xen` hook | Xen multiboot2 does not fit a single UKI; needs a signed Xen/GRUB chain and an injection hook. |
| Proxmox VE | debian `pve` sub-hook | Linux NEEDS-INJECTION. |
| ChromeOS family (FydeOS, Flex, CloudReady), IMG distros (LibreELEC, Batocera, Recalbox, OpenWrt, ...) | Loop-mount the `.img` in GRUB, boot its kernel with the cpio and a per-OS loop hook | Hard, one integration per OS; needs a virtual-disk mode first. |
| Generic `.img`, `.vtoy` vDisk | 512-byte virtual disk, chainload its EFI loader | Loader side easy (a 512-byte BlockIO mode); the booted OS still has to find itself. |
| `.efi` files, memtest | Chainload | Done (bare payloads, DESIGN Section 7.2). |
| Memdisk mode | Whole ISO in RAM as BlockIO | Easy, of limited use. |
| "VENTOY COMPATIBLE" ISOs | ISOs containing `ventoy.dat` or that string in the PVD are booted unpatched and find themselves through `VentoyOsParam`, a volatile runtime variable (GUID 77772020-...) carrying path, disk GUID and extents | Worth considering: publishing it is state handed to the OS, not patching, and third-party ISOs already read it. Needs its own design discussion. |
| Plugins | `auto_install`, `persistence`, `injection`, `conf_replace`, `dud`, `password`, menus | `conf_replace` and `dud` are runtime patching; the rest are UX. |

## 7. What to do with this, ranked

1. **exFAT check at bless time** (Corrections, item 1). Cheap, and it turns a signed
   wrap that silently cannot boot into a refusal that says why.
2. **Fix the Fedora row** with `%LABEL%` and prove it on a Fedora Live ISO.
3. **Name Debian installers** (`/install.amd`) so their refusal says
   "installer media needs injection" instead of "unknown family".
4. **Add cmdline families in order of reach:** openSUSE (installer and live),
   Manjaro, Porteus, antiX/MX, Knoppix, Parted Magic, NixOS, Artix. Each is a
   registry row, a kernel/initrd path and one blessed boot to prove it.
5. **Test ESXi** as a plain chainload; it may already work.
6. **winjump parity:** Win11 LabConfig/BypassNRO and unattend.
7. **Decide on injection.** Proxmox, Alpine, the Debian installer and the
   long tail all need the stubbed Section 6.2 mechanism. It is the one
   remaining large piece between us and Ventoy's Linux coverage, and blessing
   gives it a place to live: the hook cpio can go inside the signed wrap
   with the extents computed offline.
8. **Out of scope unless asked:** BSDs, ChromeOS, IMG distros, Xen, Windows
   VHD boot.
