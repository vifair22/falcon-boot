# The 32-bit stick (2026-09-24)

A second falcon stick for the machines the 256 GB one cannot serve: Windows
2000 and XP era PCs that boot from USB but have no 64-bit mode. The line is
USB boot; a BIOS too old to offer it is out of scope. The loader is the one
falcon-bios image both sticks carry (bios-route.md Section 6h); what makes
this stick 32-bit is what is on it.

| part | contents |
|---|---|
| p1 | the ESP `mkstick create` always writes; unused on a BIOS machine |
| p2 | 32-bit ISOs, in `Linux/` |
| p3 | LUKS2 (pbkdf2, the loader's 200000 iterations): `/uki/arch32.efi`, the MOK |
| p4 | LUKS2 (argon2id, 128 MiB, time cost 4, one thread): Arch Linux 32 |
| gap | falcon-bios stages 1 to 3 (`mkstick bios`) |

The current one is the 128 GB Corsair Voyager, serial
`19368169000125990059`.

## Building one

```bash
# 1. The stick. --p4-kdf-memory is what makes p4 openable on an old machine.
printf '%s' 'the passphrase' > pass          # no newline; p3 and p4 share it
sudo build/bin/falcon-boot-mkstick create /dev/sdX --force \
    --passphrase-file pass --iso-size 60G --p4-kdf-memory 131072 \
    --cn "falcon-boot i686 stick"
sudo build/bin/falcon-boot-mkstick bios /dev/sdX --no-backup

# 2. Arch Linux 32 into p4, its UKI onto p3.
sudo falcon-bios/tools/mk-stick32-pos.sh /dev/sdX <serial> pass /some/workdir

# 3. ISOs onto p2.
sudo mount /dev/sdX2 /mnt && sudo mkdir -p /mnt/Linux
sudo cp alpine-standard-*-x86.iso archlinux32-*-i686.iso /mnt/Linux/
sudo umount /mnt
```

`mk-stick32-pos.sh` refuses a device whose serial is not the one given, and
undoes every mapping and mount on the way out. It installs a base system
(networking, ssh, editors); a desktop is `pacman -S` away once it boots.

## What it took, and why each one

Four things stood between a stock `mkstick create` plus a stock Arch Linux 32
install and a stick that boots on a Pentium III. Each is in the script or in
`mkstick`, with the reason beside it.

**p4's KDF.** `mkstick create` gives p4 cryptsetup's stock argon2id, which
cryptsetup tunes on the machine building the stick: here a gibibyte, time
cost 13, four threads. The initramfs of a machine with 512 MB cannot run
that, and nothing says so on screen beyond a failed unit. `--p4-kdf-memory`
caps the memory, pins the time cost at 4 (cryptsetup would otherwise tune
that on a processor twenty times faster than the target) and uses one
thread. 131072 KiB is what this stick uses.

**The credential.** A UKI booted by falcon-bios gets the passphrase the same
way one booted by sd-stub does, as `/.extra/credentials/cryptsetup.passphrase.cred`,
and PID 1 files it under `/run/credentials/@encrypted/` and the generated
cryptsetup unit then fails trying to decrypt it (status 243, CREDENTIALS).
This is the 64-bit stick's problem too and its fix is the archusb runbook's
`falcon-credential` hook and drop-in; the script installs the same two
files, taken from the running archusb install.

**The keyring.** Arch Linux 32's keyring (20251214) carries a packager key
(TasosSah, `80EC 1879 9E8B CD37 5C6E 64AB E4D4 1569 196B 1160`) signed by
two master keys and a developer, one short of what the pgp trust model
wants from marginally trusted signers, so pacman refuses what it signed.
The script signs that key locally, in the build keyring and in the
installed system's, and only that key.

**The UKI.** mkinitcpio builds it with `ukify`, which needs Python and
`python-pefile` in the target, picks its stub from `uname -m` (so the build
runs under `setarch i686` in a chroot on a 64-bit host), and without
`autodetect` the `kms` hook pulls every GPU's firmware into the initramfs:
196 MB, read through the BIOS on every boot. Without `kms` it is 45 MB, and
the GPU drivers load from root after the pivot.

## The passphrase

The stick was made with a temporary passphrase, `falcon-i686`, which is also
root's password. Change all three. p3's keyslot has to stay pbkdf2 at the
loader's iteration count and p4's at the low-memory parameters, so both
changes name them:

```bash
sudo cryptsetup luksChangeKey /dev/sdX3 --pbkdf pbkdf2 \
    --pbkdf-force-iterations 200000
sudo cryptsetup luksChangeKey /dev/sdX4 --pbkdf argon2id \
    --pbkdf-memory 131072 --pbkdf-parallel 1 --pbkdf-force-iterations 4
# and, booted into it: passwd
```

p3 and p4 must end up with the same passphrase: the loader opens p3 with
what was typed and hands the same string on for p4.

## Measured (QEMU, SeaBIOS, `-cpu pentium3`, the real stick read-only)

- 512 MB: the menu, the passphrase, p3 unlocked on the portable AES path,
  `arch32.efi` read and unwrapped, the i686 kernel through the 32-bit
  entry, p4 opened from the credential with no second prompt, p3 at `/efi`
  from its keyfile, `falcon32 login:`.
- Alpine 3.24.2 x86 and Arch Linux 32 2024.07.10 off p2, direct: the kernel
  read as 32-bit, the i686 falcon-hook appended, a login prompt each.

## Measured on hardware (2026-09-24)

A Windows 7 era machine, BIOS mode, booted the stick to the personal OS.
The retry after a wrong passphrase ran its asterisks off the text area: the
attempt-2 prompt is 80 characters with its TAB hint, the 80x25 screen's text
area was 64 columns, and the echo did not follow the wrap. Fixed the same
day in `falcon-bios/src/screen.c` (the echo and its erase wrap, a backspace
climbs back over a wrap) and `falcon-core/src/tui/tui.c` (an 80-column
console gets a one-column margin, so 74 columns of text).

Not yet measured on hardware: the ISOs, and a processor without a 64-bit
mode.
