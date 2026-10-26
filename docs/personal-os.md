# Building a personal OS for falcon-boot

falcon-boot carries an encrypted operating system of your own beside the
ISOs on the stick (or on a station's own disk). You bring the OS; this page
is the contract it meets so falcon can unlock it, launch it, and hand it the
passphrase you already typed.

Nothing here is distribution-specific except the examples. The tested build
is Arch with mkinitcpio; a dracut build on Gentoo with systemd is proven
too. Any distribution that produces a signed UKI and boots with a systemd
initramfs can meet the contract. `docs/Manual.md` Section 5 is the deep
reference for every mechanism named below; this page is the on-ramp.

## How the handoff works

1. You pick the personal-OS entry; falcon prompts once for the passphrase.
2. falcon unlocks **p3**, the encrypted shelf, and reads your signed UKI off
   the filesystem inside it.
3. falcon launches the UKI through the platform's verified load path, with
   the passphrase attached as a credential (systemd-stub's companion-credential
   pickup, Manual 5.3).
4. The UKI's initramfs reads that credential and uses it to unlock **p4**,
   your root, then pivots into it. No second prompt.

p3 and p4 share one passphrase, so the string you typed opens both. p4 is a
separate partition only so the loader never has to mount your root.

## The contract

An OS boots from the shelf when all of these hold:

- **A UKI.** A single-file kernel + initramfs + cmdline built with
  systemd-stub (`ukify`, `kernel-install`, or `falcon-boot-sign uki`). The
  stub is what picks up the passphrase credential; a loose kernel + initrd
  pair will not.
- **Signed with your stick's MOK.** Under Secure Boot the platform verifies
  it; an unsigned or wrong-key UKI is refused (Manual 11.1 signs it, 5.4
  places it).
- **On the shelf, named `*.efi`.** Copy it into `/uki/` on p3 (falcon also
  reads `/EFI/Linux/`). The loader offers every regular file whose name ends
  `.efi`, sorted by name; one match boots directly, several are offered in a
  post-unlock chooser.
- **A systemd initramfs.** The credential arrives the systemd way; a
  non-systemd initramfs cannot consume it.
- **The passphrase-handoff drop-in** (below), present both in the initramfs
  and in the real root.
- **p4 is LUKS2 and shares p3's passphrase.** What lives inside p4 (plain
  ext4, LVM, btrfs, whatever) and how your initramfs finds root is your
  distribution's normal encrypted-root setup. falcon adds nothing to it but
  the drop-in.

## The passphrase-handoff drop-in

This is the one falcon-specific piece. PID 1 files a credential arriving
from the stub under `/run/credentials/@encrypted/` and, by default, tries to
*decrypt* it as a `systemd-creds` blob. Our credential is the raw
passphrase, so the generated `systemd-cryptsetup@.service` dies at
`CREDENTIALS` before it ever runs, and the boot drops to emergency mode with
a bare `Failed to start Cryptography Setup for <name>`. The fix is a one-file
`systemd-cryptsetup@.service` drop-in that clears the import and takes the
plaintext route:

    [Service]
    ImportCredential=
    LoadCredential=cryptsetup.passphrase:/.extra/credentials/cryptsetup.passphrase.cred

It must be installed into the initramfs. It must **also** be present in the
real root: `/run` is one tmpfs across the pivot, so the same credential is
still there when the real root's generator writes its own units from
`/etc/crypttab`, and without the drop-in those units fail the same way (for
example p3's own unit, which opens from a keyfile and needs no credential at
all). Put the file at
`/etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf`
on the real root, and install the same file into the initramfs so a rebuild
after a kernel or systemd update keeps it.

Verify it landed in a built image with:

    lsinitrd <uki> | grep falcon-credential

### mkinitcpio (Arch, tested)

Use the `systemd` hook, not `base`/`udev`, so the initramfs is systemd. Keep
the drop-in on the real root (see above) and add an install hook that copies
it into the image. `/etc/initcpio/install/falcon-credential`:

    build() {
        add_file /etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf \
            /etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf
    }
    help() { echo "ships the falcon passphrase-handoff drop-in"; }

Then list it in `mkinitcpio.conf`, after `systemd`:

    HOOKS=(systemd ... sd-encrypt ... falcon-credential)

### dracut (other systemd distributions)

A dracut module installs the same drop-in. Keep the drop-in source at
`/etc/falcon/initrd-cryptsetup.conf` and put a module at
`/usr/lib/dracut/modules.d/99falcon-credential/module-setup.sh`:

    #!/bin/bash
    check() {
        # systemd-cryptsetup lives in systemd's utility directory, not on
        # PATH, so require_binaries cannot find it by name.
        [[ -x "${systemdutildir}/systemd-cryptsetup" ]] || return 1
        return 0
    }

    depends() { echo systemd crypt; }

    install() {
        inst_simple /etc/falcon/initrd-cryptsetup.conf \
            /etc/systemd/system/systemd-cryptsetup@.service.d/falcon-credential.conf
    }

Enable it beside the systemd initramfs with a `dracut.conf.d` file:

    add_dracutmodules+=" systemd crypt falcon-credential "

## Putting the OS on the stick

1. **Prepare p4.** Install your OS onto p4 as a normal LUKS2 (argon2id)
   encrypted root, using the same passphrase as p3. Set its cmdline and
   `/etc/crypttab` to unlock p4 and mount root exactly as any encrypted-root
   install would.
2. **Add the drop-in.** Install the handoff drop-in on the real root and into
   the initramfs by the mkinitcpio or dracut recipe above.
3. **Build and sign the UKI.** Build it with the embedded cmdline from step 1,
   then sign it with the stick's MOK. `falcon-boot-sign uki` builds one from a
   kernel, an initramfs and an os-release file (Manual 4.4); `falcon-boot-sign`
   also signs a UKI your distribution already built (Manual 11.1).
4. **Place it on the shelf.** Copy the signed UKI into p3's `/uki/` as a
   `*.efi` file (Manual 5.4). From the running personal OS, p3 is normally
   mounted at `/efi`; from another Linux, open p3 and copy it in.
5. **Boot it.** Pick the personal-OS entry, type the passphrase once. Under
   Secure Boot, the first boot enrolls the MOK in MokManager (Manual 3.1).

## Keeping it alive across updates

Build the signed UKI on every kernel or systemd update, not once. Because the
drop-in ships through the initramfs builder, a normal rebuild keeps it. For an
automated route, a `kernel-install` plugin can drop each new signed UKI onto
the shelf as `<name>-<kver>.efi` and refresh a stable `<name>.efi` copy, so a
station default naming `<name>.efi` survives kernel bumps (Manual 5.3).

## When it does not work

- **Emergency mode, `Failed to start Cryptography Setup`:** the drop-in is
  missing from the initramfs or the real root. Confirm with `lsinitrd`
  (Manual 5.3).
- **The UKI is not offered in the menu:** the name does not end `.efi`, it is
  not directly under `/uki` (or `/EFI/Linux`), or a name starting `appliance`
  is reserved for the signing appliance (Manual 5.4).
- **`the UKI is not signed with an enrolled key`:** sign it with the MOK this
  machine enrolled, or turn Secure Boot off (Manual 5.4).
- **A second passphrase prompt from the initramfs:** falcon warns on its own
  screen when it could not deliver the credential; the boot still succeeds,
  the OS just asks again. A plaintext shelf has no passphrase to hand over.
