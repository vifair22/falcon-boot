# Third-party binaries in a falcon-boot release

falcon-boot itself is GPL-3.0-or-later (LICENSE). A release also carries two
programs it did not build, unmodified, each under its own licence, and the
signing appliance, which is built from others.

## shim and MokManager

`usr/lib/falcon-boot/shim/bins/fedora-16.1-shimx64.efi` and
`fedora-16.1-mmx64.efi`: shim 16.1 as Fedora signs and ships it, signed by
Microsoft's UEFI CA, taken from the package `shim-x64-16.1-7.x86_64.rpm`
(<https://kojipkgs.fedoraproject.org/packages/shim/16.1/7/x86_64/>) by
`falcon-efi/tools/shim-spike/fetch-shims.sh`.

- Licence: BSD-2-Clause (shim's COPYRIGHT file).
- Source: <https://github.com/rhboot/shim/releases/tag/16.1>, and Fedora's
  source package for the build, `shim-unsigned-x64`.

## memtest86+

`usr/lib/falcon-boot/memtest/memtest-i586.bin` and `memtest-x86_64.efi`,
`MEMTEST.BIN` in the station bundle, and `MEMTEST.BIN` on the jump floppy:
memtest86+ 8.10 as released, `mt86p_810_i586` and `mt86p_810_x86_64` from
`mt86plus_8.10.binaries.zip`
(<https://memtest.org/download/v8.10/mt86plus_8.10.binaries.zip>).

| file | sha256 |
|---|---|
| `mt86plus_8.10.binaries.zip` | `7e6c5162cb84ab959aeb9d13c9cfd6976b0dec3b34936b73820b20c55eb26c29` |
| `mt86p_810_i586` | `99807cbc4d8a017279932e4bf9b46b7fd3b6c2ea2c1284c34a042f01591fbbad` |
| `mt86p_810_x86_64` | `11808288ea1ee332f7d89e683cef10fbcdf2d6585bb972688ed9fe0e3d30fc55` |

- Licence: GPL-2.0.
- Source: <https://github.com/memtest86plus/memtest86plus/releases/tag/v8.10>.

## The signing appliance

`usr/lib/falcon-boot/appliance/appliance-uki.efi` and
`appliance-uki-aa64.efi`: the on-stick signing appliance (DESIGN Section
7.5), a UKI built by `falcon-appliance/image/mk-appliance.sh`. Besides
falcon-boot's own PID 1 and `falcon-boot-sign`, each carries, unmodified:

- Fedora's kernel and eight of its modules (exfat, loop, fat, vfat,
  usb-storage, uas, i2c-hid, i2c-hid-acpi), at the build
  `tools/fetch-fedora-kernel.sh` pins. Licence: GPL-2.0.
- `sbsign` and `sbverify` (sbsigntool, GPL-3.0-or-later with an OpenSSL
  exception) and the libraries they load: glibc (LGPL-2.1-or-later),
  OpenSSL's libcrypto (Apache-2.0), zlib (Zlib) and zstd (BSD-3-Clause or
  GPL-2.0), from Debian.
- systemd-stub, `linuxx64.efi.stub` and `linuxaa64.efi.stub`
  (LGPL-2.1-or-later), from Debian's systemd-boot-efi.

`appliance-uki.sources` and `appliance-uki-aa64.sources`, beside each UKI,
name the exact builds: the Fedora kernel release with its source package
URL, and each Debian package with its source package and version, as the
build that made the release found them.
