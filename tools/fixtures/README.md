# Station fixtures

Disks for the station rig (DESIGN Section 12.11): each is a raw
image with an MBR, one active primary partition and an OS on it that boots on
its own under SeaBIOS on an i440fx machine. The station finds them as a
machine's existing OSes and chainloads them.

The images are too large and too licence-bound for the repository. They live
on the build host in the fixture store, with the media they were made from
and the other rigs' disks:

    /srv/falcon/fixtures/
        station/
            freedos.img      FreeDOS 1.4, FAT16, 32 MiB
            win98se.img      Windows 98 SE, FAT32, 2 GiB
            win95.img        Windows 95 OSR2.5, FAT32, 2 GiB
            media/           the source media, checked as below
            shots/           the verification screenshots
            scripts/         a copy of this directory
        cf-98-bridge.img     Windows 98 SE on a CF card (W98_IOS_CF,
                             run-cfstation-rig.sh)
        a21m/DJSA-210.img    the A21m's disk (A21M_ORIG, mk-a21m-station.sh)
        memtest/             memtest86+ 8.10, i586 and x86-64 (MEMTEST)
        SHA256SUMS           over station/, memtest/ and cf-98-bridge.img

The pipeline's rig jobs mount the store read-only at /fixtures
(`FALCON_FIXTURES`, tools/coverage/harness.sh). Its copy is on Unraid at
`Documents/Vincent/falcon-fixtures/`, checked against SHA256SUMS; the A21m
image's original is beside it, in `Documents/Vincent/thinkpad-a21m/`.

| Image | sha256 | Verified |
|---|---|---|
| `freedos.img` | `e613044e31d9ec6cb2a6629204ed98c14db7308faa6a78158554fec99befa695` | `shots/freedos.png`: `C:\>` prompt |
| `win98se.img` | `10b65fab638ecda8ec813d6d237943d028d2dfbce78a6932b6f9b351b4a03cc8` | `shots/win98se.png`: desktop |
| `win95.img` | `b7836584be52a7b10ee013daa8fa1c55bf818b4d6e928caaf7221cc31fcc1ff2` | `shots/win95.png`: desktop (`fixture-shot.sh --mem 64 --icount 2`) |

## Use

`make test-station-fixtures` (falcon-bios/tools/run-station-fixture-rig.sh,
docs/DESIGN.md Section 12.11) boots each image behind the BIOS station. It
reads them from `build/fixtures/` and checks each against the sha256 above;
copy them there sparse (scp would write the 2 GB image whole):

    ssh builder@buildhost \
        'tar -cSf - -C /srv/falcon/fixtures/station freedos.img win98se.img' |
        tar -xSf - -C build/fixtures/

The rig only reads them: every write goes into a qcow2 overlay.

## Scripts

Everything runs inside the coverage harness image
(`tools/coverage/Dockerfile`), which has QEMU, mtools and xorriso:

    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/st" -w /st \
        falcon-boot-harness:latest scripts/<script> ...

- `mk-freedos-fixture.sh ZIP VERIFY OUT` checks FD14-LiteUSB.zip against
  the release's `verify.txt`, takes `FD14LITE.img` out of it and renames
  `SETUP.BAT` so it boots to a prompt instead of the installer.
- `mk-win9x-fixture.sh` installs Windows 95 OSR2 or 98 SE unattended from
  the CD image, a DOS boot floppy, an msbatch.inf template and a key file.
  The key is read from the file and is never in a command line or in the
  repository. Its header has the whole procedure.
- `win98se-msbatch.inf`, `win95-msbatch.inf` are the templates;
  `@PRODUCTKEY@` is where the key goes.
- `fixture-shot.sh IMG OUT.png` boots an image alone, read-only, and
  screenshots it.
- `vmctl.py` is the QEMU monitor client the other two use (screendump to
  PNG, sendkey, "is this the desktop", "has the screen stopped changing").

Windows 98 SE:

    scripts/mk-win9x-fixture.sh --iso media/win98se.iso --srcdir WIN98 \
        --inf scripts/win98se-msbatch.inf --key-file secret/win98se.key \
        --setup-args "/is /ie /p i" win98se.img

`/p i` keeps Setup from installing the ACPI HAL. The CD is an OEM one: its
first boot runs a Start Wizard that asks for the name, the licence and the key
again (prefilled from msbatch.inf), and the script's default nudge, Alt+A
then Enter, walks it. The boot after Setup's last restart hung behind the
startup logo in both runs made; the script restarts QEMU when two nudges
change nothing, and the cold start reaches the desktop. About 70 minutes
under TCG on the build host. The key file is written from the
thinkpad-a21m repository's `docs/LICENSES.md` for the run and deleted after
it; the image's registry holds the key, as any installed Windows does.

Windows 95 OSR2.5 (the OEM key in the thinkpad-a21m repository's
`docs/LICENSES.md`, 6ba8bf4 there):

    scripts/mk-win9x-fixture.sh --iso media/win95osr25.iso \
        --floppy media/win95osr25-ebd.img --srcdir WIN95 \
        --inf scripts/win95-msbatch.inf --key-file secret/win95.key \
        --mem 64 --icount 2 --setup-args "/is /iq" --nudge "alt-y ret" \
        --still 6 win95.img

Two things this one needs that 98 SE does not:

- `--icount 2`. TCG on the build host's i9 is still fast enough to fault
  Windows 95's IOS timing loop at the first boot from C: ("While initializing
  device IOS: Windows protection error"); `-icount shift=2` holds the guest
  at 250 MHz. The station rig boots the image the same way (its `w95-own`
  and `w95-second` cases).
- The key typed by hand. This OEM CD's Certificate of Authenticity page
  stays empty whether msbatch.inf gives the key as `ProductID` or
  `ProductKey`, and batch mode presses Next on it, which raises "not valid".
  At that dialog (about five minutes in), Enter for Re-enter, then the
  key's three digit groups typed through the QEMU monitor (`sendkey`, one
  digit at a time, the boxes advance on their own), then Enter. `--still 6`
  keeps the nudge from firing while that is done. Screenshots taken while
  the key is on screen are deleted afterwards.

Setup's last stage stops on the Add Printer Wizard once; Esc cancels it.
About 25 minutes on the build host. The licence dialog is Yes/No, hence
`alt-y`.

## Media

| File | Source | Check |
|---|---|---|
| `FD14-LiteUSB.zip` | ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/ | sha256 `857dcd2e...338b` in the release's `verify.txt` |
| `win98se.iso` | the user's own disc image | sha256 `2adfb46df8a9c7bbd2f67bff07461cc2f9d9ec8e01f0e112cb044c9e3e62f607` |
| `win95osr25.iso` | archive.org item `windows-95-oem-x03-52599` (OEM CD X03-52599, 4.00.950 C) | sha256 `ee30571c4a6f290ef3e261ee1a1ec5c3ecc39c9688b30133518f7193500b59c4`, as the item's description publishes |
| `win95osr25-ebd.img` | archive.org item `en_win95c_osr25_ebd.img` | md5 `192e482cae3396352371e0700dbb1537`, sha1 `0fbb9e028d4f83db4378cc7a8ee0749ebf5423b6`, archive.org's own file checksums (the item publishes none) |
