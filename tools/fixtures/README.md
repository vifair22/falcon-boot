# Station fixtures

Disks for the station rig (docs/notes/station-rework.md): each is a raw
image with an MBR, one active primary partition and an OS on it that boots on
its own under SeaBIOS on an i440fx machine. The station finds them as a
machine's existing OSes and chainloads them.

The images are too large and too licence-bound for the repository. They live
on binhost-builder, with the media they were made from:

    /srv/falcon/fixtures/station/
        freedos.img      FreeDOS 1.4, FAT16, 32 MiB
        win98se.img      Windows 98 SE, FAT32, 2 GiB
        media/           the source media, checked as below
        shots/           the verification screenshots
        scripts/         a copy of this directory

| Image | sha256 | Verified |
|---|---|---|
| `freedos.img` | `e613044e31d9ec6cb2a6629204ed98c14db7308faa6a78158554fec99befa695` | `shots/freedos.png`: `C:\>` prompt |
| `win98se.img` | `10b65fab638ecda8ec813d6d237943d028d2dfbce78a6932b6f9b351b4a03cc8` | `shots/win98se.png`: desktop |
| `win95.img` | not made | no Windows 95 key on record (see below) |

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
under TCG on binhost-builder. The key file is written from the
thinkpad-a21m repository's `docs/LICENSES.md` for the run and deleted after
it; the image's registry holds the key, as any installed Windows does.

Windows 95 OSR2.5, once a key file exists (LICENSES.md has no Windows 95
key, in any revision):

    scripts/mk-win9x-fixture.sh --iso media/win95osr25.iso \
        --floppy media/win95osr25-ebd.img --srcdir WIN95 \
        --inf scripts/win95-msbatch.inf --key-file secret/win95.key \
        --mem 64 --setup-args "/is /iq" --nudge "alt-y ret" win95.img

A trial run with a placeholder key reached Setup's licence page (FDISK /MBR,
FORMAT, the copy to `C:\WIN95` and Setup's DOS stage all worked). Its licence
dialog is Yes/No, hence `alt-y`.

## Media

| File | Source | Check |
|---|---|---|
| `FD14-LiteUSB.zip` | ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/ | sha256 `857dcd2e...338b` in the release's `verify.txt` |
| `win98se.iso` | the user's own disc image | sha256 `2adfb46df8a9c7bbd2f67bff07461cc2f9d9ec8e01f0e112cb044c9e3e62f607` |
| `win95osr25.iso` | archive.org item `windows-95-oem-x03-52599` (OEM CD X03-52599, 4.00.950 C) | sha256 `ee30571c4a6f290ef3e261ee1a1ec5c3ecc39c9688b30133518f7193500b59c4`, as the item's description publishes |
| `win95osr25-ebd.img` | archive.org item `en_win95c_osr25_ebd.img` | md5 `192e482cae3396352371e0700dbb1537`, sha1 `0fbb9e028d4f83db4378cc7a8ee0749ebf5423b6`, archive.org's own file checksums (the item publishes none) |
