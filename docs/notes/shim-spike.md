# shim plumbing spike

Result of the spike that DESIGN.md Section 11 step 1 gates the rest of the
project on, and that Section 7.6 asks to be run before anything is built on a
particular load path.

Question asked: of the four load-path branches in Section 7.6, which one does
a real, shippable, Microsoft-signed shim actually offer us, and which shim
should airies-boot ship?

Short answer: **branch B**, on any current (16.x) distro shim. Branch A is
offered by nobody. There is also a fifth mechanism nobody wrote down, and it
is the one that makes the design work with no code change at all: current shim
patches `gBS->LoadImage` and `gBS->StartImage` in place, so a plain
`gBS->LoadImage` call from our loader **is** shim's verifying loader. Proven
empirically below, under Secure Boot, on three vendors' shims.

Everything here was produced on 2026-09-12 with the artifacts and scripts in
`tools/shim-spike/`; `build/shim/` holds the downloads, staged ESPs and serial
logs. Nothing in `src/` was touched.


## 1. Packages classified

Pinned versions; `tools/shim-spike/fetch-shims.sh` re-fetches exactly these.

| Tag | Package | URL | sha256 |
| --- | --- | --- | --- |
| fedora-16.1 | `shim-x64-16.1-7.x86_64.rpm` | `https://kojipkgs.fedoraproject.org/packages/shim/16.1/7/x86_64/` | `0a119f34...dd608` |
| fedora-15.8 | `shim-x64-15.8-5.x86_64.rpm` | `https://kojipkgs.fedoraproject.org/packages/shim/15.8/5/x86_64/` | `26779330...d2791` |
| suse-16.1 | `shim-16.1-4.2.x86_64.rpm` | `https://provo-mirror.opensuse.org/tumbleweed/repo/oss/x86_64/` | `3338c457...1b95c` |
| debian-16.1 | `shim-signed_1.51+16.1-2_amd64.deb` | `https://deb.debian.org/debian/pool/main/s/shim-signed/` | `993868c3...200e74` |

Packaging notes worth keeping:

- Fedora's rpm stages under `/usr/lib/efi/shim/<ver>/EFI/{BOOT,fedora}/`, not
  `/boot/efi`. `BOOTX64.EFI`, `EFI/fedora/shim.efi` and `EFI/fedora/shimx64.efi`
  are byte-identical; `mmx64.efi` and `fbx64.efi` sit beside them.
- openSUSE's rpm stages `/usr/share/efi/x86_64/`. It ships four shim builds:
  `shim.efi` == `shim.nx.efi`, and `shim-opensuse.efi` == `shim.non-nx.efi`.
  The pair differs only in the NX-compatibility PE flag; the classification
  below is identical for both.
- Debian's `shim-signed` carries only `usr/lib/shim/shimx64.efi.signed`.
  MokManager comes from a separate `shim-helpers-amd64-signed` package, which
  matters if Debian's shim is ever the one we ship.
- `rpm2cpio` is not installed on this machine. `bsdtar` reads `.rpm` and `.deb`
  directly and was used for both.


## 2. Static classification

| | fedora-15.8 | fedora-16.1 | suse-16.1 | debian-16.1 |
| --- | --- | --- | --- | --- |
| shim release (`.data.ident`) | 15.8 | 16.1 | 16.1 | 16.1 |
| upstream commit | 5914984a | afc49558 | afc49558 | afc49558 |
| MS UEFI CA 2011 signature | yes | yes | yes | yes |
| MS UEFI CA 2023 signature | **no** | yes | yes | yes |
| distro signature | no | no | openSUSE | no |
| next-stage loader name | `\grubx64.efi` | `\grubx64.efi` | `\grub.efi` | `\grubx64.efi` |
| MokManager name | `\mmx64.efi` | `\mmx64.efi` | `\MokManager.efi` | `\mmx64.efi` |
| fallback name | `\fbx64.efi` | `\fbx64.efi` | `\fallback.efi` | `\fbx64.efi` |
| `.sbat` vendor row | `shim.fedora,3` | `shim.fedora,3` | `shim.opensuse,1` | `shim.debian,1` |
| `.sbatlevel` latest | `2024010900` shim,4 | `2025051000` shim,4 grub,5 | `2025051000` shim,4 grub,5 | `2025051000` shim,4 grub,5 |
| vendor cert | Fedora SB CA (1119 B) | Fedora SB CA (1119 B) | openSUSE SB CA (1144 B) | Debian SB CA (930 B) |
| vendor dbx | none | none | 7566 B | 5500 B |
| SHIM_LOCK GUID xrefs | 22 | 21 | 21 | 21 |
| SHIM_IMAGE_LOADER GUID xrefs | **0** | 2 | 2 | 2 |
| SECURITY/SECURITY2 GUID xrefs | 0 | 0 | 0 | 0 |
| `security_policy_install` symbol | absent | absent | absent | absent |
| `init_image_loader` symbol | absent | present | present | present |
| `hook_system_services` symbol | present | present | present | present |

Signature issuers came from `sbverify --list`. Fedora 16.1, openSUSE and
Debian are all dual-signed by "Microsoft Corporation UEFI CA 2011" and
"Microsoft UEFI CA 2023"; Fedora 15.8 carries only the 2011 signature, which
is the one being retired on new hardware.

### 2.1 The GUID scan, and why presence alone is not evidence

`tools/shim-spike/guidscan.py` greps a binary for the little-endian byte
encoding of a GUID and reports the section-relative offset of every hit.

Run plainly, it says every shim contains `EFI_SECURITY_ARCH` and
`EFI_SECURITY2_ARCH`, which would suggest branch A everywhere. That is a false
positive: shim's `lib/guid.c` defines every GUID it knows in one table that is
always compiled in, so the constant sits in `.data` whether or not any code
path touches it.

`guidscan.py --xref` answers the real question by disassembling `.text` and
counting instructions that take the address of each GUID symbol (distro shims
keep their symbol tables). Zero references to `SECURITY_PROTOCOL_GUID` and
`SECURITY2_PROTOCOL_GUID` in all four binaries, and no
`security_policy_install` symbol in any of them: **`OVERRIDE_SECURITY_POLICY`
is not compiled into any shipped shim.** Branch A is unavailable everywhere,
which also matches upstream, where that option is off by default and no
distro turns it on.

### 2.2 Protocol GUIDs, with sources

From `rhboot/shim` `lib/guid.c` at `main` (commit
`72fd4b70e7562d25234204249462da97080ef56f`):

| Name | GUID | Source |
| --- | --- | --- |
| `SHIM_LOCK_GUID` | `605dab50-e046-4300-abb6-3dd810dd8b23` | `lib/guid.c:37` |
| `SHIM_IMAGE_LOADER_GUID` | `1f492041-fadb-4e59-9e57-7cafe73a55ab` | `lib/guid.c:38` |
| `SHIM_LOADED_IMAGE_GUID` | `6e6baeb8-7108-4179-949d-a3493415ec97` | `lib/guid.c:39` |
| `SECURITY_PROTOCOL_GUID` | `a46423e3-4617-49f1-b9ff-d1bfa9115839` | `lib/guid.c:34` |
| `SECURITY2_PROTOCOL_GUID` | `94ab2f58-1438-4ef1-9152-18941a3a0e68` | `lib/guid.c:35` |

Permalink form:
`https://github.com/rhboot/shim/blob/72fd4b70e7562d25234204249462da97080ef56f/lib/guid.c#L34-L39`

The interface behind `SHIM_IMAGE_LOADER_GUID` is `SHIM_IMAGE_LOADER` in
`include/loader-proto.h`: four members, `LoadImage`, `StartImage`, `Exit`,
`UnloadImage`, with the boot-services signatures.


## 3. How shim wires itself up (source reading that the probe then confirmed)

`shim.c` `shim_init()` and `install_shim_protocols()`:

- `SHIM_LOCK` and `SHIM_IMAGE_LOADER` are installed together by one
  `InstallMultipleProtocolInterfaces` call, **unconditionally**, i.e. whether
  or not Secure Boot is on. 15.8 installs only `SHIM_LOCK`.
- `security_policy_install()` is called only when Secure Boot is on **and**
  `OVERRIDE_SECURITY_POLICY` was compiled in. It never is (Section 2.1).
- `hook_system_services(systab)` is called when `secure_mode()` **and** the
  shim carries a vendor cert or vendor dbx. Every shim classified here carries
  a vendor cert (Section 2 table), so the condition is "Secure Boot is on".
  It replaces `gBS->LoadImage` and `gBS->StartImage` in the live boot-services
  table with `shim_load_image` / `shim_start_image`.
- `hook_exit(systab)` is unconditional, which is why `gBS->Exit` points into
  shim even with Secure Boot off.
- The hooks are torn down in `shim_fini()`, which runs only after `init_grub()`
  returns. The second stage therefore sees the hooks in place for its whole
  lifetime.

The 15.8 hook is a different animal and is the reason 15.8 is useless to us.
`replacements.c` `load_image()` in 15.8 unhooks itself, calls the firmware's
`LoadImage`, and re-hooks. It performs no verification of its own, so a
MOK-signed image is refused by the firmware exactly as if shim were not there.
The verifying `shim_load_image` in `loader-proto.c` only arrives in 16.0.


## 4. Runtime probe

`tools/shim-spike/probe.c` is a freestanding mingw PE built with the root
Makefile's flags (copied into `tools/shim-spike/Makefile`; no repo headers are
shared, since `src/efi` is in flight). It is staged under each shim's own
second-stage name and reports the SecureBoot/SetupMode/MokSBState variables,
`LocateProtocol` for the five GUIDs above, the boot-services pointers, and the
result of `gBS->LoadImage` on three staged payloads.

`tools/shim-spike/run-probe.sh VARIANT [plain|secboot]` stages the ESP and runs
QEMU headless with a serial log. `tools/shim-spike/mok-setup.sh` builds the
Secure Boot case.

### 4.1 Protocols and hooks observed

| | plain OVMF, no shim | plain OVMF, any shim | secboot, fedora-15.8 | secboot, 16.1 (all three) |
| --- | --- | --- | --- | --- |
| shim launched the probe | n/a | yes | yes | yes |
| `SecureBoot` variable | unset | unset | 1 | 1 |
| `SetupMode` | unset | unset | 0 | 0 |
| `SHIM_LOCK` | absent | present | present | present |
| `SHIM_IMAGE_LOADER` | absent | 16.1 only | absent | present |
| `SHIM_LOADED_IMAGE` | absent | absent | absent | absent (per-image, not global) |
| Security/Security2 iface | firmware | firmware, unchanged | firmware, unchanged | firmware, unchanged |
| `gBS->LoadImage` hooked | n/a | **no** | yes (non-verifying) | **yes, == SIL->LoadImage** |
| `gBS->StartImage` hooked | n/a | no | yes | yes, == SIL->StartImage |
| `gBS->Exit` hooked | n/a | yes | yes | yes |

Two details worth keeping:

- With Secure Boot **off**, `gBS->LoadImage` is still the firmware's: the
  address matches the no-shim baseline byte for byte. That is correct and
  harmless (with Secure Boot off there is nothing to verify), but it means a
  loader must not assume "shim ran, therefore LoadImage is shim's".
- On 16.1 the probe can decide by itself, because `gBS->LoadImage` is pointer-
  identical to `SHIM_IMAGE_LOADER->LoadImage`. Both are `shim_load_image`.
- The `EFI_SECURITY_ARCH` / `EFI_SECURITY2_ARCH` interface pointers are
  identical in the no-shim baseline and in every shim run, which confirms at
  runtime what Section 2.1 concluded statically: shim never replaces them.
- 15.8 also hooks `gBS->ExitBootServices`; 16.1 does not.

### 4.2 `gBS->LoadImage` acceptance, the test the design rests on

Three payloads staged beside the probe, differing in one property each, loaded
by device path (never started):

| payload | plain OVMF | secboot fedora-15.8 | secboot fedora-16.1 | secboot suse-16.1 | secboot debian-16.1 |
| --- | --- | --- | --- | --- | --- |
| MOK-signed, has `.sbat` | loaded | refused `0x0f` | **loaded** | **loaded** | **loaded** |
| MOK-signed, no `.sbat` | loaded | refused `0x0f` | **loaded** | **loaded** | **loaded** |
| unsigned | loaded | refused `0x0f` | refused `0x1a` | refused `0x1a` | refused `0x1a` |

`0x1a` is `EFI_SECURITY_VIOLATION`, accompanied by shim printing "Verification
failed: Security Policy Violation" on the console. `0x0f` is
`EFI_ACCESS_DENIED` from the firmware: on 15.8 the hook just forwards to the
firmware's `LoadImage`, which knows db only, so even the MOK-signed payload is
refused.

This is precisely the behaviour DESIGN Section 2 wants, obtained with no
loader-side code: Secure Boot on means shim verifies against db + MOK and
refuses what does not verify; Secure Boot off means everything loads.

Caveat on how this was reached: MokManager enrollment is interactive by design,
so `mok-setup.sh` writes the `MokList` variable straight into a copy of the
OVMF varstore with `virt-fw-vars`, and signs the probe with a throwaway key
under `build/shim/keys/`. That shortcut skips the enrollment **UI** only. Shim
still reads `MokList` and still does the verification itself, so what the probe
observes afterwards is the real MOK load path. Nothing about it belongs in the
product.


## 5. Findings that change how we build, not just what we ship

### 5.1 Our loader binary must carry a `.sbat` section

The first Secure Boot attempt failed with `EFI_SECURITY_VIOLATION` for a
correctly MOK-signed probe. The cause is not the signature. `pe.c`
`verify_sbat_section()`:

```
if (SBATBase == NULL || SBATSize == 0) {
        dprint(L"No .sbat section data\n");
        /*
         * SBAT is mandatory for binaries loaded by shim, but optional
         * for binaries loaded outside of shim but verified via the
         * protocol.
         */
        return in_protocol ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}
```

`in_protocol` is set only inside `shim_load_image`. So:

- the image **shim itself launches as its second stage**, which is our loader,
  must have a `.sbat` section or it is refused under Secure Boot;
- images our loader then loads through the hooked `gBS->LoadImage` or through
  `SHIM_IMAGE_LOADER` need not, which the 4.2 table confirms empirically.

Confirmed by controls in both directions: a Fedora-built MokManager signed with
the same throwaway key loaded fine (it has a `.sbat`), and the probe started
working the moment a `.sbat` section was added.

The failure mode is nasty because it is indistinguishable from a bad signature
at the console: same `EFI_SECURITY_VIOLATION`, same dialog. Worth a comment
next to wherever the loader's `.sbat` is defined.

A plain `__attribute__((section(".sbat")))` const array works with the mingw
toolchain: the linker places the section in sequence with `.text`/`.data`, with
`CONTENTS, ALLOC, LOAD, READONLY, DATA`. No post-link `objcopy` is needed, and
in fact `objcopy --add-section` on the linked PE produces a section above
`SizeOfImage` and fails. Content is the SBAT CSV of shim's `SBAT.md`: the
mandatory `sbat,1,...` row first, then one row per component, generation number
in field 2.

Follow-on for Section 8: `mkstick` and the bless tool should stamp an SBAT row
for airies-boot, and the UKIs we build should carry one too. Not required by
shim on that path, but it is what makes a future revocation possible at all.

### 5.2 `sbsign` warns about our PE layout, harmlessly

`sbsign` prints `warning: data remaining[N vs M]: gaps between PE/COFF
sections?` for our mingw output. It prints the same warning for Fedora's own
gnu-efi binaries, and both verify fine under shim. Noise, not a defect.


## 6. The OVMF Secure Boot rig on this machine

Worth recording, because the project's test plan (Section 10) assumes a
Secure Boot rig has to be built.

`/usr/share/edk2/OvmfX64/OVMF_VARS.secboot.fd` ships **fully enrolled**:

- `PK`: Red Hat Secure Boot (PK/KEK key 1)
- `KEK`: Red Hat, Microsoft Corporation KEK CA 2011, Microsoft Corporation
  KEK 2K CA 2023
- `db`: Microsoft Windows Production PCA 2011, Windows UEFI CA 2023, Microsoft
  Corporation UEFI CA 2011, Microsoft UEFI CA 2023, plus 371 sha256 entries
- `dbx`: 17836 bytes
- `SecureBootEnable`: on

So all four shims pass db unmodified, and an unsigned image is rejected by the
firmware with "Access Denied" before it ever runs. Nothing needs to be
enrolled to get a realistic Secure Boot test; only MOK does, and only for our
own MOK-signed artifacts.

Two rig details:

- Both shipped OVMF builds are release builds. The `-debugcon` log
  (`isa-debugcon.iobase=0x402`) comes out empty, so the serial console is the
  only channel. Not worth chasing a debug OVMF build for this.
- `virt-fw-vars` (python3 `virt-firmware`) was the one host tool not already
  installed. `mok-setup.sh` puts it in a venv under `build/shim/venv`, per the
  no-system-pip rule. It is a test-rig dependency only.


## 7. Conclusion against DESIGN Section 7.6

| shim | branch A (Security2 hook) | branch B (image-loader protocol) | branch C (`shim_lock->Verify`) | usable as shipped? |
| --- | --- | --- | --- | --- |
| fedora-15.8 | no | no | yes | **no** for MOK payloads |
| fedora-16.1 | no | **yes** | yes | yes |
| suse-16.1 | no | **yes** | yes | yes |
| debian-16.1 | no | **yes** | yes | yes |

**Branch B is the answer, and it arrives twice over.** On every 16.x shim we
can either call `SHIM_IMAGE_LOADER->LoadImage` explicitly or just call
`gBS->LoadImage`, because under Secure Boot they are the same function. That
second form is worth stating plainly because Section 7.6 does not anticipate
it: the design's phrase "verifying `LoadImage`" can be implemented as a literal
`gBS->LoadImage` call, with no shim-specific code in the loader at all.

Recommended shape for the loader: locate `SHIM_IMAGE_LOADER` once at startup
and prefer it when present; fall back to `gBS->LoadImage`. Preferring the
protocol is not about capability (they are identical) but about intent: it
documents which loader we meant to call, it keeps working if a future shim
stops patching the boot-services table, and it is the interface upstream
maintains. Branch C stays unbuilt, so no PE relocator enters trusted code.

**Which shim to ship: Fedora's `shim-x64` 16.1.** Reasons, in order:

1. Dual-signed by the MS 2011 **and** 2023 UEFI CAs, so it passes db on both
   current machines and on hardware shipping with only the 2023 CA. This rules
   out 15.8 outright, and it is the single most important criterion.
2. Branch B present, verified at runtime on this exact binary.
3. Smallest vendor trust surface of the three: one Fedora CA cert, no vendor
   dbx. Shipping openSUSE's shim additionally trusts the openSUSE CA and
   carries a 7.5 KB vendor dbx we would inherit and have to reason about;
   Debian's carries 5.5 KB.
4. Fedora publishes every build at a stable, versioned Koji URL, which makes
   `mkstick` able to pin and re-fetch a known binary deterministically.
5. Its naming (`grubx64.efi`, `mmx64.efi`, `fbx64.efi`) matches what nearly
   every distro ISO on p2 already uses, so the ESP layout looks unsurprising.

Debian's is an equally capable second choice; the only cost is fetching
MokManager from a second package. openSUSE's is also fully capable, so
Section 4's existing choice is not wrong, just not the best one on criterion 3.

This is a branch-D style decision in Section 7.6's terms: choose the shim by
feature, not by sentiment. Nothing in the loader changes with the choice.


## 8. Corrections DESIGN.md needs (not applied here)

1. **Section 7.6, branch B version floor.** "shim >= 15.7" is wrong. The
   `SHIM_IMAGE_LOADER` protocol is first installed in **16.0**; 15.7 and 15.8
   do not install it (checked at both tags, and confirmed against the 15.8
   binary). 15.8 does contain `loader-proto`-era code internally, which is
   probably where 15.7 came from, but it is not published as a protocol and
   15.8's `gBS->LoadImage` hook does not verify.
2. **Section 7.6 is missing the mechanism that actually applies.** Add the
   in-place `gBS->LoadImage` / `gBS->StartImage` patch (shim's
   `hook_system_services`, active whenever Secure Boot is on and the shim
   carries a vendor cert) as the form branch B takes in practice. Also worth
   recording its precondition, since it is not "shim ran" but "Secure Boot is
   on": with Secure Boot off, `gBS->LoadImage` is the firmware's.
3. **Section 7.6, branch A.** Can be marked settled: no shipped shim compiles
   `OVERRIDE_SECURITY_POLICY`, and none replaces the Security/Security2 arch
   protocols. It is not a branch reality offers.
4. **Section 4, next-stage name.** The instruction to read the name out of the
   binary is correct and should stay. The specific claim is confirmed rather
   than corrected: openSUSE 16.1 still uses `\grub.efi`, `MokManager.efi` and
   `fallback.efi`; Fedora and Debian use `\grubx64.efi`, `mmx64.efi` and
   `fbx64.efi`. If the ship recommendation in Section 7 is taken, Section 4's
   ESP listing needs rewriting for Fedora's names, and its "the SUSE shim" line
   needs to become "the shim chosen in the spike".
5. **New requirement to state somewhere in Sections 4 or 9.** The loader binary
   must carry a `.sbat` section or shim refuses it under Secure Boot. See 5.1.
   This is a build-system requirement, not a design choice, and it is invisible
   until Secure Boot is on.
6. **Section 10 test rig.** Record that the stock
   `OVMF_VARS.secboot.fd` here is already fully enrolled with the Microsoft and
   Red Hat keys, so the Secure Boot rig needs no key setup beyond MOK.


## 9. Still unverified, with suggested procedure

1. **Real MokManager enrollment on real firmware.** Everything above used a
   varstore-injected `MokList`. What is untested is the enrollment UX itself:
   shim launching MokManager on first boot, the user confirming
   `airies-boot.cer`, the reboot, and the enrolled key persisting. Procedure:
   build the ESP with our cert at `p1:/airies-boot.cer`, boot the secboot OVMF
   with an unmodified varstore, drive MokManager interactively (it needs a
   display, so `run-qemu.sh` without `-display none`), enroll, reboot, and
   re-run the probe unmodified. It should reach the same output as 4.1. This is
   step 10 of the build order anyway.
2. **SBAT enforcement against a raw signed bzImage**, which Section 11 step 1
   also asks for and which decides the bless tool's output format. The
   mechanism is now clear (a kernel loaded through `shim_load_image` is
   `in_protocol`, so a missing `.sbat` is tolerated), but "tolerated" was only
   measured on a small PE, not on a real bzImage with an EFI stub. Procedure:
   MOK-sign an unmodified distro `vmlinuz-*` and add it as a fourth payload in
   the probe's LoadImage test.
3. **`StartImage` of a verified payload.** The probe loads and immediately
   unloads; it never starts anything. Starting exercises shim's
   `shim_start_image` and its `Exit` handling, which is a different code path
   and the one that actually boots a UKI.
4. **shim 16.1 revocation plumbing** (`revocations_sbat.efi`,
   `revocations_sku.efi`, `shim_certificate_0.efi` strings appear in the 16.1
   binaries and not in 15.8). Unknown whether any of it constrains a stick that
   carries its own MOK. Worth a look before shipping, not before building.
5. **Real hardware.** Everything here is OVMF. Firmware that implements
   `LoadImage` differently, or that has its own db contents, is the actual
   deployment target.


## 10. Reproducing

```sh
tools/shim-spike/fetch-shims.sh                 # pinned downloads -> build/shim/bins
make -C tools/shim-spike                        # build/shim/probe.efi
make -C tools/shim-spike scan                   # static classification, Section 2
tools/shim-spike/run-probe.sh none plain        # firmware baseline
tools/shim-spike/run-probe.sh fedora-16.1 plain # Secure Boot off
tools/shim-spike/mok-setup.sh                   # throwaway MOK + seeded varstore
TAG=-mok PROBE_BIN=build/shim/probe-mok.efi \
  VARS_TEMPLATE=build/shim/vars-template-mok.fd \
  tools/shim-spike/run-probe.sh fedora-16.1 secboot
```

Serial logs land in `build/shim/log-<variant>-<fw><tag>.txt`. Each run starts
from a fresh copy of the varstore, so no state leaks between runs.
