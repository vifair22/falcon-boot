# falcon-boot + Encrypted Arch - Build Runbook

Adapted from the Ventoy-era `/usr/local/share/archusb/RUNBOOK.md`, which is
preserved in the migration backup at
`/mnt/980pro/falcon-migration-20260913/p4-root/usr/local/share/archusb/RUNBOOK.md`.
Read that one for the history; read this one to rebuild.

Every step that differs from the Ventoy original is marked **CHANGED**,
**NEW** or **DELETED**, with the reason. Everything not so marked is the
original's, including its traps, which are the most valuable thing in it.
Where this document is unsure it says so instead of guessing; those places are
marked **UNVERIFIED**.

Target stick: **Corsair Voyager GTX**, serial `A104074104ED00055499`,
500118192 sectors (238.5 GiB).

**Always resolve the device by serial, never by `/dev/sda`.** USB letters
shuffle between boots.

```bash
ls -l /dev/disk/by-id/ | grep -i voyager
DISK=/dev/disk/by-id/<whatever that printed>
readlink -f $DISK   # sanity check before every destructive step
```

> **CHANGED, small but do not skip it.** The old runbook hardcoded
> `ata-Corsair_Voyager_GTX_A104074104ED00055499`. Which `by-id` alias udev
> emits depends on how the bridge enumerates, and a `usb-...` form is at
> least as likely. Look it up rather than paste it.

---

## 0. Design decisions (settled)

| Decision | Choice | Why |
|---|---|---|
| Boot artifact | Signed UKI | Covers kernel + initramfs + cmdline in one signature |
| Kernel/initramfs location | **p3, the LUKS2 UKI shelf, mounted at `/efi`** | **CHANGED.** There is no FAT boot partition on a falcon stick; the loader reads the UKI off p3 |
| p4 unlock | initramfs (`sd-encrypt`), passphrase handed over as an sd-stub credential | **CHANGED reason.** Ventoy's GRUB had no `luks2.mod`; falcon's loader has its own LUKS2 stack but deliberately never mounts root (DESIGN Section 3) |
| Container layout | LUKS2 -> LVM -> ext4, inside the p4 mkstick created | ext4 shrinks; xfs doesn't. EFI never touches p4, so its interior is the OS's business |
| Desktop | XFCE + lightdm | Best config tooling for unknown hardware; X11 is more forgiving than Wayland |
| VG / mapper names | `vgusb` / `cryptusb`, plus `cryptefi` for p3 | Must not collide with the host's `vg0` / `cryptroot` |
| FS labels | `USBROOT` / `USBHOME` | Must not collide with host `ROOT` / `HOME` |
| Secure Boot | shim 16.1 + MOK enrollment of `falcon-boot.cer` | **CHANGED.** One enrollment, and the UKI is genuinely verified. See Section 8 |

### Constraints discovered by inspection

**DELETED, all of them Ventoy's.** The four constraints the old runbook opened
with were: GRUB has no `luks2.mod` anywhere on the stick; Ventoy's custom menu
is bound to F6 through `ventoy/ventoy_grub.cfg`; exFAT cannot be shrunk; and
Ventoy's `grubx64_real.efi` hard-fails with *"This is NOT a standard Ventoy
device"* unless VTOYEFI immediately follows partition 1. Not one of them
survives, because none of the software they were about is on the stick any
more. The VTOYEFI relocation dance in particular - copy 32 MiB out, rewrite
the table, copy it back, preserve attribute bit 63 and the partition GUID - is
gone entirely; `falcon-boot-mkstick` writes the whole GPT.

**NEW, and these replace them:**

- **p2 must keep the Microsoft basic data type GUID and the partition name
  `FALCON-ISO`.** Both halves, exactly (DESIGN Section 3). The type is what
  makes Windows create a volume for it at all - a falcon-specific type GUID
  was measured in WinPE against our own image and reported as
  `Hidden : Yes`, no letter, no access - and the name is the discriminator
  the loader matches on. Do not "tidy" either.
- **p3 and p4 must keep their unrecognised type GUIDs**, for the mirror-image
  reason: Windows offers to *format* a basic-data partition it cannot read,
  and p3 holds the stick's only copy of the MOK private key.
- **p3's keyslot must be pbkdf2** at the pinned iteration count. The loader's
  in-EFI unlock implements pbkdf2 and aes-xts-plain64 and nothing else; a
  default `luksFormat` on p3 produces a partition the boot menu cannot open.
  mkstick does this for you, which is why p3 is created by mkstick and not by
  hand.
- **Nothing in Arch may write to p1.** The ESP holds shim, MokManager, the
  loader and the enrollment certificate, all of which mkstick owns. Arch does
  not mount it and must never `bootctl install` or `grub-install` at it. See
  the `systemd.gpt_auto=0` note in Section 7.

---

## 1. Target layout

**CHANGED wholesale.** The stick is made by `falcon-boot-mkstick`, not by
`sgdisk` in this runbook. The table below is what it produces (DESIGN Section
3); the sizes are the ones this build asks for.

| # | Size | Type | Name | Contents |
|---|---|---|---|---|
| 1 | 64 MiB | ESP, FAT32 | `FALCON-ESP` | shim (`BOOTX64.EFI`), MokManager (`mmx64.efi`), the falcon loader (`grubx64.efi`), `falcon-boot.cer` |
| 2 | 96 GiB | basic data, exFAT | `FALCON-ISO` | the ISO library, drag-and-drop target |
| 3 | 1 GiB | falcon UKI-shelf GUID, LUKS2 (pbkdf2) -> ext4 `FALCON-UKI` | `FALCON-UKI` | `/uki/*.efi`, `falcon-mok.key`, `falcon-mok.crt`, and (this build) `vmlinuz-linux` |
| 4 | the rest, about 141 GiB | Linux LUKS, LUKS2 (argon2id) | `FALCON-ROOT` | **the personal OS** |

Inside the p4 container, which is this runbook's business and nothing else's:

```
LUKS2 (argon2id) -> PV -> VG vgusb
  |- vgusb-root   32 GiB    ext4  USBROOT
  +- vgusb-home   100%FREE  ext4  USBHOME
```

**NEW, and it is the one place this build overrides mkstick.** mkstick puts a
plain ext4 (`FALCON-ROOT`) directly inside p4. This install replaces that with
LVM, exactly as the Ventoy-era stick had it. That is legitimate and not a
fork: DESIGN Section 3 says EFI-land never touches p4 and the loader never
matches it, so p4's interior belongs to the OS. What must survive the
replacement is the **LUKS header and mkstick's keyslot** - the passphrase the
loader prompts for opens both p3 and p4, and re-running `luksFormat` would
break that. Section RESTORE does it the safe way.

No swap partition - flash endurance. `zram-generator` instead.

> The VG is fully allocated. ext4 shrinks offline (`resize2fs`), so if you
> later want space back for LVM snapshots you can reclaim it from `home` - it
> is not a one-way decision.

> **`home` is bigger than it was.** The old stick gave 128 GiB to the whole
> container; this one gives roughly 141 GiB, because the 108 GiB Ventoy
> partition shrank to a 96 GiB ISO library and VTOYEFI's 32 MiB is gone.
> `root` stays at 32 GiB and `home` takes the difference. Check the arithmetic
> against the actual p4 size before running `lvcreate`; if `--p4-size` was set
> differently the 32 GiB root still fits but the home figure will not match.

### Unallocated space

**CHANGED, and the change is a demotion.** The old stick had one gap that
"must never be trimmed": sectors 34..2047, holding Ventoy's legacy-BIOS core
image, 452 KB of 16-bit boot code. There is no BIOS path on falcon and nothing
is written there, so the gap is now genuinely empty.

The *habit* stays. Anything that trims this stick addresses named partitions
by PARTUUID and never enumerates partition-table gaps, because "discard
everything unallocated" is a rule that is wrong the moment somebody puts
something in a gap again.

---

## 2. Pre-flight

**CHANGED.** There is no Ventoy partition to back up and restore, because
mkstick creates p2 empty and the ISO library is rebuilt by dropping files on
it. What must exist before anything destructive is the migration backup this
runbook restores from:

```
/mnt/980pro/falcon-migration-20260913/
  RESTORE-NOTES.txt   GPT, LVM, filesystem UUIDs, old fstab, old HOOKS
  p1/                 the old Ventoy exFAT partition
  p3/                 the old ARCHBOOT FAT32 /boot
  p4-root/            the Arch root filesystem
  p4-home/            home
```

Verify it before trusting it, the same way the old pre-flight did:

```bash
rsync -rnc --itemize-changes /mnt/980pro/falcon-migration-20260913/p4-root/ /mnt/target/
```

against a restored copy, not against the stick. Do not proceed on a backup
you have not read back.

**Keep `RESTORE-NOTES.txt`.** It is the only record of the old filesystem
UUIDs, and preserving those is what lets `/etc/fstab` come through this
migration unedited. The two that matter:

```
vgusb-root  USBROOT  13faf1a5-d8b7-429f-86ac-b4c9e9b46795
vgusb-home  USBHOME  4c712605-d751-4d13-bcf6-3bb106059cea
```

---

## 3. Make the stick

**CHANGED wholesale.** The old Section 3 was four screens of `sgdisk`,
partition relocation, attribute bits and `md5sum` checks. It is one command
now.

```bash
falcon-boot-mkstick create $DISK --force \
    --p1-size 64M \
    --iso-size 96G \
    --p3-size 1G \
    --shim   <dir holding shimx64.efi and its MokManager> \
    --loader build/bin/falcon-boot-signed.efi \
    --uki    <a first UKI to sign and stage, optional>
```

`--force` is not optional against a block device; mkstick refuses one without
it. `--p4-size` is left unset so p4 takes the remainder. `--uki` can be
omitted here and the real UKI built later by `mkinitcpio -P` on the restored
install, which is what the RESTORE path below does.

It partitions, formats p1 FAT32 and p2 exFAT, creates p3 with a **pbkdf2**
keyslot at the pinned iteration count and p4 with LUKS2's stock argon2id,
generates the MOK keypair, writes the private key to p3 at 0600 and the DER
certificate to p1 as `falcon-boot.cer`, installs shim and MokManager and our
loader on p1 under whatever next-stage name the shim binary actually embeds,
and signs the loader with the new MOK.

It prompts once for the passphrase, and that single passphrase is what opens
**both** p3 and p4. That is a design requirement, not a convenience: the
loader asks for it, unlocks p3, and hands the same bytes to the initramfs as
a credential so `systemd-cryptsetup` can open p4 without a second prompt
(DESIGN Section 5.1).

> **`--p3-plain` is not for this build.** A plaintext shelf means no private
> key on the stick, which means no signing appliance and no self-hosted
> blessing (DESIGN Section 3). It also means the loader never prompts and the
> credential handoff is not used, so the initramfs would prompt for p4
> itself. Everything else in this runbook assumes an encrypted p3.

> **Record the LUKS UUIDs the moment mkstick finishes.** They are new, they
> are not in `RESTORE-NOTES.txt`, and two files need them:
>
> ```bash
> cryptsetup luksUUID ${DISK}-part3   # -> /etc/crypttab
> cryptsetup luksUUID ${DISK}-part4   # -> /etc/kernel/cmdline, twice
> ```

---

## 4. LVM inside p4

**CHANGED.** The old runbook ran `cryptsetup luksFormat` here. Do not. mkstick
already formatted the container and the keyslot it wrote is the one the loader
and the credential handoff depend on. Open it, throw away the ext4 mkstick put
inside, and build LVM in its place.

```bash
cryptsetup open ${DISK}-part4 cryptusb        # mkstick's passphrase

# mkstick left an ext4 here. wipefs removes only the signature; the LUKS
# header is on the partition, not inside the mapper, so it is untouched.
wipefs -a /dev/mapper/cryptusb

pvcreate /dev/mapper/cryptusb
vgcreate vgusb /dev/mapper/cryptusb
lvcreate -L 32G -n root vgusb
lvcreate -l 100%FREE -n home vgusb
```

The mapper name `cryptusb` is load-bearing: it is what `rd.luks.name=` names
in `/etc/kernel/cmdline`, and it was chosen so it cannot collide with the
Gentoo host's `cryptroot`.

For a fresh build, plain `mkfs`:

```bash
mkfs.ext4 -L USBROOT /dev/vgusb/root
mkfs.ext4 -L USBHOME /dev/vgusb/home
```

For a restore of the 2026-09-13 backup, preserve the old filesystem UUIDs -
see RESTORE below. That is the whole trick that keeps `/etc/fstab` unedited.

p3 is opened separately and mounted at `/efi`, not at `/boot`:

```bash
cryptsetup open ${DISK}-part3 cryptefi        # the same passphrase

mkdir -p /mnt/target
mount -o noatime /dev/vgusb/root /mnt/target
mkdir -p /mnt/target/home /mnt/target/efi
mount -o noatime /dev/vgusb/home /mnt/target/home
mount -o noatime /dev/mapper/cryptefi /mnt/target/efi

LUKS4=$(blkid -s UUID -o value ${DISK}-part4); echo "p4 LUKS UUID: $LUKS4"
LUKS3=$(blkid -s UUID -o value ${DISK}-part3); echo "p3 LUKS UUID: $LUKS3"
```

Record both.

### Why `/efi` and not `/boot`

**CHANGED, and this was the one real judgement call in the migration.** The
old install mounted the FAT32 ARCHBOOT partition at `/boot`. There is no such
partition; the UKI now lives on p3. It has to be mounted somewhere, because
kernel and mkinitcpio updates must regenerate the UKI in the place the loader
actually reads - otherwise pacman writes a new UKI onto the root LV and the
stick goes on booting the stale one forever.

`/efi` wins on three counts and loses on one. Stated honestly:

1. **It is where Arch's own tooling looks.** With no preset file at all,
   mkinitcpio's pacman script defaults to `/efi/EFI/Linux/arch-<pkgbase>.efi`
   and its removal path stats `/boot/ /efi/`. Following upstream means a
   future `linux.preset.pacnew` merge lands somewhere sane instead of
   fighting us.
2. **The name change is a signal, and every trap below turns on it.** The old
   `/boot` was vfat: `umask=0077`, `fmask`, `dmask`, no ownership, no journal,
   `sbctl` refusing to treat it as an ESP because it was typed `0700`. The new
   mount is ext4 on an encrypted volume, mounted late, by a keyfile. Keeping
   the name would invite every one of the old assumptions to be reused
   silently.
3. **Churn is small and was measured, not guessed.** Grepping the backup for
   `/boot` outside the runbook itself finds: `sign-uki` (rewritten anyway),
   `linux.preset` (rewritten anyway), `archusb-status-collect` (two lines),
   `archusb-boot-next` and `ventoy-update` and `bootnext-install.sh` (all
   deleted with Ventoy), and `archusb-boot-next-reset.service` (deleted).
   That is one surviving file to touch.

**The count against: `/efi` conventionally means "the ESP", and p1 IS the
ESP.** That objection is real and is answered rather than dismissed - Arch
never mounts p1, nothing in this install may write to it, and so there is no
competing claim on the name *inside this install*. The only boot-artifact
filesystem Arch ever mounts is p3.

**If you disagree, flipping to `/boot` is five minutes**, and this is the
complete list: `linux.preset` (`ALL_kver`, `default_uki`), the `UKI=` default
in `sign-uki`, the `/efi` line in `fstab`, `UKI=` in
`/usr/local/sbin/archusb-status-collect`, and the mount point you create in
Section RESTORE. Nothing else in this document depends on the spelling.

### `/efi` must be mounted, and nothing shouts if it is not

**NEW trap, and it is the worst one this layout introduces.** If p3 fails to
open - keyfile missing, wrong slot, disk not settled - then:

- pacman's kernel transaction creates `/efi` on the **root LV** and writes the
  kernel copy and a full UKI into it;
- mkinitcpio reports success;
- `sign-uki` signs the file it was handed and reports success;
- the loader goes on booting the **old** UKI still sitting on p3.

Every layer says it worked. Nothing says the boot image did not move. Check
after any kernel update:

```bash
findmnt /efi                                     # must be /dev/mapper/cryptefi
stat -c '%y %n' /efi/uki/arch-usb.efi            # must be recent
sbverify --cert /efi/falcon-mok.crt /efi/uki/arch-usb.efi
```

This is exactly the class of silent failure the status widget exists for; see
"Status widget" below for the key it should carry.

---

## 5. Bootstrap Arch on Gentoo

**Unchanged** except for the mount check. Gentoo has no pacstrap; the
bootstrap tarball ships one.

```bash
cd /mnt/980pro
curl -O https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst
curl -O https://geo.mirror.pkgbuild.com/iso/latest/sha256sums.txt
sha256sum -c --ignore-missing sha256sums.txt   # must say OK

mkdir -p archboot && tar -I zstd -xf archlinux-bootstrap-x86_64.tar.zst -C archboot
B=/mnt/980pro/archboot/root.x86_64
echo 'Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch' > $B/etc/pacman.d/mirrorlist

mount --bind /dev $B/dev; mount --bind /dev/pts $B/dev/pts
mount -t proc proc $B/proc; mount -t sysfs sys $B/sys; mount --bind /run $B/run
mkdir -p $B/mnt && mount --rbind /mnt/target $B/mnt && mount --make-rslave $B/mnt
cp /etc/resolv.conf $B/etc/

chroot $B /bin/bash
```

> **`--rbind`, not `--bind`.** A plain bind does not carry the nested mounts
> into the chroot, and pacstrap silently writes all of them onto the root LV
> instead. Verify before installing - **CHANGED, the third path is now
> `/mnt/efi`**:
> `chroot $B df -h /mnt /mnt/home /mnt/efi` must show three *different*
> devices.

Inside the bootstrap chroot:

```bash
pacman-key --init && pacman-key --populate archlinux && pacman -Sy
```

---

## 6. Package set

**CHANGED in two lines only.** `sbctl` goes (there is no db keystore to manage
and its hook has to be neutralised anyway) and `sbsigntools` comes in
explicitly, since `sign-uki` needs `sbsign`, `sbattach` and `sbverify` and
nothing else pulls them. `grub` and `os-prober` are kept: they are rescue
tools for fixing *other* machines, which is most of what this stick is for,
and they no longer have anything to do with how it boots itself.

```bash
pacstrap /mnt \
  base linux linux-firmware amd-ucode intel-ucode mkinitcpio cryptsetup lvm2 \
  sudo zram-generator nano vim base-devel git \
  \
  e2fsprogs xfsprogs btrfs-progs f2fs-tools jfsutils nilfs-utils bcachefs-tools \
  udftools squashfs-tools mtools dosfstools exfatprogs ntfs-3g \
  mdadm dmraid multipath-tools gptfdisk parted \
  \
  nfs-utils cifs-utils sshfs gvfs gvfs-smb gvfs-mtp gvfs-gphoto2 udisks2 \
  \
  gparted arch-install-scripts grub os-prober qemu-img \
  testdisk ddrescue smartmontools nvme-cli hdparm efibootmgr sbsigntools \
  \
  networkmanager network-manager-applet wpa_supplicant openssh ufw \
  bind traceroute mtr ethtool iperf3 whois socat openbsd-netcat \
  nmap tcpdump lsof strace usbutils pciutils dmidecode \
  \
  xfce4 xfce4-goodies lightdm lightdm-gtk-greeter xorg-server \
  mesa vulkan-radeon vulkan-intel vulkan-nouveau libva-mesa-driver xf86-video-vesa \
  pipewire pipewire-pulse pipewire-alsa wireplumber pavucontrol \
  ttf-dejavu noto-fonts noto-fonts-emoji \
  \
  p7zip unrar unzip zip lz4 lzop htop rsync wget curl less man-db tree
```

`photorec` (disk carving) ships inside `testdisk`. BitLocker volumes are
readable via `cryptsetup` directly - its `bitlk` support covers them, no extra
package.

Deliberately excluded: **ZFS** and **APFS**. Both are out-of-tree DKMS modules
that must be rebuilt against every kernel update; on a stick you update
irregularly you will eventually boot to a broken module. Boot a ZFS-aware ISO
from the falcon menu instead.

---

## 7. Configure

```bash
genfstab -U /mnt >> /mnt/etc/fstab
arch-chroot /mnt
```

```bash
# genfstab reads /proc/swaps through the bind and picks up the HOST's swap, which does
# not exist on the stick and will stall boot. Remove it.
sed -i '/[[:space:]]swap[[:space:]]/d' /mnt/etc/fstab
```

**CHANGED.** Check `/etc/fstab` against `tools/archusb/fstab.snippet`: `/` and
`/home` need `noatime` and `commit=5`, `/efi` needs `nofail`, the old vfat
options (`umask`, `fmask`, `dmask`, `codepage`, `iocharset`, `shortname`,
`errors=remount-ro`) must all be **absent** because ext4 has none of them, and
there must be **no swap line**.

```bash
# NO `hwclock --systohc` here. This stick must never write a host's CMOS RTC;
# see the Clock section below. The timezone stays pinned to Central US -
# the stick travels, the operator's local time does not.
ln -sf /usr/share/zoneinfo/America/Chicago /etc/localtime
sed -i 's/^#en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen && locale-gen
echo 'LANG=en_US.UTF-8' > /etc/locale.conf
echo 'archusb' > /etc/hostname

useradd -m -G wheel -s /bin/bash <user>

# passwd needs /dev/urandom for the salt. A plain `chroot` without /dev fails with
# "Authentication token manipulation error". Bind it first if not using arch-chroot:
#   mount --bind /dev /mnt/target/dev && mount -t proc proc /mnt/target/proc
passwd <user>
echo '%wheel ALL=(ALL:ALL) ALL' > /etc/sudoers.d/wheel

# Set a root password - do NOT lock the account.
# systemd's emergency.target and rescue.target both prompt for it. A locked root on a
# rescue stick means a failed boot drops you at a prompt you cannot authenticate to,
# leaving `init=/bin/bash` as the only way in. Daily use still goes through sudo.
passwd root

systemctl enable NetworkManager lightdm sshd ufw
# stalls boot up to 90s when the stick has no network
systemctl disable NetworkManager-wait-online.service
printf '[zram0]\nzram-size = min(ram/2, 4096)\n' > /etc/systemd/zram-generator.conf
```

### sshd + ufw

Unchanged. This stick joins untrusted networks, so sshd is key-auth only and
root login is off:

```bash
cat > /etc/ssh/sshd_config.d/10-hardening.conf <<'EOF'
PermitRootLogin no
PasswordAuthentication no
KbdInteractiveAuthentication no
EOF
```

Drop your pubkey in before first boot or you will lock yourself out of sshd
entirely:

```bash
install -d -m700 -o <user> -g <user> /home/<user>/.ssh
# copy authorized_keys in, then:
chmod 600 /home/<user>/.ssh/authorized_keys
chown <user>:<user> /home/<user>/.ssh/authorized_keys
```

ufw defaults to deny-incoming, which would silently swallow sshd unless the
port is opened:

```bash
ufw default deny incoming
ufw default allow outgoing
ufw allow ssh
ufw enable
```

> If you would rather sshd not listen the moment the stick joins a network,
> `systemctl disable sshd` and `ufw deny ssh`, then bring both up by hand when
> needed. Everything else here stays the same.

### The p3 keyfile, so `/efi` mounts itself

**NEW.** p3 has to be open on a running system, and the passphrase is not
available to ask for again. Add a second p3 keyslot holding a random keyfile
on the already-decrypted root:

```bash
install -d -m700 /etc/cryptsetup-keys.d
( umask 077; head -c 64 /dev/urandom > /etc/cryptsetup-keys.d/cryptefi.key )
cryptsetup luksAddKey ${DISK}-part3 /etc/cryptsetup-keys.d/cryptefi.key
```

Then the `/etc/crypttab` line from `tools/archusb/crypttab.snippet`, with the
p3 LUKS UUID substituted.

Three things about that, all of them checked rather than assumed:

- **Use argon2id, cryptsetup's default. Do not pass `--pbkdf pbkdf2` here.**
  `src/luks/luks.c` marks a keyslot usable only when its KDF type is pbkdf2,
  and the unlock tries the passphrase against every usable slot, failing with
  `LUKS_ERR_UNSUPPORTED_KDF` only when none existed. An argon2id slot is
  therefore skipped outright and costs the loader nothing, whereas a second
  pbkdf2 slot at mkstick's 200000 iterations would make every boot burn a
  full extra derivation against a slot it can never open.
- **It gives nothing away.** The keyfile lives inside a filesystem already
  encrypted under the same passphrase that opens p3. Anyone who can read it
  has already opened the volume it unlocks. DESIGN Section 7.5 names this
  concentration as an accepted residual; this does not add to it.
- **UNVERIFIED alternative.** The passphrase does reach the initramfs as an
  sd-stub credential. Whether it is still readable in the real root after the
  pivot (i.e. whether PID 1 materialises stub credentials under
  `/run/credentials/@system` and whether those survive switch-root) was not
  established while writing this, so nothing here depends on it. If
  `/run/credentials/@system/cryptsetup.passphrase` turns out to exist on a
  booted system, a simpler crypttab is possible and the keyfile can go.

### mkinitcpio - the portability-critical bit

`/etc/mkinitcpio.conf`, **unchanged**:

```
MODULES=()
BINARIES=()
FILES=()
HOOKS=(base systemd microcode keyboard sd-vconsole modconf voyager-trim block sd-encrypt lvm2 filesystems fsck falcon-credential)
COMPRESSION="zstd"
```

**`autodetect` is deliberately absent.** It trims the initramfs to the modules
present on the building machine - fatal for a stick meant to boot arbitrary
hardware. Its absence is why this build works in a chroot at all. Do not add
it back.

**`systemd` and `sd-encrypt` carry the credential into the initramfs**, and
they are already there. `falcon-credential` is the one hook of ours, and it is
what makes the credential actually get *used*; it is appended last because it
only drops a unit override and depends on nothing.

The earlier claim here - that stock `systemd-cryptsetup` consumes
`cryptsetup.passphrase` with no hook of ours - was wrong, and was proved wrong
on 2026-09-13. sd-stub does land the credential at
`/.extra/credentials/cryptsetup.passphrase.cred`, but PID 1 files everything
arriving by that route under `/run/credentials/@encrypted/`, because the ESP is
untrusted and credentials delivered through it are expected to be
`systemd-creds` blobs. The generated unit's `ImportCredential=cryptsetup.*`
therefore tries to *decrypt* the plaintext passphrase, fails, and the service
dies at step `CREDENTIALS` with `EINVAL` (status 243) - before
`systemd-cryptsetup` runs at all, so not even the interactive fallback prompt
happens. The symptom is a bare `Failed to start Cryptography Setup for
cryptusb` and a drop into emergency mode.

`tools/archusb/falcon-credential` clears `ImportCredential=` and names the file
by path instead, taking the plaintext route. With it the unlock succeeds from
the credential with no prompt. It is an install hook rather than a hand-edit of
the initramfs precisely so that the next `mkinitcpio -P` keeps it.

**It bites a second time after switch-root, and the initramfs hook does not
cover that.** `/run/credentials/@encrypted/` is on `/run`, which is the same
tmpfs before and after the pivot, so the credential is still sitting there when
the real root's generators run. Every `systemd-cryptsetup@.service` that
`systemd-cryptsetup-generator` writes from `/etc/crypttab` gets the same
`ImportCredential=cryptsetup.*` and dies the same way - including `cryptefi`,
which has a perfectly good keyfile it never gets as far as opening. Observed
2026-09-15 on the laptop: a clean boot, p4 unlocked from the credential, and
`/efi` silently not mounted behind `Failed to start Cryptography Setup for
cryptefi`. `nofail` is why it is silent rather than fatal, and an unmounted
`/efi` means the next `mkinitcpio -P` writes the UKI into a directory on the
root filesystem instead of onto p3, so the stick keeps booting the old kernel.

`tools/archusb/falcon-credential.conf` is the real-root half: the same
`ImportCredential=` clear, installed into `/etc/systemd/system`. It deliberately
does **not** add a `LoadCredential=`. Everything in this `/etc/crypttab` unlocks
from a keyfile, and a `LoadCredential=` naming a file that is absent on a boot
that did not come through the loader would fail the unit for a credential
nothing in the real root needs.

**`voyager-trim` stays exactly where it is in HOOKS**, ahead of `block`. See
TRIM below; the reason is unchanged because p4 still stacks dm-crypt and LVM.

Observed result on the old build: 1107 modules, UKI **73 MB** (91 MB at one
point). `ext4`, `ahci`, `sd_mod`, `xhci` and `btrfs` will not appear as `.ko`
files - they are compiled into the Arch kernel image. That is not a gap.

> **Size p3 with that number in mind.** A 90 MB UKI, a rollback copy, the
> signing-appliance UKI and a 15 MB kernel copy is most of half a gigabyte.
> 1 GiB is comfortable; 512 MiB would not be.

`/etc/kernel/cmdline` - see `tools/archusb/kernel-cmdline`, and substitute the
**new** p4 LUKS UUID in both places:

```
rd.luks.name=<p4-luks-uuid>=cryptusb rd.luks.options=<p4-luks-uuid>=discard root=/dev/mapper/vgusb-root rootfstype=ext4 rw fsck.repair=yes systemd.gpt_auto=0
```

**CHANGED, and only in the ways listed. What does NOT change is the more
interesting half:**

- `root=/dev/mapper/vgusb-root` is **unchanged**, and stays unchanged as long
  as the VG is `vgusb` and the LV is `root`. It names a device-mapper node,
  not a UUID, so it is indifferent to the container being recreated.
- `rootfstype=ext4`, `rw` and `fsck.repair=yes` are **unchanged**.
- The **LUKS UUID changes**, in both `rd.luks.name=` and `rd.luks.options=`.
  The old one, `ff827808-e497-40bd-8e4c-04f4e08de29f`, belonged to the old
  ARCHLUKS partition. mkstick made a new container with a new UUID.
- **`/etc/fstab` needs no edit at all**, provided the new logical volumes are
  created with `mkfs.ext4 -U` preserving the old *filesystem* UUIDs. The LUKS
  UUID and the filesystem UUIDs are different things; only the former is
  forced to change.

**`rd.luks.name=` is still the right formulation, and the credential handoff
is the reason, not an argument against it.** `rd.luks.name=<uuid>=<name>` is
what makes `systemd-cryptsetup-generator` emit a
`systemd-cryptsetup@cryptusb.service` in the initrd for that container. That
generated unit is the thing that carries `ImportCredential=cryptsetup.*`, so
it is precisely the unit that picks the passphrase up out of
`/.extra/credentials/` and opens p4 without prompting. Replace it with
something that bypasses the generator and the credential has nothing to be
consumed by.

Two details worth knowing about the credential:

- It is named `cryptsetup.passphrase`, the **generic** form that applies to
  every volume `systemd-cryptsetup` handles, not the per-volume
  `cryptsetup.passphrase.cryptusb`. Only p4 is opened in the initrd, so there
  is no ambiguity here - but if a second container is ever added to the
  initramfs, that generic name will be tried against it too.
- Its content is raw passphrase bytes, no trailing newline and no NUL. That is
  the loader's business, not this install's, but it is why nothing here needs
  to strip anything.

**`systemd.gpt_auto=0` is NEW.** `systemd-gpt-auto-generator` discovers an ESP
on the same disk as the root filesystem and mounts it, at `/efi` if that is
free and otherwise at `/boot`. On this stick that ESP is **p1**, the falcon
one, holding shim and the loader - the last filesystem that should appear
read-write in a desktop session. Disabling the generator costs nothing here,
because `root=`, `rd.luks.name=` and `/etc/fstab` already name everything
explicitly and nothing is left to discover. **UNVERIFIED:** whether the
generator would in fact reach p1 through dm-crypt and LVM was not tested. The
flag is cheap enough that it was not worth finding out the other way.

> Do not put a comment in `/etc/kernel/cmdline`. It is passed to `ukify` with
> `--cmdline`, and whether ukify strips `#` lines was not confirmed; if it
> does not, the comment becomes kernel arguments. The file ships as exactly
> one line for that reason.

`/etc/mkinitcpio.d/linux.preset` - see `tools/archusb/linux.preset`:

```
ALL_kver="/efi/vmlinuz-linux"
PRESETS=('default')
default_uki="/efi/uki/arch-usb.efi"
default_options="--cmdline /etc/kernel/cmdline"
```

**CHANGED in three ways, and the middle one was the question worth asking.**

- `default_uki` moves to `/efi/uki/`. The loader looks in p3:`/uki` first and
  falls back to p3:`/EFI/Linux` (DESIGN Section 5); `/uki` is where mkstick
  stages, so it is the one that needs no explanation later.
- `default_image` is **deleted**. It existed only for the legacy-BIOS GRUB
  menuentry, which loaded `/vmlinuz-linux` and `/initramfs-linux.img`
  directly. falcon-boot is UEFI x64 only. Dropping it saves about 75 MB on a
  1 GiB p3.
- `ALL_kver` still resolves, and this was **checked against the backup rather
  than assumed**. No package owns `/boot/vmlinuz-linux`: Arch's `linux`
  package ships the kernel only at `usr/lib/modules/<kver>/vmlinuz`. The copy
  is maintained by mkinitcpio's own pacman script
  (`/usr/share/libalpm/scripts/mkinitcpio`): `is_kernelcopy()` returns true
  for an absolute path that no package owns, and `install_kernel()` then
  installs the real kernel to it on every transaction that touches
  `usr/lib/modules/*/vmlinuz`. mkinitcpio reads the version out of that file.
  So the idiom is supported, and it works at `/efi/vmlinuz-linux` for exactly
  the same reason it worked at `/boot/vmlinuz-linux` - that path is owned by
  nothing either.

  The trap it carries is the one named above: the copy is refreshed only
  during a package transaction, and only if the directory is reachable.

```bash
mkdir -p /efi/uki && mkinitcpio -P
```

> **The `*-ucode.img` files now land on the root LV and are unused.** The
> `amd-ucode` and `intel-ucode` packages install `/boot/amd-ucode.img` and
> `/boot/intel-ucode.img`; `/boot` is now an ordinary empty directory on the
> root LV. Harmless, about 10 MB, and not worth fighting: the `microcode`
> hook in HOOKS builds the microcode into the UKI itself from
> `/usr/lib/firmware`, which is what actually gets loaded. Do not delete the
> packages to save the space - the hook needs them.

### Pin generic tuning

Unchanged. Neither `google-chrome` nor `visual-studio-code-bin` compiles
anything (both repack upstream binaries) and yay is Go (`GOAMD64` defaults to
baseline `v1`), so nothing in the planned set can pick up native tuning. This
is insurance against a future careless PKGBUILD.

**Do not sed `CFLAGS`.** Arch already ships `-march=x86-64 -mtune=generic`, so
there is nothing to fix there - and `CFLAGS` is a *multi-line continuation*. A
`sed 's|^CFLAGS=.*|...|'` replaces only the first line, orphans the
continuation lines, and leaves an unterminated string that breaks `makepkg`
with `unexpected EOF while looking for matching \``. It also silently drops
Arch's hardening flags (`_FORTIFY_SOURCE=3`, `-fstack-clash-protection`,
`-fcf-protection`).

Append only, never edit:

```bash
cat >> /etc/makepkg.conf <<'EOF'

#-- Portability pins for this USB install: never build host-tuned binaries
RUSTFLAGS="-C target-cpu=x86-64"
export GOAMD64=v1
EOF
bash -n /etc/makepkg.conf   # must parse
```

If it is already broken, restore from the package rather than patching:
`bsdtar -xf /var/cache/pacman/pkg/pacman-[0-9]*.pkg.tar.zst etc/makepkg.conf`

Exit both chroots, then unmount and close - **CHANGED, there are two mappers
to close now**:

```bash
umount -R /mnt/target
vgchange -an vgusb
cryptsetup close cryptusb
cryptsetup close cryptefi
```

---

## 8. Sign the UKI

**CHANGED wholesale, and this section is the biggest single improvement in the
migration.**

The old model: copy the Gentoo workstation's own Secure Boot **db private
key** onto the stick, into `/var/lib/sbctl/keys/db/`, and sign the UKI with
it. It worked, and the old runbook named the consequence honestly - "your
Secure Boot signing key now exists on a device you carry; if the stick is lost,
an attacker who breaks the LUKS passphrase can sign binaries your
workstation's firmware trusts."

**That residual is now retired.** falcon-boot generates its own MOK keypair
per stick. The private half lives on p3 at `/efi/falcon-mok.key`, mode 0600,
inside the LUKS volume and nowhere else (DESIGN Section 7.5). The public half
is on p1 as `falcon-boot.cer` and is what gets enrolled. Losing the stick now
costs you the stick's own key and nothing of the workstation's.

Two consequences follow, and both are improvements:

- **Rotation is local.** Re-running `falcon-boot-mkstick` with a new keypair
  means re-enrolling one MOK on the machines you use, not re-enrolling `db` on
  the Gentoo box.
- **The UKI is actually verified now.** On the Ventoy path it never was:
  Ventoy's GRUB chainloaded `arch-usb.efi` without routing the load through
  the security protocol shim installs, so the signature was not consulted on
  any machine, ours included. Secure Boot vouched for the bootloader and
  nothing after it. falcon launches the UKI through the shim-verified load
  path (DESIGN Section 7.6), so shim checks our signature against MOK,
  honestly, every time.

### What to install

From `tools/archusb/`:

| File | Destination | Mode |
|---|---|---|
| `sign-uki` | `/usr/local/bin/sign-uki` | 0755 |
| `zz-sign-uki` | `/usr/lib/initcpio/post/zz-sign-uki` | 0755 |
| `95-sign-uki.hook` | `/etc/pacman.d/hooks/95-sign-uki.hook` | 0644 |
| `falcon-credential` | `/etc/initcpio/install/falcon-credential` | 0644 |
| `falcon-credential.conf` | `/etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf` | 0644 |
| `linux.preset` | `/etc/mkinitcpio.d/linux.preset` | 0644 |
| `voyager-trim` | `/etc/initcpio/install/voyager-trim` | 0755 |
| `10-voyager-gtx-trim.rules` | `/etc/udev/rules.d/10-voyager-gtx-trim.rules` | 0644 |
| `kernel-cmdline` | `/etc/kernel/cmdline` | 0644, UUID substituted |
| `fstab.snippet` | `/etc/fstab` | 0644, whole file |
| `crypttab.snippet` | `/etc/crypttab` | 0644, one added line, UUID substituted |

`sign-uki` is the old script with four edits and nothing else: the default UKI
path, the key path, the cert path, and the wording of the "key missing" hint.
Its comments are kept verbatim because each one records a bug that was
actually hit:

> **Strip before signing, or the signatures pile up.** `sbsign` APPENDS a
> certificate entry and never replaces one, so the original version of this
> script added a second identical signature on every kernel update. Firmware
> boots it regardless - any one valid entry is enough - which is why it went
> unnoticed until the UKI was inspected on 2026-09-05 and found carrying two.
> `sbattach --remove` strips one entry per call, not the whole table, hence
> the loop.

> **`sbverify --cert X` returns 0 for binaries X did not sign.** It cannot be
> used to tell our signature from anyone else's. Compare the signer subject
> instead - `sbverify --list` and `openssl x509 -nameopt compat` print the
> same slash-separated form. That comparison is why `sign-uki` derives `want`
> from the certificate rather than hardcoding it, and it is why the expected
> subject needed no edit in the migration: with mkstick's default CN it
> resolves to `/CN=falcon-boot MOK`, and if you passed `--cn` it resolves to
> yours. **UNVERIFIED:** that exact string was read out of
> `src/cli/mkstick/keys.c`, not off a real stick. It fails closed, so a
> mismatch refuses to install the image rather than installing an unverified
> one. Check it on the first run.

One property that is new: the scratch names `*.unsigned.$$` and `*.signed.$$`
do not end in `.efi`, and the loader enumerates `/uki` for a case-insensitive
`.efi` suffix. A crash mid-sign leaves litter, never a half-written image in
the boot menu. Both scratch files are still created beside the UKI, so the
final `mv` is still within one filesystem and still atomic.

### sbctl is gone

**CHANGED.** `sbctl sign` never worked with a db-only keystore anyway - it
opens `keys/KEK/KEK.key` and fails with *"no such file or directory"* - which
is why its pacman hook had to be neutralised with an empty file. There is now
no keystore at all, so:

```bash
pacman -Rns sbctl
rm -f /etc/pacman.d/hooks/zz-sbctl.hook     # the neutraliser, now pointless
```

If you would rather keep `sbctl` for `sbctl status` (it reads firmware state
and needs no keys), keep the empty neutraliser file as well. An empty
`/etc/pacman.d/hooks/<name>.hook` shadows the one in
`/usr/share/libalpm/hooks/` and makes pacman skip it.

### Sign now and verify

```bash
arch-chroot /mnt /usr/local/bin/sign-uki
sbverify --cert /mnt/efi/falcon-mok.crt /mnt/efi/uki/arch-usb.efi
```

**CHANGED:** the old note said `sbctl verify` fails because it looks for an
ESP and `/boot` was typed `0700`. It now fails because there is no keystore.
Use `sbverify` either way.

### Keep a rollback UKI, and mind the sort order

**NEW, and it is nearly free.** The old stick kept `arch-usb-prev.efi` beside
the live UKI on the FAT partition. Keep doing it - a stick whose job is
rescuing other machines should be able to rescue itself from a bad kernel.

The loader offers a chooser when `/uki` holds more than one image, sorted by
name (DESIGN Section 5). `-` is 0x2D and `.` is 0x2E, so `arch-usb-prev.efi`
sorts **before** `arch-usb.efi`. If you would rather the live image came
first, name the rollback something that sorts after it. The signing-appliance
UKI is in the same directory and in the same sort, so pick its name
deliberately too.

---

## 9. Enrollment, and what replaced the Ventoy layer

**DELETED: the entire `ventoy_grub.cfg` layer.** Read what it was before
throwing it away, because both of its workarounds were real and both were
documented in its own comments:

```
# UEFI: signed UKI. Ventoy renders its menu with gfxterm; leaving gfxterm
# active across the chainload leaves GOP in a state the EFI stub cannot pass
# through as screen_info, so the kernel gets a null framebuffer and all early
# boot output is lost. Switching back to text mode immediately before the
# handoff restores it, while keeping the menu itself graphical.
# Force text mode for this submenu: Ventoy enters it via configfile without
# switching terminal, so a themed gfxmenu carries in and swallows the inherited
# timeout - an auto-selected entry then highlights but never fires. console mode
# uses grub plain menu loop, where the timeout reliably counts down and boots.
terminal_output console
```

So: **two** workarounds, for two different bugs.

1. **The GOP/`screen_info` handoff.** GRUB's `gfxterm` leaves the graphics
   output protocol in a state the EFI stub cannot turn into a `screen_info`,
   so the kernel came up with a null framebuffer and every message before
   simpledrm (about 0.77 s) went into the void. Symptom: black screen from
   menu selection to the LUKS prompt, and a mangled prompt.
2. **The swallowed timeout.** Ventoy enters the custom menu with `configfile`
   without switching terminal, so a themed `gfxmenu` carried in, and a nested
   `gfxmenu` swallows the inherited `timeout` - the auto-selected entry
   highlighted but never fired. Only plain console mode's menu loop counted
   down reliably.

**Neither failure mode exists on falcon, so the workaround is dropped rather
than ported.** The loader draws its menu on the firmware's own Simple Text
Output protocol (`src/menu`); it never initialises a graphics terminal, so
there is no `gfxterm` to switch out of and GOP is left exactly as the firmware
set it up. And it launches with `LoadImage`/`StartImage` through the
shim-verified path, not with GRUB's `chainloader`, so nothing about GRUB's
terminal state is in the handoff at all. There is also no nested `configfile`
menu, so there is no inherited timeout to swallow.

Two smaller things go with it:

- **`earlycon=efifb` stays deleted.** It never helped: it registered but
  reported `efifb0 at I/O port 0x0`, a null framebuffer address, and wrote
  every early message into the void. Tested both with and without the
  terminal fix. Do not re-add it on the theory that the new loader changed
  something.
- **The duplicated cmdline trap is gone.** `fsck.repair=yes` and
  `rd.luks.options=...=discard` had to be written in **two** places, in
  `/etc/kernel/cmdline` and in the BIOS menuentry, and kept in sync by hand.
  There is one cmdline now.

**Also gone: the legacy-BIOS entry, as a capability.** It was genuinely useful
- GRUB loaded the kernel with `linux`/`initrd` and populated `screen_info`
itself, so it was the entry that showed early boot most reliably - and it had
no Secure Boot protection at all: editable cmdline, unsigned initramfs.
falcon-boot is UEFI x64 only (DESIGN Section 1), so it has no equivalent. If
you need to watch early boot on a machine that is misbehaving, boot a rescue
ISO from the menu rather than expecting a BIOS entry to be there.

### What replaces it

Nothing on the stick. The loader enumerates p2 for ISOs and offers the
personal-OS entry itself, so there is no menu file to write, no `--id` to tag,
no theme to clone and no `ventoy.json` to edit. The whole of the old Section 9
plus "Ventoy menu appearance" plus "Ventoy: updating from the stick" plus
"Ventoy hybrid mode" is **DELETED**. So are `ventoy-update`,
`archusb-ventoy-check.{service,timer}` and `/var/lib/archusb/ventoy-mode`.

One consolidation worth stating, because it was the point of all that work:
the Ventoy stick needed **two** enrollments to boot under Secure Boot on a
foreign machine (Microsoft's UEFI CA for the shim, plus Ventoy's own CA as a
MOK for `fbx64.efi`), and the cert changed between Ventoy versions, so every
update re-broke it. falcon needs **one**: shim is Fedora's, dual MS-signed
(2011 and 2023), and the only MOK to enroll is our own `falcon-boot.cer`.

### First boot: enroll the MOK

On each machine, once:

1. Boot the stick. Firmware verifies shim against db; shim cannot verify our
   loader against db, so it consults MOK, finds nothing, and launches
   MokManager.
2. In MokManager: *Enroll key from disk* -> the ESP -> `falcon-boot.cer`.
   Confirm, reboot.
3. Second boot: shim verifies our loader against MOK, the loader runs, and the
   menu comes up.

The `ENROLL_THIS_KEY_FOR_ARCH.cer` that sat on the old ARCHBOOT partition is
**DELETED** - it was the workstation's db cert, for a key that no longer
exists on this stick.

### Boot entries: there is exactly one now

**CHANGED, and it removes a bug rather than adding one.** The old stick had
**two** firmware boot entries on a machine set up for it: the Ventoy loader on
VTOYEFI, and a direct entry for the UKI on ARCHBOOT. That ambiguity is what
made `archusb-boot-next` take `BootCurrent` and get it wrong (see the reboot
chooser section below).

falcon has one: the removable-media path `\EFI\BOOT\BOOTX64.EFI` on p1. A
direct firmware entry for the UKI is **not possible any more** and that is by
design - the UKI is inside a LUKS2 volume that only the loader can open.
Anything matching the stick's boot entry by partition GUID now has exactly one
candidate.

---

## 10. Verify in qemu before trusting it

Unchanged in shape. The overlay makes the VM physically unable to write to the
real stick.

```bash
qemu-img create -f qcow2 -b $(readlink -f $DISK) -F raw /mnt/980pro/sticktest.qcow2
cp /usr/share/OVMF/OVMF_VARS.fd /mnt/980pro/ovmf-vars.fd

qemu-system-x86_64 -enable-kvm -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/mnt/980pro/ovmf-vars.fd \
  -device nec-usb-xhci,id=xhci \
  -drive file=/mnt/980pro/sticktest.qcow2,format=qcow2,if=none,id=stick \
  -device usb-storage,bus=xhci.0,drive=stick \
  -vga virtio -display gtk
```

**CHANGED, the reason for USB emulation.** It is no longer "so Ventoy sees the
device shape it expects" - it is so the stick is enumerated the way it will be
on real hardware, which is what the loader's device discovery walks.

**CHANGED, the chain to walk:** falcon menu -> the personal-OS entry -> the
loader's masked passphrase prompt -> p3 unlock -> UKI launch -> ext4 root ->
lightdm. Note there is now **one** passphrase prompt and it is the *loader's*,
not the initramfs's: if you are asked a second time, the credential handoff
did not work and that is the thing to debug.

Test with Secure Boot **off** first. Under Secure Boot on, the stick's own
MOK has to be enrolled in the VM's MokList or shim will refuse the loader, and
a failure there tells you nothing about whether the rest works.

> A qcow2 overlay backed by `/dev/sda` reads the WHOLE-DISK page cache, which
> does not see writes made through a mounted partition. A VM booted that way
> ran pre-signing binaries and reported Access Denied, which looks exactly
> like firmware rejecting the signature. Before every run:
>
>     sync; blockdev --flushbufs /dev/sda; echo 1 > /proc/sys/vm/drop_caches
>
> and re-create the overlay afterwards.

The repo's own rig (`tools/run-qemu.sh`, `make run-stick` /
`run-stick-secboot`) is the better harness for testing the loader itself; this
one is for testing the installed OS.

---

## 11. First boot on real hardware

Unchanged.

```bash
sudo pacman -S --needed git base-devel
git clone https://aur.archlinux.org/yay.git && cd yay && makepkg -si
yay -S google-chrome visual-studio-code-bin
```

`makepkg` refuses to run as root, which is why this cannot happen during
pacstrap.

**NEW check on the first real boot**, because it is the one thing the QEMU rig
could not prove (DESIGN Section 5.1 says so explicitly): that a full systemd
initramfs really does unlock p4 from the credential. You will know within
seconds - one prompt means it worked, two means it did not.

---

## Rollback

**CHANGED.** The old rollback levers were a `sgdisk` backup, the Ventoy data
backup and `Ventoy2Disk.sh -u`. The new ones:

- **The migration backup**,
  `/mnt/980pro/falcon-migration-20260913/`, which is what RESTORE below
  consumes. Keep it until the new stick has survived a few real boots.
- **The LUKS headers.** Take them before touching anything, for both
  partitions. Without the header the data is unrecoverable even with the
  passphrase:

  ```bash
  cryptsetup luksHeaderBackup ${DISK}-part3 --header-backup-file p3-luks.img
  cryptsetup luksHeaderBackup ${DISK}-part4 --header-backup-file p4-luks.img
  ```

  Store them somewhere encrypted. A LUKS header backup plus the passphrase is
  the volume.
- **The GPT**, `sgdisk --backup=gpt.bin $DISK`, though re-running mkstick
  rebuilds it.
- **`falcon-boot-mkstick`** itself: the p1 contents are reproducible from the
  repo plus the shim package, so nothing on the ESP needs backing up. The MOK
  **key** does - it is on p3 only, and losing it means every previously signed
  artifact has to be re-signed and the MOK re-enrolled everywhere.

## Standing warning

**CHANGED, and the old one is retired.** "Never run `Ventoy2Disk.sh -I` on
this stick again" no longer applies. Its replacement:

**Never run `falcon-boot-mkstick create` on a stick that already holds the
install.** It repartitions and reformats. There is no `-u`-style update mode
that preserves p3 and p4.

**Never `luksFormat` p3 or p4 after the fact.** The loader's p3 unlock depends
on the pbkdf2 keyslot mkstick wrote, and the p3/p4 shared passphrase is what
makes the credential handoff a single prompt. Add keyslots; never replace the
header.

---

# RESTORE: put the 2026-09-13 backup into the new p4

**NEW section.** The old runbook built from scratch; this one puts an existing
install back. Sections 5, 6 and the desktop half of 7 are skipped entirely -
they are already in the backup.

Everything below assumes `falcon-boot-mkstick create` has already run and the
stick is in the Section 1 shape. Read the whole section before starting; two
steps are irreversible and one needs a decision.

### R0. Set up, and check you have the right disk

```bash
ls -l /dev/disk/by-id/ | grep -i voyager
DISK=/dev/disk/by-id/<whatever that printed>
readlink -f $DISK
sgdisk -p $DISK        # 1 FALCON-ESP, 2 FALCON-ISO, 3 FALCON-UKI, 4 FALCON-ROOT
lsblk -o NAME,SIZE,FSTYPE,PARTLABEL $(readlink -f $DISK)

B=/mnt/980pro/falcon-migration-20260913
```

Nothing below should ever name `/dev/sda`.

### R1. Take the header backups first

Irreversible steps follow. Do this even though the stick was made five minutes
ago - it is the only thing that makes R2 recoverable.

```bash
cryptsetup luksHeaderBackup ${DISK}-part3 --header-backup-file /mnt/980pro/p3-luks.img
cryptsetup luksHeaderBackup ${DISK}-part4 --header-backup-file /mnt/980pro/p4-luks.img
sgdisk --backup=/mnt/980pro/falcon-gpt.bin $DISK
```

### R2. Replace mkstick's ext4 inside p4 with LUKS -> LVM -> ext4

**Keep mkstick's keyslot.** Do not run `luksFormat`. The header on p4 carries
the argon2id keyslot for the passphrase the loader prompts for and hands to the
initramfs; recreating it breaks the single-prompt boot and orphans the p3
passphrase pairing.

```bash
cryptsetup open ${DISK}-part4 cryptusb        # mkstick's passphrase

# Sanity: this must say LUKS2, and the keyslot must be argon2id.
cryptsetup luksDump ${DISK}-part4 | sed -n '1,40p'

# mkstick put a plain ext4 (LABEL=FALCON-ROOT) in here. Remove the signature.
# The LUKS header lives on the partition, outside the mapper, so it is not
# touched by anything addressed at /dev/mapper/cryptusb.
wipefs -a /dev/mapper/cryptusb

pvcreate /dev/mapper/cryptusb
vgcreate vgusb /dev/mapper/cryptusb
vgs vgusb                                     # note VSize before sizing home
lvcreate -L 32G -n root vgusb
lvcreate -l 100%FREE -n home vgusb
```

**Now the part that keeps `/etc/fstab` unedited.** Recreate the filesystems
with the *old* UUIDs, taken from `RESTORE-NOTES.txt`:

```bash
mkfs.ext4 -L USBROOT -U 13faf1a5-d8b7-429f-86ac-b4c9e9b46795 /dev/vgusb/root
mkfs.ext4 -L USBHOME -U 4c712605-d751-4d13-bcf6-3bb106059cea /dev/vgusb/home
```

Verify before going further, because a typo here is only discovered at boot:

```bash
blkid /dev/vgusb/root /dev/vgusb/home
# must print exactly those two UUIDs, and LABEL=USBROOT / USBHOME
```

**JUDGEMENT CALL: `home` is now larger.** The old container was 128 GiB with
`home` at 96.01 GiB. Depending on how p2 was sized, the new p4 is around 141
GiB, so `-l 100%FREE` gives `home` roughly 109 GiB. That is fine and the
restore fits. If you would rather leave slack for LVM snapshots, use
`lvcreate -L 96G -n home vgusb` instead and keep the remainder in the VG - the
old runbook's note that ext4 shrinks offline still applies either way, so this
is not a one-way decision.

**Also check `stripe=`.** The old fstab carried `stripe=8191` on both
filesystems, an ext4 allocation hint genfstab picked up from the old LV
geometry. Read what mkfs computed this time and make fstab agree:

```bash
tune2fs -l /dev/vgusb/root | grep -i -e stride -e 'stripe width'
```

It is a hint, not a correctness matter. Do not lose sleep over it; do not
carry an obviously wrong number forward either.

### R3. Mount the target, including `/efi`

```bash
cryptsetup open ${DISK}-part3 cryptefi        # the same passphrase

mkdir -p /mnt/target
mount -o noatime /dev/vgusb/root /mnt/target
mkdir -p /mnt/target/home /mnt/target/efi
mount -o noatime /dev/vgusb/home /mnt/target/home
mount -o noatime /dev/mapper/cryptefi /mnt/target/efi

df -h /mnt/target /mnt/target/home /mnt/target/efi   # three different devices
ls /mnt/target/efi                                   # uki/  falcon-mok.key  falcon-mok.crt
```

If `/mnt/target/efi` does not already contain `uki/`, `falcon-mok.key` and
`falcon-mok.crt`, mkstick did not populate p3 and nothing below will work.
Stop and fix that first.

### R4. rsync the backup back

```bash
rsync -aHAXS --numeric-ids --info=progress2 $B/p4-root/ /mnt/target/
rsync -aHAXS --numeric-ids --info=progress2 $B/p4-home/ /mnt/target/home/
```

`--numeric-ids` matters: the restoring system is Gentoo and its `/etc/passwd`
is not the stick's. `-H` preserves hardlinks (pacman's cache and
`/usr/lib/modules` are full of them), `-A` ACLs, `-X` xattrs (systemd
capabilities live there), `-S` sparse files.

Checked while writing this, so you do not have to: `$B/p4-root/home` and
`$B/p4-root/boot` are both empty directories in the backup, and
`/proc`, `/sys`, `/dev`, `/run` and `/tmp` are present but empty. Nothing
needs excluding and nothing collides with the `/home` mount.

Verify:

```bash
rsync -rnc --itemize-changes $B/p4-root/ /mnt/target/    # should print nothing
```

### R5. Deal with the workstation's db key

**Do this before anything else in the restored tree, and do it deliberately.**
The backup contains the Gentoo workstation's Secure Boot **db private key**:

```
/mnt/target/var/lib/sbctl/keys/db/db.key    3272 bytes, mode 0600
/mnt/target/var/lib/sbctl/keys/db/db.pem    1757 bytes
/mnt/target/var/lib/sbctl/GUID
```

It has no job on a falcon stick: signing is done with the MOK key on p3. The
old runbook's "consequence worth naming once" - a key that signs binaries your
workstation's firmware trusts, on a device you carry - stops being true the
moment this is removed, and stays true for as long as it is not.

```bash
shred -u /mnt/target/var/lib/sbctl/keys/db/db.key
rm -rf /mnt/target/var/lib/sbctl
```

**JUDGEMENT CALL, and it is yours, not this document's:** if you intend to
keep booting this stick under Secure Boot on the Gentoo workstation *without*
enrolling the falcon MOK there, the db key is what made that work and removing
it ends it. The alternative is to enroll `falcon-boot.cer` as a MOK on the
workstation like any other machine, which is the model falcon is built around
and costs one MokManager visit. Decide before you shred.

### R6. Clear out the Ventoy layer

None of this has anything to drive any more.

```bash
T=/mnt/target

# Ventoy updater and its timer
rm -f  $T/usr/local/sbin/ventoy-update
rm -f  $T/etc/systemd/system/archusb-ventoy-check.service
rm -f  $T/etc/systemd/system/archusb-ventoy-check.timer
rm -f  $T/etc/systemd/system/timers.target.wants/archusb-ventoy-check.timer
rm -rf $T/var/lib/archusb/ventoy-backup
rm -f  $T/var/lib/archusb/ventoy-mode $T/var/lib/archusb/ventoy-latest \
       $T/var/lib/archusb/ventoy-version

# Reboot chooser: see "Reboot chooser" below before deleting, if you want to
# keep the BootNext half of it.
rm -f  $T/usr/local/sbin/archusb-boot-next
rm -f  $T/usr/local/bin/archusb-reboot
rm -f  $T/etc/systemd/system/archusb-boot-next-reset.service
rm -f  $T/etc/systemd/system/multi-user.target.wants/archusb-boot-next-reset.service
rm -f  $T/etc/sudoers.d/archusb-boot-next
rm -f  $T/usr/share/applications/archusb-reboot.desktop
rm -f  $T/usr/local/share/archusb/bootnext-install.sh

# Old enrollment cert and the on-stick runbook, replaced by this document
rm -f  $T/usr/local/share/archusb/RUNBOOK.md
```

Then put this runbook on the stick, since the old one lived there for a
reason:

```bash
install -Dm644 docs/archusb-runbook.md $T/usr/local/share/archusb/RUNBOOK.md
```

**Reset the stick-specific state** that the old runbook says to rewrite on a
rebuild:

```bash
: > $T/var/lib/archusb/retract-log
rm -f $T/var/lib/archusb/last-backup
# smart-baseline: rewrite it from the current counters once the stick boots,
#   smartctl -d sat -A /dev/<stick>
# The SMART counters themselves are lifetime and cannot be reset; the baseline
# file is what makes "since install" meaningful, and it is now stale.
```

### R7. Apply the falcon configuration

From this repo's `tools/archusb/`:

```bash
T=/mnt/target
A=<path to airies-boot>/tools/archusb

install -Dm755 $A/sign-uki                  $T/usr/local/bin/sign-uki
install -Dm755 $A/zz-sign-uki               $T/usr/lib/initcpio/post/zz-sign-uki
install -Dm644 $A/95-sign-uki.hook          $T/etc/pacman.d/hooks/95-sign-uki.hook
install -Dm644 $A/falcon-credential          $T/etc/initcpio/install/falcon-credential
install -Dm644 $A/falcon-credential.conf     $T/etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf
install -Dm644 $A/linux.preset              $T/etc/mkinitcpio.d/linux.preset
install -Dm755 $A/voyager-trim              $T/etc/initcpio/install/voyager-trim
install -Dm644 $A/10-voyager-gtx-trim.rules $T/etc/udev/rules.d/10-voyager-gtx-trim.rules
install -Dm644 $A/fstab.snippet             $T/etc/fstab
```

`voyager-trim` and the udev rule are in the backup already and are identical;
installing them again is a no-op that documents the intent.

Now the three files that need the new UUIDs. Read them once:

```bash
LUKS3=$(cryptsetup luksUUID ${DISK}-part3)
LUKS4=$(cryptsetup luksUUID ${DISK}-part4)
echo "p3 $LUKS3"; echo "p4 $LUKS4"
```

`/etc/kernel/cmdline` - one line, no comment (see Section 7):

```bash
sed "s/NEW_P4_LUKS_UUID/$LUKS4/g" $A/kernel-cmdline > $T/etc/kernel/cmdline
chmod 644 $T/etc/kernel/cmdline
cat $T/etc/kernel/cmdline      # read it. Both occurrences must be substituted.
```

`/etc/crypttab` - append the one line:

```bash
sed "s/NEW_P3_LUKS_UUID/$LUKS3/" $A/crypttab.snippet \
    | grep -v '^#' | grep -v '^[[:space:]]*$' >> $T/etc/crypttab
tail -3 $T/etc/crypttab
```

The p3 keyfile, which is what makes `/efi` mount itself:

```bash
install -d -m700 $T/etc/cryptsetup-keys.d
( umask 077; head -c 64 /dev/urandom > $T/etc/cryptsetup-keys.d/cryptefi.key )
cryptsetup luksAddKey ${DISK}-part3 $T/etc/cryptsetup-keys.d/cryptefi.key
# argon2id, cryptsetup's default. Do NOT pass --pbkdf pbkdf2 - see Section 7.
cryptsetup luksDump ${DISK}-part3 | grep -A2 '^  [0-9]*: luks2'
```

That dump should show mkstick's pbkdf2 slot and your new argon2id slot, and
nothing else.

### R8. Remove sbctl, rebuild and sign

```bash
arch-chroot $T pacman -Rns sbctl
rm -f $T/etc/pacman.d/hooks/zz-sbctl.hook

arch-chroot $T pacman -S --needed sbsigntools

mkdir -p $T/efi/uki
arch-chroot $T mkinitcpio -P
```

The mkinitcpio post hook signs it on the way out. Verify by hand anyway:

```bash
sbverify --list $T/efi/uki/arch-usb.efi        # exactly one signature
sbverify --cert $T/efi/falcon-mok.crt $T/efi/uki/arch-usb.efi
ls -la $T/efi/                                 # vmlinuz-linux should now exist
```

`vmlinuz-linux` appears because `ALL_kver` is a path no package owns, so
mkinitcpio's pacman script treats it as a kernel copy. If it is **not** there
after a `pacman -S linux`, `ALL_kver` is wrong - see Section 7.

### R9. Unmount, close, boot

```bash
umount -R /mnt/target
vgchange -an vgusb
cryptsetup close cryptusb
cryptsetup close cryptefi
sync
```

Then Section 10's QEMU run against a **snapshot** overlay, then real hardware.

### R10. Post-boot checklist

In order, because each answers a different question:

```bash
findmnt /efi                              # /dev/mapper/cryptefi, ext4
lsblk -D                                  # DISC-MAX nonzero on sda, cryptusb AND the LVs
cat /sys/block/sd?/device/scsi_disk/*/provisioning_mode    # "unmap", not "disabled"
sbverify --cert /efi/falcon-mok.crt /efi/uki/arch-usb.efi
archusb-rtc-probe --check                 # the RTC must NOT be being written
systemd-analyze blame | head              # nothing waiting 90s on the network
sudo /usr/local/share/archusb/test-timekeep.sh
```

And the one that only real hardware can answer: **you were asked for the
passphrase exactly once**. Twice means the sd-stub credential did not reach
`systemd-cryptsetup`, and the place to look is the initrd's
`systemd-cryptsetup@cryptusb.service` and whether
`/.extra/credentials/cryptsetup.passphrase` existed.

### What this section could not decide for you

Four things, listed so they are not discovered by accident:

1. **Whether to keep the workstation db key** (R5). Security-relevant, and it
   changes whether the stick boots under Secure Boot on the Gentoo box without
   a MOK enrollment.
2. **How to size `home`** (R2). The new container is bigger; taking all of it
   is the default, leaving snapshot room is defensible.
3. **Whether a "reboot into X" chooser is wanted at all** (see below). The
   Ventoy half of the old one cannot be ported and the falcon half is a design
   question for DESIGN, not something this runbook should invent.
4. **Whether `/efi` or `/boot`** (Section 4). This document commits to `/efi`
   and lists the five places to change if you disagree.

---

## TRIM - working (fixed 2026-09-07)

**Carried over almost entirely.** Nothing here was about Ventoy; it is about a
SandForce SSD behind a USB-SATA bridge and about device-mapper. The three
changes are marked inline.

### What is actually wrong with the bridge

READ CAPACITY(16) reports `lbpme=0` while the Logical Block Provisioning VPD
page (0xB2) advertises `LBPU=1` and Block Limits (0xB0) gives a Maximum unmap
LBA count of 4194240. Linux keys auto-enable off `lbpme` alone, so it sets
`provisioning_mode=full` and `discard_max_bytes=0` and never tries. That is a
known reporting quirk on this class of USB-SATA bridge, not a missing feature.

UNMAP was verified by hand with `sg_raw`, which bypasses the block layer
entirely and so needs no change to `provisioning_mode`:

    # zero-descriptor UNMAP: exercises the full command path, discards nothing
    printf '\x00\x06\x00\x00\x00\x00\x00\x00' > /tmp/u.bin
    sg_raw -s 8 -i /tmp/u.bin /dev/sda 42 00 00 00 00 00 00 00 08 00

Returned Good. A real 31.3 MiB UNMAP against the unallocated tail also
returned Good, and that range read back as all zeros afterwards despite
`LBPRZ=0`. The drive performs a real discard and returns zeros from trimmed
sectors, so the readback check the literature calls inconclusive is in fact
conclusive here.

### Why the old test saw ASC 0x24

`ASC 0x24` is INVALID FIELD IN CDB, not `ASC 0x20` INVALID COMMAND OPERATION
CODE. The bridge always understood UNMAP; it rejected one field.

Forcing `provisioning_mode=unmap` makes the sd driver compute
`discard_max_bytes=4294966784` (8388607 blocks, `SD_MAX_WS16_BLOCKS`), which
is exactly twice the 4194240 this device accepts. `sd_read_block_limits()`
only copies `max_unmap_blocks` out of VPD 0xB0 when `lbpme=1`, so with
`lbpme=0` it falls back to the constant. Any tool issuing a max-size discard
(`blkdiscard`, `mkfs -E discard`) then builds an oversized descriptor and
earns ILLEGAL REQUEST.

The clamp is load-bearing, but not for ext4. ext4 trims one block group at a
time, 128 MiB max, sixteen times below the device ceiling, so it can never
build an oversized descriptor. exFAT has no such cap: **CHANGED only in the
name of the partition** - it was the 108 GiB Ventoy partition and it is now
the 96 GiB `FALCON-ISO` partition, but it is the same filesystem with the same
tens of gigabytes of contiguous free space, so it still generates 4 GiB
requests and `fstrim` on it unclamped still reproduces:

    sd 1:0:0:0: [sda] Sense Key : Illegal Request [current]
    sd 1:0:0:0: [sda] Add. Sense: Invalid field in cdb
    critical target error, dev sda, op 0x3:(DISCARD)
    fstrim: FITRIM ioctl failed: Remote I/O error

So both pieces are needed, for different filesystems: `allow_discards` on
dm-crypt unblocks ext4, the clamp unblocks exFAT. What blocked *every*
filesystem-driven path was dm-crypt opened without `allow_discards`, so
nothing reached the bridge.

### The disable latch - check this before concluding TRIM is broken

The sd driver sets `provisioning_mode=disabled` after **any** ILLEGAL REQUEST
on a discard. It is device-wide and sticks for the rest of the boot, so a
single oversized request makes every later trim on every filesystem fail,
reporting `the discard operation is not supported` rather than the original
error. Check it first:

    cat /sys/block/sda/device/scsi_disk/*/provisioning_mode

If that reads `disabled`, the hardware is fine. Re-arm with

    udevadm trigger --action=change --subsystem-match=block --sysname-match=sda

or reboot.

### Effective unmap granularity is 1 MiB, not 1 block

VPD 0xB0 reports `Optimal unmap granularity: 1` with no alignment requirement.
That is under-reported by a factor of 2048. Measured against known-free space:

    64 KiB aligned unmap   -> Good status, data unchanged (silent no-op)
    1 MiB  aligned unmap   -> Good status, reads back as zeros
    partial 1 MiB unit     -> Good status, silently skipped

The FTL can only invalidate whole mapping units, so anything smaller or
misaligned is accepted and ignored. Two consequences. `fstrim` byte counts are
what the filesystem asked to discard, not what the device actually unmapped.
And verifying a discard by reading the range back needs a cache flush plus a
few seconds first (`sync; sg_sync /dev/sda; sleep 3`), otherwise a recently
written range still reads as data and looks like a failure. That cost an hour
and a wrong conclusion about granularity.

### The fix, as deployed

Four pieces, all of which travel with the stick.

1. `/etc/udev/rules.d/10-voyager-gtx-trim.rules` sets `provisioning_mode=unmap`
   and then clamps `discard_max_bytes` to 2147450880. Order is load-bearing:
   writing `provisioning_mode` recomputes the queue limits, so the clamp must
   follow it. Both assignments sit in one rule because udev guarantees
   ordering within a rule but not between rules. The glob in
   `ATTR{device/scsi_disk/*/provisioning_mode}` matters, since the H:C:T:L
   directory name changes with the USB topology of whatever machine the stick
   is in. **Unchanged.**

2. `/etc/initcpio/install/voyager-trim` ships that rule into the initramfs,
   and the hook sits in HOOKS ahead of `block`. **This is the non-obvious
   part.** A dm table caches the stacked queue limits computed when it is
   loaded. If `provisioning_mode` is still `full` when sd-encrypt and lvm2
   build `cryptusb` and the LVs in early userspace, every layer above the
   bridge caches `discard_max_bytes=0` and stays that way after switch-root
   even once udev fixes the bridge. Symptom: `lsblk -D` shows a nonzero
   DISC-MAX on sda and cryptusb but `0B` on the LVs. Recovering without a
   reboot needs a genuine table reload (`dmsetup suspend` / `reload` /
   `resume`); `lvchange --refresh` does not recalculate limits and will
   mislead you. **Unchanged, and still needed: p4 keeps LUKS + LVM.**

   **NEW, and it is a non-problem worth naming so it is not chased.** There is
   a second dm-crypt device now, `cryptefi` for p3 at `/efi`. It is created in
   the real root, long after udev has fixed the bridge, so it inherits correct
   limits with no help from the initramfs. Its `discard` comes from the
   `discard` option in `/etc/crypttab`.

3. `rd.luks.options=<p4-luks-uuid>=discard` on the kernel cmdline. **CHANGED:
   only in `/etc/kernel/cmdline` now.** It used to have to be in the BIOS
   menuentry in `ventoy_grub.cfg` as well, kept in sync by hand, for the same
   reason `fsck.repair` was in both. There is one cmdline now and that whole
   class of drift is gone. Without the option dm-crypt swallows every discard.

4. `shutdown-maintenance` trims **every** partition on the stick, not just the
   mounted ones. Nothing else ever will; no other OS understands this layout.
   **CHANGED: the two partitions it addresses by PARTUUID are different
   partitions.** It named the Ventoy exFAT partition and VTOYEFI; it must now
   name **p1 (`FALCON-ESP`, vfat)** and **p2 (`FALCON-ISO`, exFAT)**. Read the
   new values and edit the two constants at the top of
   `/usr/local/sbin/shutdown-maintenance`:

   ```bash
   blkid -s PARTUUID -o value ${DISK}-part1    # -> the vfat one
   blkid -s PARTUUID -o value ${DISK}-part2    # -> the exFAT one
   ```

   Everything else about it stands: PARTUUID rather than label because the
   install roams and a label match would be ambiguous next to another stick;
   both mounted **read-only**, because exFAT and vfat implement FITRIM and
   discards reach the block layer regardless, since `-o ro` gates filesystem
   writes rather than block I/O, so an unclean power-off during shutdown can
   never damage them. Cost is negligible - 87.5 GiB of exFAT took 67 commands
   and about 90 ms, because UNMAP here is a metadata operation. exFAT has no
   trimmed-group tracking the way ext4 does, so it re-issues the full extent
   every shutdown rather than a delta.

   **NEW: `/efi` is now a third mounted filesystem that needs trimming**, and
   it is small and low-churn, so it costs nothing to include it in the mounted
   set alongside `/` and `/home`.

`fstrim.timer` is deliberately **disabled**. Trim runs once per clean shutdown
from `shutdown-maintenance.service`, a better cadence for a stick that spends
most of its life powered off, and one that avoids a weekly job firing at an
arbitrary moment. Do not add `discard` to fstab: this bridge accepts only one
range per command (`Maximum unmap block descriptor count: 1`), so inline
discard would inject a synchronous USB round-trip per freed extent into the
write path.

### Unallocated space

**CHANGED, see Section 1.** The old stick's sectors 34..2047 held Ventoy's
legacy-BIOS core image and had to be protected from any discard. There is no
BIOS boot image now and the gap is genuinely empty.

The rule that came out of it still stands and should not be relaxed:
`shutdown-maintenance` trims **named partitions by PARTUUID** and never
enumerates partition-table gaps. "Discard everything unallocated" was wrong
once and will be wrong again the next time something lives in a gap.

### The tradeoff that was accepted

`allow_discards` on LUKS lets anyone who images the stick distinguish trimmed
regions from written ones, i.e. see roughly how much data is on it and where
it sits. It does not expose plaintext or weaken the cipher. Weighed against
the FTL problem and accepted deliberately on 2026-09-07. **It now applies to
two volumes rather than one** (p4 via `rd.luks.options`, p3 via crypttab), and
p3 is a 1 GiB shelf whose contents are not secret in any interesting way, so
the marginal exposure is nil.

### Why this was worth doing

Instantaneous usage is low, which makes it tempting to argue the never-written
blocks already provide plenty of over-provisioning. That misreads the problem.
`Lifetime_Writes_GiB` was already 459 on a 238 GiB drive. What the FTL tracks
is the set of LBAs ever written, not current usage, and journald, pacman and
systemd churn only grow it. Once that set approaches the partition size the
drive is effectively full from the controller's point of view, and every write
becomes read-modify-write garbage collection. This is a sustained-throughput
fix, not a wear fix; wear is nowhere near the limit (SSD_Life_Left 99).

---

## Filesystem checks at shutdown

`shutdown-maintenance` runs a **read-only** `fsck` on the two firmware-visible
partitions immediately before trimming them. **CHANGED: those are now p1
(vfat) and p2 (exFAT)** rather than VTOYEFI and the Ventoy partition; the
argument is identical. They are the only filesystems on the stick that nothing
else ever checks: neither is in fstab, so no boot-time `systemd-fsck` unit
covers them, and neither has a journal. On a device that loses power uncleanly
about half the time, that combination makes them the likeliest thing to rot
quietly.

**NEW and it matters more than it did: p1 is now the ESP**, holding shim,
MokManager and the loader. A corrupted p1 is a stick that does not boot at
all, where a corrupted VTOYEFI was a stick that did not show a menu. The check
is cheap and the stakes went up.

It **never repairs**. Shutdown is the worst possible moment to attempt one:
there is time pressure and a real chance of losing power part-way through
rewriting a FAT or a bitmap, which turns a small inconsistency into a lost
filesystem. Fix by hand from a running system with `fsck.exfat -r` or
`fsck.fat -a`.

The check runs before the trim mount, because a check is only valid on an
unmounted filesystem; if the partition happens to be mounted the function says
so and skips rather than reporting a result it cannot trust. Exit codes were
verified against deliberately corrupted loopback images rather than assumed:
`fsck.fat -n` returns 1 and `fsck.exfat -n` returns 8 on a damaged
filesystem, both nonzero, so a real problem cannot pass silently.

The mounted filesystems are deliberately **not** checked here. The script runs
`Before=umount.target` so they are still mounted read-write, and `e2fsck -n`
against a live read-write filesystem reports false positives as the structure
shifts underneath it. They are covered at boot by `systemd-fsck` - **CHANGED:
passno 1 for `/`, 2 for `/home` and 2 for `/efi`** - which is the pass that
matters after an unclean power-off.

Note there is no such thing as a data scrub here. ext4 carries
`metadata_csum`, so metadata is checksummed and `e2fsck` can verify it, but
ext4 has no data checksums at all: nothing can tell you a file's contents
rotted. A periodic forced `e2fsck` via `tune2fs -c` was considered and
rejected, because an unexpected minute-long check at boot is exactly the wrong
surprise on a machine that is not yours.

SMART self-tests are supported through the SAT bridge (short is 2 minutes).
They exercise the drive's own media and electronics, which no filesystem check
reaches. They cannot go in the shutdown path, since powering off aborts a
running test. Run one by hand occasionally:

    smartctl -d sat -t short /dev/sda     # wait 2 minutes
    smartctl -d sat -l selftest /dev/sda

A full-device read pass surfaces unreadable sectors and takes about 10.5
minutes at the observed 384 MB/s (`dd if=/dev/sda of=/dev/null bs=1M
iflag=direct`). Worth doing before trusting the stick with anything important,
not on a schedule.

---

## Drive health: use the deltas, not the lifetime counters

Unchanged, with one instruction that now applies.

`Power_Cycle_Count` and `Power-Off_Retract_Count` are **lifetime** counters,
and this drive spent its first life as an ordinary Ventoy stick being pulled
out of machines. The raw ratio (207 of 434 on 2026-09-07) describes that era
and says nothing about this install.

`/var/lib/archusb/smart-baseline` holds the counters as they stood at the end
of the build. `archusb-status-collect` subtracts them and emits
`smart_cycles_since` and `smart_unclean_since`. **The old runbook said "if the
stick is ever rebuilt, rewrite that file". This is that rebuild.** See R6.

Two things worth knowing about these counters:

- A **warm reboot does not increment `Power_Cycle_Count`** - USB bus power is
  not cut. Verified across a reboot on 2026-09-07: 434 before and after. So
  these count real power-downs, which is exactly the population
  `Power-Off_Retract_Count` measures against.
- `lastboot` is the metric that actually answers "was the last shutdown
  clean", and it is derived from the journal, not from SMART. It reports
  `unknown` rather than `UNCLEAN` when the previous boot's journal has been
  vacuumed away; journald is capped at 64M and rotates, and a missing journal
  is not evidence of a yank.

### SMART self-tests on a timer

`archusb-smart-test.timer` runs a short self-test weekly, `Persistent=true` so
a stick that is powered off for a month catches up on the next boot, with a 30
minute randomised delay. The delay is kept modest on purpose: too much spread
on a machine that is only up for an hour at a time means the test would never
fire at all.

`/usr/local/sbin/archusb-smart-test` finds the disk by PARTUUID rather than
assuming `/dev/sda`, refuses to start if a test is already running, waits for
completion, logs the outcome to the journal under its own tag, and refreshes
the status cache. Every failure path exits 0; a missed self-test is not worth
failing a unit over.

**CHANGED: the PARTUUID it anchors on.** It used one of the Ventoy partitions;
give it one of the new ones. p1 is the natural anchor now - it is the one
partition that is guaranteed to exist on any falcon stick.

It cannot live in `shutdown-maintenance`: powering off aborts a running test.

### manage_shutdown: the drive was never told to prepare for power-off

Unchanged and still deployed via the udev rule.

`Power-Off_Retract_Count` was rising on shutdowns the OS considered perfectly
clean - 11 of 29 real power-downs since the install was built. The cause is
that the sd driver had `manage_shutdown=0`, so it never sent START STOP UNIT
at shutdown. A SAT bridge translates that to STANDBY IMMEDIATE, which is what
makes the drive commit its FTL mapping table. Without it the host simply drops
USB VBUS and the drive finds out afterwards.

The rule sets `manage_shutdown=1`. **`manage_system_start_stop` is
deliberately left at 0**: it also governs suspend, so it would put the root
filesystem at risk on resume in exchange for nothing extra at shutdown.

**Do not read the 2026-09-07 test as proof the fix works.** Only 38% of
power-downs were incrementing the counter, so there was a 62% chance of seeing
no increase even if the setting did nothing. What the two tests *do* establish
is safety: the stick boots afterwards on both the cold and warm paths.

One caveat on the warm-reboot test: dmesg shows the bridge was re-enumerated
on the way back up regardless, because a warm reboot resets the xHCI
controller and therefore the bus. So the scenario of a bridge stuck with no
reset at all may not be reachable here.

### Tracking, and why the sidebar shows no counter

`archusb-status-collect` appends one line per boot to
`/var/lib/archusb/retract-log`:

    <boot_id> <unix_time> <Power_Cycle_Count> <Power-Off_Retract_Count>

A running total cannot say *which* shutdown the drive objected to; one line
per boot can. Derived keys: `retract_delta`, `retract_tracked`,
`cycles_tracked`.

**The conky sidebar deliberately shows no cumulative unclean counter.** History
cannot be acted on, the pre-fix events can never be cleared, and a number that
only ever climbs is noise on a permanently visible panel. The sidebar stays
silent unless `retract_delta` is nonzero, which means something really did cut
power since the last boot - worth seeing, and it clears itself on the next
clean boot.

---

## Signing hook: trigger on paths, not package names

Unchanged, and re-verified while adapting: `95-sign-uki.hook`'s triggers are
byte-for-byte `90-mkinitcpio-install.hook`'s, checked against the copy in the
backup.

An earlier version keyed on package names (linux, systemd, ucode) and silently
missed rebuilds caused by path triggers - installing `wireless-regdb` writes
`usr/lib/firmware/*`, which regenerates the UKI. Result was an unsigned UKI
that booted fine with Secure Boot off and would have failed where it is on.

**CHANGED: the verification command.**

```
sbverify --cert /efi/falcon-mok.crt /efi/uki/arch-usb.efi
```

---

## Status widget

**Carried over, with a list of keys that change.** A desktop widget reports
the things that otherwise fail silently - most importantly whether the UKI is
actually signed, which used to be discoverable only by reading boot messages
that scroll past.

    /usr/local/sbin/archusb-status-collect   root; writes /run/archusb-status
    /etc/systemd/system/archusb-status.{service,timer}   boot + every 15 min
    /usr/local/bin/archusb-status-conky      renders it as conky markup
    ~/.config/conky/archusb.conkyrc          the widget itself
    ~/.config/autostart/archusb-conky.desktop
    /usr/local/bin/archusb-status-report     yad detail window
    /usr/share/applications/archusb-status.desktop   menu entry for the above

Split in two because sbverify, smartctl and tune2fs all need root and conky
does not run as root. The collector writes a world-readable key=value file to
`/run`, so nothing touches the disk and the widget needs no privileges.

**What to change in `archusb-status-collect`:**

| Line | Was | Becomes |
|---|---|---|
| `UKI=` | `/boot/EFI/Linux/arch-usb.efi` | `/efi/uki/arch-usb.efi` |
| signing cert | `/var/lib/sbctl/keys/db/db.pem` | `/efi/falcon-mok.crt` |
| disk fallback | `lsblk -no PKNAME $(findmnt -no SOURCE /boot)` | see below |
| `ventoy_*` keys | mode, version, drift | **delete** |

**The disk-fallback line is a real break, not a rename.** It resolved the
stick by taking `PKNAME` of whatever backed `/boot`. `/boot` was a raw
partition, so `PKNAME` gave the disk. `/efi` is backed by
`/dev/mapper/cryptefi`, whose `PKNAME` is the *partition* (`sda3`), not the
disk. Either walk up twice or, better, resolve from p1's PARTUUID the way
`archusb-smart-test` already does.

**NEW key worth adding, given the trap in Section 4:** whether `/efi` is
actually a mount point. `findmnt -no SOURCE /efi` returning
`/dev/mapper/cryptefi` is the single check that catches "the kernel update
went to the root LV and nobody noticed", and it is exactly the kind of silent
failure this widget exists for. Red when it is not mounted.

The widget otherwise shows: UKI signature, firmware and Secure Boot state,
host machine, USB link speed, pending updates, filesystem state with free
space and a usage bar, whether the last boot ended cleanly, SMART
life/writes/unclean-power-off counters, and backup age. Green when fine,
orange-red when not; backup older than 14 days warns.

USB link speed is worth having: a USB 2 port negotiates 480 Mb/s and makes the
whole system feel broken with no other indication. Found by walking the sysfs
device chain up from the disk backing `/`, not by matching a product string.

Pending updates come from checkupdates, refreshed at most hourly because it
hits the mirrors, and cached in `/var/lib/archusb/updates-count`. Exit code 2
means no updates and 1 means failure - conflating them gives a false "up to
date" when offline.

Filesystem bars are conky's native `${fs_bar}`, which fills with USED space.
Thresholds: teal below 75%, amber 75-90%, red above.

`own_window_type = 'desktop'` keeps it behind windows and out of the taskbar
and alt-tab. It is not clickable - the detail window is in the applications
menu as "Arch USB Status".

The backup-age field is fed by `/var/lib/archusb/last-backup`. **That file was
deleted in R6**, so until the first backup on the new stick it will correctly
report "never recorded".

---

## Reboot chooser: what survives, and what cannot

**Read this before deleting `archusb-boot-next` in R6.**

### What `bootnext-install.sh` did

It installed the privileged helper `archusb-boot-next`, the zenity front end
`archusb-reboot`, a reset service, a `.desktop` launcher and a sudoers
drop-in - the drop-in at 0440 and checked with `visudo -c` first, and removed
if the check failed, because a sudoers file with the wrong mode or a syntax
error locks sudo out entirely. It enabled the reset service. Then it mounted
partition 1 of whatever disk backed `/boot` and did two pieces of surgery on
`ventoy_grub.cfg`: tagged the first UEFI menuentry that chainloaded
`arch-usb.efi` with `--id archusb` (by awk, keeping a `.bak`, matching on the
chainload rather than the title so a retitled entry still got tagged), and
idempotently inserted a top-level `terminal_output console`. It finished by
printing manual follow-ups: refresh the stick backup, and wire the Whisker
menu Restart button with `xfconf-query` as the desktop user.

Its own closing note already said it was half-broken: the sibling scripts it
installs are no longer on the stick, so re-running it fails at the first
`install`.

### What of it still applies

Split by mechanism, because the old chooser used three:

| Choice | Old mechanism | Now |
|---|---|---|
| Back to Arch | `VTOY_DEFAULT_IMAGE="F6>archusb"` + timeout, **and** `BootNext` | **Gone.** There is no menu-preselect file falcon reads |
| An ISO | `VTOY_DEFAULT_IMAGE="/ISO/foo.iso"`, **and** `BootNext` | **Gone**, same reason |
| Firmware entry | `efibootmgr -n` | **Survives unchanged** |
| Firmware setup | `systemctl reboot --firmware-setup` | **Survives unchanged** |
| Stock reboot | everything cleared, `BootOrder` decides | **Survives unchanged** |

The two Ventoy rows die because there is nothing to write. falcon's loader
reads no configuration file off p2 that names a default entry, and every
filesystem driver in EFI-land is read-only by design, so a marker on the stick
could be set from Linux but never cleared by the loader.

**`FalconNextBoot` is NOT a substitute, and must not be repurposed.** It is a
non-volatile EFI variable under the loader's own vendor GUID whose *whole
lifecycle belongs to the loader* (DESIGN Section 6.3.1,
`src/efi/loadops.h`): the loader sets it immediately before launching the
signing appliance to prepare Windows media, and on the next boot reads it,
deletes it unconditionally, and only then validates the name against the
manifest before acting. Writing it from Linux would hand the loader an entry
name that never came from a manifest-validated preparation step, and would
break the read-then-delete ordering that is the entire boot-loop defence. It
is also for a different job: "the appliance just prepared this, go finish it",
not "boot this ISO next time".

**What survives is worth keeping on its own**, and it is the half that was
always necessary anyway: on the daily driver, `BootOrder` puts the internal
Gentoo entry ahead of the stick, so coming back to the stick at all needs
`BootNext`. Two of the old tool's findings carry straight over:

- **Identify the entry by partition, not by `BootCurrent`.** `stick_boot_entry()`
  matched the stick's own partition GUIDs against the device paths in
  `efibootmgr -v`, after `BootCurrent` was measured on the Latitude returning
  `0000` - `Linux Firmware Updater` on the **internal** disk - while actually
  running from the stick. The old code would have pointed the next boot at the
  host's `fwupdx64.efi`. **CHANGED, and simplified: there is only one stick
  entry now** (p1's `\EFI\BOOT\BOOTX64.EFI`), where the Ventoy stick had two
  and had to choose between them. The ambiguity that caused the bug is gone;
  the technique that fixed it is still the right one.
- **`efibootmgr` 18 cannot delete `BootNext`.** `-N` and `--delete-bootnext`
  both exit 17 with `Boot entry 0001 does not exist` and leave the variable in
  place, on a machine that has no `Boot0001` at all. `efibootmgr -N || true`
  therefore cleared nothing, quietly. Verify afterwards and fall back to
  removing
  `/sys/firmware/efi/efivars/BootNext-8be4df61-93ca-11d2-aa0d-00e098032b8c`
  directly, clearing the immutable bit first.

It also **deliberately never created** a boot entry with `efibootmgr -c`:
writing a boot entry into someone else's NVRAM is the same class of trace as
writing their RTC, and this stick does not leave those. Keep that rule.

**JUDGEMENT CALL.** Whether to rebuild a three-item chooser (stick / firmware
setup / plain reboot) around the surviving mechanism is yours. This document
does not ship one, because a two-thirds-amputated tool is a design question
rather than an adaptation, and because "reboot and pick from falcon's menu" is
now a real answer where under Ventoy it was not - the ISO list is in the boot
menu itself.

### Two findings that die with the tool, recorded so they are not re-learned

- **The chooser died while the desktop held partition 1.** `mount_data` always
  mounted partition 1 on a private `mktemp -d`; udisks mounts it **read-write**
  the moment anyone clicks it in Thunar's Places sidebar, and the kernel's
  exfat driver refuses a second mount of the same device at a different RO
  state. So the chooser worked or failed purely on whether a file manager
  happened to be holding the data partition. The fix was to adopt an existing
  mount (`findmnt --source`, `MNT_ADOPTED=1` so cleanup syncs and lets go
  rather than unmounting out from under the file manager). **Nothing on falcon
  needs to mount p2 from Linux at boot-target time, so the whole class goes
  away** - but the lesson generalises to anything that ever wants to touch p2
  programmatically.
- **Rejected: GPT attribute bit 63 (`no_automount`) on p2.** It would stop
  udisks offering the partition, but it is a partition-table write on the boot
  stick to work around a fixable userspace assumption, and p2 still has to be
  mountable by hand to drop ISOs on it. **Still rejected, and now more
  strongly: p2's whole purpose is being drag-and-drop visible**, from Windows
  as well (DESIGN Section 3).

---

## Clock: never write the host's RTC, and still know what time it is

**Carried over untouched. Nothing here is about the bootloader.** A stick that
boots on arbitrary machines has a problem no fixed install has: the CMOS RTC
it finds is written in a convention it cannot determine. Linux hosts keep it
in UTC, anything dual-booting Windows keeps it in local time, and the register
contents look identical either way.

### The bug that was found

The stick was actively corrupting the clock of every machine it touched.

`systemd-timesyncd` clears `STA_UNSYNC` once it synchronises, deliberately, to
hand RTC upkeep to the kernel. That flag is the entire gate on
`sync_hw_clock()` in `kernel/time/ntp.c`, and this kernel is built
`CONFIG_RTC_SYSTOHC=y`. With the flag clear the kernel copies system time into
CMOS every 11 minutes, forever. So: boot on a Windows laptop in Chicago, come
up five hours behind, let timesyncd correct us, and eleven minutes later the
kernel has written real UTC into a register Windows reads as local time. That
host is now five hours into the future, and stays that way.

Read the flag with:

```bash
archusb-rtc-probe --check
```

### Never writing it

`STA_UNSYNC` is the right lever because it gates *both* kernel write paths -
the rtc-class one and x86's direct `mach_set_cmos_time`. Blacklisting
`rtc_cmos` does not, because the x86 CMOS path does not go through the driver.

So `chrony` replaces `timesyncd`. chronyd leaves `STA_UNSYNC` set unless told
otherwise, while still disciplining the system clock normally.

- `/etc/chrony.conf` - **Arch ships this file with `rtcsync` uncommented.** It
  was replaced wholesale, and the load-bearing part is the absence of
  `rtcsync`, `rtcfile`, `rtcautotrim` and `rtconutc`. A pacnew merge that
  restores any of them re-arms the bug. The original is kept at
  `/etc/chrony.conf.pacnew-orig`.
- `systemd-timesyncd` is **masked**, not merely disabled. It offers no way to
  opt out of clearing the flag.
- `/etc/adjtime` stays `UTC`. Not because we believe it, but because
  `timedatectl set-timezone` writes the RTC when and only when the RTC is in
  local mode. Leaving it UTC removes an accidental write path.
- Never run `hwclock --systohc`, `timedatectl set-time`, or
  `timedatectl set-local-rtc` on this install. All three write CMOS directly.

### Still knowing the time

With the RTC demoted to untrusted input, absolute time before the network
exists comes from `/var/lib/archusb/timestamp`, on our own encrypted root.

    /usr/local/sbin/archusb-timekeep  {restore|save}
    /etc/systemd/system/archusb-timekeep.service        restore at boot, save at clean shutdown
    /etc/systemd/system/archusb-timekeep-save.{service,timer}   every 10 min, for yanks

`restore` runs `Before=sysinit.target` and steps the clock **forward only**. A
backward step at boot could undo a clock that was already correct; chronyd is
what corrects downward, later, once it has a source.

`save` will not record a time it has no reason to trust. With chronyd
synchronised it records the wall clock. Without, it advances the floor by
elapsed `CLOCK_BOOTTIME` from the value restored at boot, never by the wall
clock - on a Windows host that wall clock is hours out, and persisting the
overshoot would pin every future boot to a time in the future. With no anchor
at all it writes nothing.

The RTC is still used for one thing, and it is sound: the *elapsed* delta
across suspend, which the kernel computes from it and which is correct under
either convention. Only the absolute reading is ambiguous.

`chronyd -s` is left on as a second belt (drop-in at
`/etc/systemd/system/chronyd.service.d/archusb.conf`). With no `rtcfile`
directive it steps the clock forward to the drift file's mtime, which covers
the case where our own state file is lost but chrony's is not.

### Learning each machine's convention

The convention is a property of the machine, not the moment, so it only has to
be observed once. With a synchronised clock, the RTC read as if it were UTC
differs from true UTC by exactly zero on a UTC host, or by that machine's
local offset on a Windows host.

    /usr/local/sbin/archusb-rtc-probe
    /etc/systemd/system/archusb-rtc-probe.{service,timer}   boot + hourly
    /var/lib/archusb/rtc-hosts/<dmi-hash>

Hourly rather than once, because a boot can go online late, and a local-time
host crosses a DST boundary twice a year. The payoff is a **later offline boot
on the same machine**: `restore` asks `archusb-rtc-probe --offset`, corrects
the RTC reading, and comes up with a right wall clock immediately instead of
waiting for a network that may never appear.

An offset that is not within 90 s of a clean quarter-hour is a host whose RTC
is simply wrong - a dead coin cell, or an OS that never set it. That is
recorded as `verdict=drifting`, `usable=no`, and never used to move our clock.
Correcting from it would be worse than having no entry.

DST is a known limit: a stored offset for a local-time host goes stale by an
hour twice a year, until the next online boot refreshes it. An hour out beats
five hours out, and it self-heals.

### Two consequences elsewhere

- `/etc/e2fsck.conf` sets `broken_system_clock = 1`. `restore` cannot get
  ahead of the `/home` fsck, so a host RTC reading hours in the future will
  occasionally stamp a future "last write time" into a superblock. Without
  this, e2fsck reports that as corruption on the next boot. **NEW: this now
  covers `/efi` too**, which is a third ext4 filesystem with the same
  exposure.
- The status widget carries the state, because this is exactly the kind of
  thing that regresses silently. `archusb-rtc-probe --kv` feeds `rtc_write`,
  `rtc_host`, `rtc_delta` and `clock_sync` into `/run/archusb-status`; the
  conky line goes red on `RTC BEING WRITTEN`.

### Tests

    sudo /usr/local/share/archusb/test-timekeep.sh

24 cases, covering the situations this machine cannot produce - a Windows
host, a host east of Greenwich, a half-hour zone, a dead coin cell, and a save
taken while the clock is untrusted - by pointing `ARCHUSB_RTC_DIR` at a
synthetic sysfs directory and putting a stub `chronyc` on `PATH`. Nothing in
the harness touches the real clock, the real RTC, or `/var/lib/archusb`.
`ARCHUSB_DRY_RUN=1 archusb-timekeep restore` reports what it would do without
doing it, which is also worth running by hand on a machine the stick has not
seen.

**The harness is bootloader-agnostic and needs no adaptation.** Re-run it
after any pacman upgrade that touches chrony, and check
`archusb-rtc-probe --check` after any that touches systemd. Run it once after
the restore, as part of R10.

### Timezone

Pinned to `America/Chicago` and left there. The stick travels; the operator's
local time does not. No geoclue, no tzupdate.

---

## Reliability: writeback, journal, unattended fsck

Carried over. Aimed at the failure mode SMART actually shows: about half of
this drive's power events were unclean. Wear is not the constraint, so the
build-era tuning - which minimised writes at the cost of a long loss window -
was optimising for the wrong thing.

### Writeback: `/etc/sysctl.d/50-usb-writeback.conf`

    dirty_expire_centisecs      12000 -> 1500     loss window 120s -> 15s
    dirty_writeback_centisecs    6000 -> 1500     flusher actually runs
    dirty_ratio / background     20%/10% -> dirty_bytes 256M / 64M

Byte limits rather than percentages because 20% of RAM is 6.2 GB on the 31 GB
Latitude and 1.6 GB on an 8 GB machine - the percentage form behaves very
differently depending on which laptop the stick is in.

### Journal: `/etc/fstab`

`commit=60` -> `commit=5` on `/` and `/home`, **and now on `/efi`**. Confirm
with `/proc/fs/ext4/<dm-N>/options`; `/proc/mounts` hides `commit=5` because
it is the ext4 default.

### Unattended fsck: kernel cmdline

`fsck.repair=yes` in `/etc/kernel/cmdline`. Without it an unfixable error
drops to an emergency shell - unhelpful on a machine that is not yours. The
trade is that automatic repair may discard data to get mounted.

**CHANGED: one place, not two.** It used to have to be duplicated into the
BIOS menuentry in `ventoy_grub.cfg`, and the two had to be kept in sync by
hand.

### Signing: a manual mkinitcpio no longer leaves an unsigned UKI

Adding `fsck.repair` meant `mkinitcpio -P` by hand, which produced an UNSIGNED
UKI: `95-sign-uki.hook` is a *pacman* hook and does not fire on a manual run.
Fixed with an mkinitcpio post hook, `/usr/lib/initcpio/post/zz-sign-uki`,
which calls `/usr/local/bin/sign-uki`. Verified by stripping the signature
with `sbattach --remove`, re-running mkinitcpio, and confirming the hook
re-signed it.

Note that hook lives under `/usr/lib`, so a mkinitcpio upgrade can remove it.
Check it is still there after any transaction that touches mkinitcpio.

### Backup

`/root/ventoy-arch/backup-archusb.sh` on the Gentoo box: one command, offline,
finds the stick by serial and refuses if anything is mounted. Captured GPT,
LUKS header, VTOYEFI image, ARCHBOOT, Ventoy config, rootfs and home, then
checksummed. The passphrase is typed at the cryptsetup prompt and never
touches the script.

**CHANGED, and it needs rewriting for the new layout**, which this document
does not do for you. What it should now capture:

    GPT                      sgdisk --backup
    p3 LUKS header           cryptsetup luksHeaderBackup  (NEW, and it holds the MOK key slot)
    p4 LUKS header           cryptsetup luksHeaderBackup
    p1 image                 32-64 MiB dd, or just re-run mkstick to rebuild it
    p2 /blessed              the manifest and blessed artifacts, if any
    p3 contents              /uki, falcon-mok.key, falcon-mok.crt   -- SECRET, encrypt it
    rootfs and home          rsync

**p3 is new in that list and it is the important one.** It holds the only copy
of the stick's MOK private key. A p3 backup is a secret and must not land
anywhere in the clear.

Network push was designed and abandoned: sshd is inactive on the Gentoo box
with `PermitRootLogin=no`, so a push would require a permanently running sshd
plus a key on the stick granting write access to the workstation. It also
could not capture the GPT, LUKS headers or UKI, which need the stick offline -
so it would back up the least important half at the highest cost.

---

## Reliability / hardening pass

Carried over unchanged. Done in a libvirt VM with the real stick passed
through, then verified across a cold boot.

**CHANGED, one line of the VM recipe:** the domain was built with OVMF and
**Secure Boot off**, because `--boot uefi` alone selects a Secure-Boot
firmware and Ventoy's MOK-signed grub failed with "Verification failed:
(0x1A) Security Violation". On falcon the same VM under Secure Boot on needs
the stick's own MOK enrolled in the varstore instead; the repo's
`tools/run-qemu.sh --secboot` already builds that.

Guest networking needed three rules on the **Gentoo host**, not the guest: ufw
default-deny was swallowing DHCP on virbr0, and libvirt's own accept rules sit
behind ufw's chain.

    ufw allow in on virbr0 to any port 67 proto udp
    ufw allow in on virbr0 to any port 53
    ufw route allow in on virbr0 out on enp11s0

### What was applied

    /etc/modprobe.d/99-hardening-blacklist.conf   sctp rds tipc cramfs jffs2 hfs hfsplus
    /etc/sysctl.d/60-hardening.conf               11 settings (see file)
    /etc/udev/rules.d/60-usb-bfq.rules            BFQ on USB disks
    /etc/systemd/system/user-.slice.d/            TasksMax=33%, IOWeight=500
    /etc/systemd/system/system.slice.d/           IOWeight=100
    /etc/systemd/system/user@.service.d/          delegate the io controller
    /etc/systemd/user/aur-build.slice             build limits, mirrors portage.slice
    /etc/systemd/user/background.slice            generic low-priority slice
    /usr/local/bin/yay                            wrapper: slice + nice 19 + sleep inhibit

    disabled: cups.{service,socket,path}, cups-lpd.socket, avahi-daemon{,.socket}

A normal boot has **zero listening sockets** (sshd does not autostart).

### Traps found

**ufw overrides `/etc/sysctl.d`.** ufw applies `/etc/ufw/sysctl.conf` *after*
systemd-sysctl, so it wins for rp_filter, log_martians, accept_redirects,
accept_source_route and the ICMP keys. It shipped `rp_filter=1` and
`log_martians=0`. The live effect of rp_filter was loose anyway - the kernel
uses max(all, interface) and Arch sets interfaces to 2 - but `default=1` is
inherited by interfaces appearing *after* boot, i.e. USB ethernet and VPN
tunnels in the field. Corrected in `/etc/ufw/sysctl.conf` (watch for .pacnew).

**IOWeight in user slices is inert by default.** Only cpu/memory/pids are
delegated to the user manager; `io.weight` does not exist until the io
controller is delegated, and the running user manager only picks it up at next
login.

**Do not sandbox udisks2.** Every useful directive (ProtectHome,
ProtectControlGroups, ProtectKernelLogs) puts it in a private mount namespace,
and mounts it creates then never reach the host namespace: `udisksctl` reports
success and the file manager sees nothing. `MountFlags=shared` did not fix it.
Left at 9.6 UNSAFE deliberately. bluetooth, NetworkManager and accounts-daemon
are already sandboxed to MEDIUM upstream and were left alone.

---

## Appearance

**CHANGED: the first row is gone.** The Ventoy menu, LightDM greeter and
desktop were deliberately consistent on `#008080`. falcon's menu is firmware
text mode and has no theme to match, so there are two rows now:

    LightDM greeter   /etc/lightdm/lightdm-gtk-greeter.conf   background=#008080
    GTK / xfwm4       Materia-dark

Materia rather than Adwaita-dark because no stock dark theme ships xfwm4
window decorations - Adwaita-dark alone gives dark windows with light title
bars. The greeter background must be a path readable by the unprivileged
`lightdm` user (or a color); an image under `/home` silently falls back to
black.

**DELETED with Ventoy:** the whole theme-cloning procedure (clone the stock
theme to the exFAT partition, drop `background.png`, substitute
`desktop-color`, and the finding that `gfxmode: max` upscales a 1024x768
background on a 1080p panel). It only existed because Ventoy's GUI mode always
loads a theme and there is no "no theme" fallback. Also deleted: the F6-menu
background experiments - `color_normal=white/cyan` painting only the cells
GRUB redraws, gfxterm booting before the timeout elapsed, themed gfxmenu
swallowing it entirely.

---

## Window tiling and keyboard

Carried over untouched. Nothing here is about the stick's boot path.

    /usr/local/bin/xfwm-tile          KDE-style half/quadrant tiling
    ~/.config/autostart/xcape.desktop tap Super for the menu

Meta+arrows are bound as **commands** running `xfwm-tile`, NOT as xfwm4
actions. That is deliberate: **xfwm4 honours only one key binding per
action**, and the tile actions were already on the numpad keys, so a second
binding on the arrow keys was silently ignored and the keystroke fell through
to the application - which is why pressing Meta+Right in a terminal printed a
stray "C" (the ANSI sequence for cursor-right).

Model, matching KDE on the Gentoo workstation. Two independent axes,
horizontal (L/R/full) and vertical (T/B/full); each press changes one axis:

    right half  + Left  -> left half        (a half moves across)
    right half  + Up    -> top-right        (a half narrows to a quadrant)
    top-right   + Left  -> top half         (a quadrant expands away from its side)
    top half    + Left  -> top-left         (a full extent narrows)

Three things that made this hard, all worth not rediscovering:

**Do not synthesise keystrokes.** An early version triggered xfwm4's own tile
shortcuts with `xdotool key super+KP_Left`. It passed every automated test and
failed in real use, because a person is still holding Super when the script
runs and `--clearmodifiers` fights the physically-held key. Geometry is now
applied directly with wmctrl.

**Compensate for GTK client-side decorations.** ghostty and Chrome carry an
invisible shadow margin inside their X window (`_GTK_FRAME_EXTENTS`; ghostty
reports 25 on every side when tiled, 0 when maximised). Without adding it
back, a 960x521 tile shows a 910x471 window and every tile looks like it has a
large gap. Both placement and zone detection use the VISIBLE rectangle.

**Binding Super_L directly grabs the bare modifier** and kills every Super+key
combination. xcape is used instead: tapping Super alone sends Ctrl+Escape
(bound to the Whisker menu), while holding it for combinations passes through.

A fullscreen or maximised window is released before it is moved, and its frame
extents are then measured AGAIN: while a window is still fullscreen it reports
no decorations, so placing it from those numbers overshoots by the height of
the title bar. Two further faults were fixed at the same time:

    xprop prints "PROP:  not found." for an absent property. Fed to $(( )),
    bash treats that word as a variable name and set -u aborts the script -
    which broke tiling for every app without CSD (xfce4-terminal, mousepad).

    Removing the maximised states does not clear _NET_WM_STATE_FULLSCREEN, and
    a fullscreen window silently ignores the move request.

Other bindings, all in the `/commands/custom/` namespace for the same
one-binding-per-action reason:

    <Super>Page_Up     wmctrl -r :ACTIVE: -b toggle,maximized_vert,maximized_horz
    <Super>Page_Down   xdotool getactivewindow windowminimize
    <Super><Shift>s    /usr/local/bin/screenshot-region
    <Primary>Escape    xfce4-popup-whiskermenu

`screenshot-region` reproduces Spectacle's Meta+Shift+S: drag a region, the
PNG lands in `~/Pictures/Screenshots/` AND on the clipboard. It is a wrapper
because `xfce4-screenshooter`'s `-c` (clipboard) and `-s` (save) are mutually
exclusive - the last one given wins - so it saves to the file first and loads
the clipboard from it with xclip. Requires the `xclip` package. The stock
Print / Shift+Print / Alt+Print bindings are untouched.

Taskbar: `window-scrolling` and `wrap-windows` are on, so the scroll wheel
over the tasklist cycles windows. XFCE has no click-to-cycle and no hover
thumbnails; the tasklist plugin simply has no such properties. Alt+Tab
previews are on (`cycle_preview`). xfce4-docklike-plugin was trialled for
click-to-cycle and removed - icon-only, not wanted.

Plasma was sized as an alternative and rejected: minimal `plasma-desktop` is
821 MB / 167 packages against 73 MB / 53 packages for the whole XFCE stack.
Disk was not the issue - faulting in 850 MB of Qt6/KF6 over a USB bus at every
login was.

### Whisker menu Restart button

**CHANGED: only if you rebuild a chooser.** The old wiring repointed Whisker's
**Restart** action button at `archusb-reboot`, because Whisker 2.10.1 has only
a fixed set of named command buttons and no arbitrary custom slots. If you do
not rebuild a chooser (see "Reboot chooser" above), leave the Restart button
alone and this whole subsection is moot. If you do, the wiring is unchanged:

```
xfconf-query -c xfce4-panel -p /plugins/plugin-1/command-restart --create -t string -s /usr/local/bin/<chooser>
xfconf-query -c xfce4-panel -p /plugins/plugin-1/show-command-restart --create -t bool -s true
xfconf-query -c xfce4-panel -p /plugins/plugin-1/confirm-session-command --create -t bool -s false
xfce4-panel -r
```

Whisker pops a confirmation before running a session button, controlled by
`confirm-session-command` (default on). That intermediate modal reads as "the
stock reboot dialog" and buries the chooser behind a second click, so it is
turned off - the chooser is itself the confirmation.

Caveats, all unchanged:

- These are **per-user** settings in the user's xfconf and must be set **as
  the desktop user** with the session's `DISPLAY` and
  `DBUS_SESSION_BUS_ADDRESS` exported (grab them from the running
  `xfce4-panel` process's `/proc/<pid>/environ`). Root cannot set them, which
  is why it was never in an installer script.
- Turning confirm off is global to Whisker's session buttons, not just
  Restart. The only other one shown is Log Out, whose own
  `xfce4-session-logout` modal is its confirmation anyway.
- `plugin-1` is the whiskermenu id on this install; confirm in the panel xml.
- The XFCE panel's stock Action Buttons plugin talks to xfce4-session over
  D-Bus, so shadowing `xfce4-session-logout` in `/usr/local/bin` does **not**
  intercept it. Add a Launcher instead.

---

## Audio panel control + volume keys

Carried over untouched. Symptom was "no sound", then "no UI way to control
it", then "the volume Fn keys don't work". Sound was never broken - the
built-in analog sink was default at 40%, unmuted. Two things were actually
missing, both fixed by one plugin.

`xfce4-pulseaudio-plugin` was **installed but never placed on the panel**
(works fine over PipeWire via pactl). Adding it gives the tray speaker icon:
scroll = volume, click = slider + mute, right-click > Audio mixer =
pavucontrol. It also grabs the `XF86Audio{RaiseVolume,LowerVolume,Mute}` keys
itself when `enable-keyboard-shortcuts` is on (default) - which is why the
volume Fn keys started working the moment it loaded. They were dead before
because nothing owned them. (Brightness Fn keys were always fine - handled by
xfce4-power-manager, separate path.)

```
# create a new plugin above the current max id, then splice it into plugin-ids
xfconf-query -c xfce4-panel -p /plugins/plugin-11 --create -t string -s pulseaudio
xfconf-query -c xfce4-panel -p /panels/panel-1/plugin-ids \
  -t int -s 1 -t int -s 2 -t int -s 3 -t int -s 4 -t int -s 7 -t int -s 11 -t int -s 5
xfce4-panel -r
```

`xfconf-query -p /panels/panel-1/plugin-ids` prints the existing order; the
array set must re-specify **every** element, so copy the current ids and add
yours. Per-user channel, so root cannot do it - manual step, like the Whisker
wiring.

---

## Disk-usage analyzer: QDirStat

Carried over. `qdirstat` as a Filelight replacement - treemap + sortable tree,
fast on big trees, with right-click cleanup actions. In the Whisker menu under
System.

**AUR build** - QDirStat was dropped from the official repos, only
`aur/qdirstat` remains. Built with `yay`; qt6-base and the rest came as
binaries from `extra`, only qdirstat itself compiled. Verify
`/etc/makepkg.conf` uses `-march=x86-64 -mtune=generic` (NOT `-march=native`)
first - portability rule for this stick, since it must run on any machine.
Rebuild after a Qt6 soname bump: `yay -S qdirstat`.

---

## On-stick copy of this runbook, and the session hook

A copy of this file lives on the install at

    /usr/local/share/archusb/RUNBOOK.md

R6 installs it. **CHANGED: the master copy now lives in the airies-boot repo**
at `docs/archusb-runbook.md`, rather than being the on-stick file itself.
Keep the on-stick copy in sync when the repo one changes; there is still no
automation for that.

`~/.claude/env-context.sh` is a Claude Code SessionStart hook that prints what
the install is (hardware it is currently booted on, firmware path, Secure Boot
state, battery, partition layout) and points at the on-stick runbook for
sessions that are actually modifying the install. It is wired up in
`~/.claude/settings.json` under `hooks.SessionStart`, deliberately NOT in
CLAUDE.md - that file is a symlink into `~/git/home/claude-config`, so
anything written there would follow into the repo and into unrelated sessions.

The hook encodes its JSON with python3 rather than jq so it cannot break if jq
is ever absent.

**It needs one edit for falcon:** its partition-layout summary describes a
Ventoy stick. Point it at the new four-partition shape, and consider having it
print `findmnt /efi` - on a stick where a silently unmounted shelf is the
worst failure mode, that is the single most useful thing a session-start
banner can say.
