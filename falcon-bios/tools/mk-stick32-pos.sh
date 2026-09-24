#!/usr/bin/env bash
# falcon-bios: install Arch Linux 32 as the personal OS of a 32-bit stick
# (docs/notes/stick32.md).
#
# Run on a stick `falcon-boot-mkstick create --p4-kdf-memory` made: it opens
# p4 and p3 with the stick's passphrase, installs Arch Linux 32 (i686) into
# p4, mounts p3 at the target's /efi, and has mkinitcpio write the UKI
# straight onto p3, where falcon-bios reads it. Every mapping and mount is
# undone on exit.
#
# Usage (as root):
#   falcon-bios/tools/mk-stick32-pos.sh DEV SERIAL PASSFILE WORKDIR
#     DEV       the stick, e.g. /dev/sdb
#     SERIAL    its serial number; the script refuses any other device
#     PASSFILE  the p3/p4 passphrase, no trailing newline
#     WORKDIR   scratch: the i686 keyring, the package cache, the mount point
#
# Needs, on the host: pacstrap and arch-chroot (arch-install-scripts),
# pacman-key, bsdtar, curl, cryptsetup, and a kernel that runs i686 binaries
# (CONFIG_IA32_EMULATION), since package scriptlets and mkinitcpio run inside
# the i686 target.
set -euo pipefail

[[ $# -eq 4 ]] || { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
DEV=$1
WANT_SERIAL=$2
PASS=$3
W=$4
[[ $EUID -eq 0 ]] || { echo "ERROR: run as root" >&2; exit 1; }
[[ "$(lsblk -dno SERIAL "$DEV")" == "$WANT_SERIAL" ]] ||
    { echo "ERROR: $DEV is not serial $WANT_SERIAL" >&2; exit 1; }
[[ -f "$PASS" ]] || { echo "ERROR: no passphrase file $PASS" >&2; exit 1; }
if lsblk -no MOUNTPOINTS "$DEV" | grep -q .; then
    echo "ERROR: something on $DEV is mounted" >&2; exit 1
fi

MNT=$W/mnt
GPG=$W/gnupg
CONF=$W/pacman.conf
CACHE=$W/cache
mkdir -p "$W" "$MNT" "$CACHE"

cleanup() {
    set +e
    umount -R "$MNT" 2>/dev/null
    cryptsetup close crypt32efi 2>/dev/null
    cryptsetup close crypt32root 2>/dev/null
}
trap cleanup EXIT

# ---- the i686 keyring, kept apart from the host's ----
# pacman-key's own --populate, from the unpacked keyring package, so the
# trust setup is pacman's and not an imitation of it.
KR=archlinux32-keyring-20251214-1.0-any.pkg.tar.zst
if [[ ! -d "$W/kr" ]]; then
    curl -sSfLo "$W/$KR" "https://mirror.archlinux32.org/i686/core/$KR"
    mkdir -p "$W/kr" && bsdtar -xf "$W/$KR" -C "$W/kr"
fi
rm -rf "$GPG"
pacman-key --gpgdir "$GPG" --init
pacman-key --gpgdir "$GPG" \
    --populate-from "$W/kr/usr/share/pacman/keyrings" --populate archlinux32
# One packager key in the 20251214 keyring (TasosSah) is signed by two Arch
# Linux 32 master keys and a developer, one short of what the pgp trust
# model wants from marginally trusted signers, so pacman calls it marginal
# and refuses the packages it signed. It ships in the keyring package
# itself; sign it locally, and only it.
pacman-key --gpgdir "$GPG" --lsign-key 80EC18799E8BCD375C6E64ABE4D41569196B1160

cat > "$CONF" <<EOF
[options]
Architecture = i686
GPGDir = $GPG
CacheDir = $CACHE
SigLevel = Required DatabaseOptional
LocalFileSigLevel = Optional
ParallelDownloads = 8

[core]
Server = https://mirror.archlinux32.org/\$arch/\$repo
[extra]
Server = https://mirror.archlinux32.org/\$arch/\$repo
[community]
Server = https://mirror.archlinux32.org/\$arch/\$repo
EOF

# ---- open and mount ----
cryptsetup open --key-file "$PASS" "${DEV}4" crypt32root
cryptsetup open --key-file "$PASS" "${DEV}3" crypt32efi
e2label /dev/mapper/crypt32root ROOT32 || true
mount -o noatime /dev/mapper/crypt32root "$MNT"
mkdir -p "$MNT/efi"
mount -o noatime /dev/mapper/crypt32efi "$MNT/efi"
LUKS4=$(blkid -s UUID -o value "${DEV}4")
LUKS3=$(blkid -s UUID -o value "${DEV}3")
echo "p4 LUKS $LUKS4, p3 LUKS $LUKS3"

# ---- the packages ----
# python and python-pefile are ukify's, which mkinitcpio builds the UKI with.
pacstrap -C "$CONF" -G -M "$MNT" \
    base archlinux32-keyring linux linux-firmware mkinitcpio cryptsetup \
    sudo nano vim less man-db networkmanager openssh \
    e2fsprogs dosfstools exfatprogs usbutils pciutils python python-pefile

# ---- the system ----
cp "$CONF" "$MNT/etc/pacman.conf"
sed -i -e '/^GPGDir/d' -e '/^CacheDir/d' "$MNT/etc/pacman.conf"
arch-chroot "$MNT" pacman-key --init
arch-chroot "$MNT" pacman-key --populate archlinux32
arch-chroot "$MNT" pacman-key --lsign-key 80EC18799E8BCD375C6E64ABE4D41569196B1160

echo falcon32 > "$MNT/etc/hostname"
echo 'en_US.UTF-8 UTF-8' > "$MNT/etc/locale.gen"
arch-chroot "$MNT" locale-gen
echo 'LANG=en_US.UTF-8' > "$MNT/etc/locale.conf"
ln -sf /usr/share/zoneinfo/UTC "$MNT/etc/localtime"
arch-chroot "$MNT" systemctl enable NetworkManager systemd-timesyncd
# The root password starts as the stick's passphrase; change both
# (docs/notes/stick32.md, "The passphrase").
printf 'root:%s\n' "$(cat "$PASS")" | arch-chroot "$MNT" chpasswd

# p3 opens after the pivot from a keyfile kept on the encrypted root. The
# slot is pbkdf2 at the loader's iteration count, like the passphrase's, so
# nothing on p3 is a KDF the loader cannot parse.
install -d -m 700 "$MNT/etc/cryptsetup-keys.d"
if [[ ! -f "$MNT/etc/cryptsetup-keys.d/crypt32efi.key" ]]; then
    dd if=/dev/urandom of="$MNT/etc/cryptsetup-keys.d/crypt32efi.key" bs=64 \
        count=1 status=none
    chmod 600 "$MNT/etc/cryptsetup-keys.d/crypt32efi.key"
    cryptsetup luksAddKey --key-file "$PASS" --pbkdf pbkdf2 \
        --pbkdf-force-iterations 200000 "${DEV}3" \
        "$MNT/etc/cryptsetup-keys.d/crypt32efi.key"
fi
echo "crypt32efi UUID=$LUKS3 /etc/cryptsetup-keys.d/crypt32efi.key luks,discard" \
    > "$MNT/etc/crypttab"
cat > "$MNT/etc/fstab" <<EOF
/dev/mapper/crypt32root  /     ext4  noatime  0 1
/dev/mapper/crypt32efi   /efi  ext4  noatime,nofail  0 2
EOF

# A portable initramfs: no autodetect (a stick boots on machines that are
# not this one), and no kms, whose GPU firmware made the UKI 196 MB, read
# through the BIOS on every boot; the GPU drivers load from root after the
# pivot. systemd and sd-encrypt, so the passphrase the loader hands over as
# a credential opens p4, and falcon-credential so it is taken as plaintext.
cat > "$MNT/etc/mkinitcpio.conf.d/falcon32.conf" <<'EOF'
HOOKS=(base systemd modconf keyboard sd-vconsole block sd-encrypt filesystems fsck falcon-credential)
EOF
# The credential handoff's two halves, as the 64-bit stick has them (archusb
# RUNBOOK, "falcon-credential"): PID 1 files the passphrase under
# @encrypted and the generated unit then dies trying to decrypt it, in the
# initramfs and again after the pivot. Taken from the running archusb
# install, which is where they are maintained.
install -Dm644 /etc/initcpio/install/falcon-credential \
    "$MNT/etc/initcpio/install/falcon-credential"
install -Dm644 /etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf \
    "$MNT/etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf"
touch "$MNT/etc/vconsole.conf"
echo "rd.luks.name=$LUKS4=crypt32root root=/dev/mapper/crypt32root rw console=tty0 console=ttyS0,115200" \
    > "$MNT/etc/kernel/cmdline"

# The UKI goes straight onto p3, where falcon-bios reads it, so a kernel
# update regenerates the image the loader actually boots.
mkdir -p "$MNT/efi/uki"
cat > "$MNT/etc/mkinitcpio.d/linux.preset" <<'EOF'
ALL_kver="/boot/vmlinuz-linux"
PRESETS=('default')
default_uki="/efi/uki/arch32.efi"
EOF
# ukify picks its stub from uname -m, which says x86_64 in a chroot on a
# 64-bit host; setarch makes it say i686, as the machine itself will.
arch-chroot "$MNT" setarch i686 mkinitcpio -P
ls -la "$MNT/efi/uki/"
sync
