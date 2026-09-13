# airies-boot Design

A single-purpose, auditable multi-boot loader for UEFI x64. A distilled Ventoy:
drag-and-drop ISO booting, plus a first-class path for booting a personal
LUKS-backed OS via a Unified Kernel Image.

Status: pre-1.0 MVP. This document is the source of truth for architecture and
scope. It is written to be read cold, months from now, by the person who has to
change it.


## 1. Scope

### In scope (MVP)

1. UEFI x64 firmware only. No BIOS/CSM and no IA32, ever (see Non-goals).
2. Drag-and-drop ISO booting from an exFAT partition, Ventoy-style: copy an
   `.iso` onto the stick from any OS, it appears in the menu, it boots. No
   host-side registration step. (When Secure Boot is ON, unsigned media that does
   not ship its own signed chain requires the optional `sign` bless step; see
   Section 7.)
3. Booting a personal OS: a Unified Kernel Image (UKI) sitting on a small
   LUKS2+ext4 partition, whose initramfs then unlocks the real root/home on a
   second LUKS2 partition and pivots into it.
4. Never weaken the machine's Secure Boot posture, chaining through Fedora's
   shim-x64 16.1 (dual MS-signed, 2011 + 2023 CAs; decision 2026-09-12, see
   Section 4 and docs/notes/shim-spike.md): uphold Secure Boot when it is on,
   load freely when it is off. See Section 2 for the posture and Section 7
   for the trust model and coverage matrix.

### Non-goals

Rejected, not deferred (decisions 2026-09-12):

- BIOS / legacy boot. It has no Secure Boot story, it requires a parallel
  real-mode loader stack that shares almost nothing with the EFI code, and
  the hardware is dying out. If it is ever revisited, the recorded approach
  is a minimal BIOS chain that starts a small Linux which then kexecs
  targets - not a port of this loader.
- IA32 (32-bit UEFI). Effectively extinct: it existed on the 2013-2015
  Atom tablet class and is rarer in the wild today than BIOS-only machines.
  Not worth the dual-arch discipline tax on the whole codebase.

Explicitly deferred, not forgotten:
- Windows install media and other ISOs that need Ventoy's runtime driver
  injection to boot. The injection *plumbing* is built in from day one
  (Section 6), but the per-family injectors that would cover these are not.
- Persistence overlays for live ISOs.
- A graphical/framebuffer menu with themes. MVP menu is text via
  Simple Text Output.


## 2. Secure Boot posture (read this first)

**Governing principle: airies-boot must never weaken the machine's Secure Boot
posture.** It behaves according to the firmware's current Secure Boot state:

- **Secure Boot ON:** uphold it. The loader boots only payloads that verify
  against the platform trust chain (db / MOK, through shim). Anything that does
  not verify is refused, exactly as it would be if booted directly.
- **Secure Boot OFF:** there is nothing to uphold, so the loader chainloads
  whatever the user selects.

We do not implement our own signature checking, and we never disable the
platform's. The behavior above is obtained by *deferring to shim* rather than
by adding code:

- We always chainload through a shim-verified load path, and we never install
  a security-protocol override. Exactly which entry point that is (firmware
  `LoadImage` intercepted by shim's security hook, or shim's own image-loader
  protocol) is firmware/shim plumbing resolved by the step-1 spike; see
  Section 7.6. Either way, verification is shim's.
- shim already keys its verification off the firmware `SecureBoot` state: it
  enforces signatures when Secure Boot is on and allows everything when it is
  off. Because every image our loader launches goes through shim's verification
  hook, "uphold SB when on, load anything when off" falls out of the platform
  for free.

We do read the `SecureBoot` (and `SetupMode`) EFI variables ourselves, but only
for UX: to label each menu entry with its verification status and to print an
actionable message when Secure Boot blocks an unsigned selection.

This is a deliberate reversal of an earlier design that overrode verification to
force unsigned media to boot under Secure Boot. That approach reduced security
and has been removed; see Section 7 for the trust model and coverage matrix that
replace it.

The personal-OS path (UKI on LUKS, Section 5) is a separate story: it stays
fully signature-verified end to end, because we build and sign those artifacts
ourselves.


## 3. On-disk layout

GPT, four partitions:

```
p1  ESP        FAT32   ~64 MB   loader + shim + MokManager + our public cert
p2  ISOs       exFAT   bulk     the ISO library (drag-and-drop target) + injection assets
p3  UKI shelf  LUKS2 -> ext4    ~1 GB   personal-OS UKI + signing-appliance UKI + MOK private key, pbkdf2 keyslot
p4  root+home  LUKS2 -> ext4    bulk    real root and home, argon2id keyslot
```

Rationale per partition:

- **p1 ESP.** Standard. Contents in Section 4. FAT32 because firmware only
  guarantees FAT.
- **p2 exFAT.** exFAT so large ISOs (>4 GB) fit and any modern OS
  (Windows/macOS/Linux) can drop files on it. Firmware does not read exFAT, so
  we bring our own read-only exFAT driver (Section 6).
- **p3 UKI shelf, pbkdf2.** LUKS2 here uses a **pbkdf2** keyslot, not the LUKS2
  default argon2id. argon2id is memory-hard by design and brutal in a
  single-threaded EFI environment with no tuned memory allocator. pbkdf2 reduces
  the unlock to SHA-256 iteration, which is cheap. We control p3's creation
  (Section 8), so we mandate this. p3 holds the personal-OS UKI, the
  signing-appliance UKI (Section 7.5), and the MOK private key, all encrypted at
  rest under this one keyslot. Roughly 1 GB. The private key living here (under
  LUKS), never on the readable exFAT partition and never in the loader, is
  central to the key model in Section 7.5.
- **p4 root+home, argon2id.** p4 is unlocked by the *Linux* kernel inside the
  UKI's initramfs, where argon2id is fine, so it keeps the strong default. p4 is
  a separate partition from p3 purely so that EFI-land never has to understand or
  mount the real root filesystem. EFI touches p3 only.

p3 and p4 share the same passphrase (Section 5).

**p3 encryption is the default, not a requirement (decision 2026-09-13).**
mkstick can create p3 plaintext (`--p3-plain`), for users who want the UKI
shelf simple or firmware-readable. Consequences, all of which the tooling
states at creation time rather than letting the user discover:

- No LUKS on p3 means **no on-stick private key**: the signing appliance
  (Section 7.5) cannot exist on that stick, and blessing is external-host
  only. The key model's invariant is unchanged - the private key lives
  encrypted or it does not live on the stick at all.
- The loader skips the passphrase prompt entirely and the credential
  handoff (Section 5.1) is simply not used; the initramfs prompts for p4
  itself. Still a single prompt, just later and owned by the OS.
- Integrity is unaffected: the UKI is signed and shim verifies it off a
  plaintext shelf exactly as off a decrypted one. What is given up is
  confidentiality of the UKI contents (embedded initramfs included) and
  the appliance. A legitimate trade, the user's to make.
- The p3 filesystem choice is orthogonal (see the fs_ops registry,
  Section 9); note that p3's filesystem never meets the personal OS's own
  initramfs - p3 is read by our EFI driver alone, and the initramfs
  unlocks and mounts p4 directly.

**Partition identification (decision 2026-09-13).** The loader must not treat
"any exFAT volume" as p2 or "any LUKS2 volume" as p3: on real hardware the
stick sits next to internal disks that may carry both. mkstick creates every
partition, so identification is by **GPT partition type GUID**: one airies
GUID minted per role (p2 ISO library, p3 UKI shelf; p1 keeps the standard ESP
type, p4 keeps the standard Linux dm-crypt type since only the initramfs
touches it). The install manifest additionally records each partition's
PARTUUID as a tiebreaker if two airies sticks are ever plugged in at once.
The loader matches type GUIDs read from the GPT entry protocol (or by parsing
the GPT itself off the parent BlockIO if the firmware's PartitionInfo is
absent) and falls back to the current probe-everything behavior only when it
finds no airies-typed partition at all, so the QEMU rig images keep working
until mkstick (Section 8) exists to stamp them.


## 4. ESP contents and the Secure Boot chain

```
p1:/EFI/BOOT/BOOTX64.EFI     Fedora shim-x64 16.1 (dual MS-signed, 2011+2023)
p1:/EFI/BOOT/mmx64.efi       MokManager, shim's enrollment UI (Fedora naming)
p1:/EFI/BOOT/grubx64.efi     OUR loader (see note on next-stage name)
p1:/airies-boot.cer          our public cert, for first-boot enrollment
```

Shim vendor decision (2026-09-12, spike data in `docs/notes/shim-spike.md`):
Fedora's shim-x64 16.1 over the earlier SUSE assumption. Rationale: dual
MS signatures (2011 + 2023; the 2011 CA retirement will strand single-signed
shims), the smallest vendor trust surface of the classified candidates (one
Fedora CA, no vendor dbx payload), pinnable Koji package URLs, and >= 16.0
is required anyway for the SHIM_IMAGE_LOADER load path (Section 7.6).

Boot chain under Secure Boot:

1. Firmware verifies `BOOTX64.EFI` (shim) against the platform db. Passes: shim
   is MS-signed.
2. Shim installs its verification protocol and loads its next-stage binary. It
   cannot verify our loader against db, so it consults MOK. On first boot MOK is
   empty for our key, so shim launches MokManager, which prompts the user to
   enroll `airies-boot.cer`. After enrollment and reboot, shim verifies our
   loader against MOK. Passes.
3. Our loader runs. For the ISO path it chainloads payloads without a signature
   requirement (Section 7). For the UKI path it loads only our-signed artifacts,
   which shim verifies honestly (Section 5).

**Next-stage name.** Shim's default next-stage loader filename is compiled
into the shim binary. The spike confirmed Fedora 16.1 uses `grubx64.efi`
(and openSUSE still `grub.efi` / `MokManager.efi`). The install tool
(Section 8) must still read the actual embedded default path out of the
specific shim binary being installed and name our loader to match, rather
than assuming, so a future shim swap stays a data change.


## 5. Personal OS path: UKI on LUKS, with passphrase handoff

Goal: type the passphrase once, boot into the real encrypted OS.

Flow:

1. User selects the personal-OS entry in our menu.
2. Loader prompts for the passphrase.
3. Loader unlocks **p3** (LUKS2, pbkdf2) using our in-EFI LUKS + AES-XTS stack
   (Section 6), and reads the UKI off the ext4 inside.
4. Loader synthesizes a **virtual read-only filesystem** carrying the UKI plus
   a credential file holding the passphrase (Section 5.1), then
   `LoadImage`/`StartImage` the UKI from that filesystem. The UKI is signed by
   our key, so it passes shim verification honestly (via the shim-verified
   load path, Section 7.6). Full verification applies on this path.
5. The UKI's sd-stub packs the credential into the initrd; the initramfs's
   stock `systemd-cryptsetup` consumes it to unlock **p4** (LUKS2, argon2id)
   and pivots into the real root.

p3 and p4 share the passphrase precisely so this handoff is "reuse the string
the user already typed." p4 is separate only so EFI never mounts root.

**As built (2026-09-13, the whole flow above):** discovery, prompt, unlock,
ext4 read, the shim-verified launch, and the credential handoff all work end
to end in the QEMU rig (0.22 s unlock at 100k pbkdf2 iterations; sd-stub
delivered the credential from our synthetic filesystem under Secure Boot ON
with zero initramfs modifications). Notes that survive from the build:

- The UKI is launched **by device path off the synthetic credential
  filesystem** (src/credfs). A `SourceBuffer` load survives only as a
  loudly-warned fallback when the credfs publish fails; it is still
  shim-verified but cannot deliver credentials.
- The credential is named **`cryptsetup.passphrase.cred`** (systemd strips
  the `.cred` suffix to form the credential name; `systemd-cryptsetup`
  hardcodes `cryptsetup.passphrase`, and the generators write
  `ImportCredential=cryptsetup.*` into every generated unit, so a stock
  sd-encrypt initramfs consumes it with no hook). Content is raw passphrase
  bytes, no trailing newline, no NUL.
- What remains unproven by the rig: a full systemd initramfs actually
  unlocking p4 from the credential (the fixture proves delivery, name, and
  bytes; the consumption code path is verified by source reading of
  systemd 261). First real-hardware boot closes that gap.
- **UKI discovery convention (settled):** the loader looks in `/uki/` first,
  then `/EFI/Linux/` (the Boot Loader Specification location, so a p3
  populated by `kernel-install` still boots), taking regular files with a
  case-insensitive `.efi` suffix, sorted by name; one match boots directly,
  several are offered in a post-unlock chooser. mkstick stages to `/uki/`.

### 5.1 The handoff mechanism

The passphrase is entered at runtime, so it cannot be baked into the UKI at
build time. We pass it from our pre-ExitBootServices loader to the initramfs
through **sd-stub's companion-credential pickup**:

- The UKI is built with `ukify` (Section 8), so its stub is systemd-stub. When
  sd-stub is loaded from a filesystem, it scans a `<uki>.efi.extra.d/`
  directory next to the image for `*.cred` files, packs them into a cpio, and
  merges that cpio into the initrd at `/.extra/credentials/`.
- The loader never writes real storage, so the credential is not staged on
  disk. The loader synthesizes a small **virtual read-only filesystem** in RAM
  (the same virtual-device machinery the ISO path already needs, Section 6)
  containing the UKI bytes read off p3 plus `<uki>.efi.extra.d/p4.cred`, and
  calls `LoadImage` with that device path.
- `systemd-cryptsetup` natively consumes credentials (`cryptsetup.passphrase`),
  so the initramfs needs no custom hook, only a systemd-based initramfs
  (mkinitcpio `systemd` hooks). That is a build-time property of the UKI,
  which we build (Section 8).

Lifetime and exposure: the credential exists only in RAM (the virtual
filesystem evaporates at `ExitBootServices`; the cpio copy lives inside the
initramfs), is root-only, and is discarded when the initramfs pivots. It never
touches persistent storage and is never world-readable. If
`LoadImage`/`StartImage` fails, the loader zeroes the credential buffer and
tears down the virtual filesystem before returning to the menu.

All three open questions of this design are now VERIFIED in the QEMU rig
(2026-09-13, systemd 261 sd-stub, shim 16.1): the `.extra.d` naming and
pickup rules behave as documented; sd-stub accepts unauthenticated `.cred`
files under Secure Boot ON (credentials are data, not code); and pickup
works from our synthetic filesystem exactly as from a real ESP. Measured
detail worth keeping: both the firmware and shim loaders size the image via
`SetPosition(max)`/`GetPosition`, never `GetInfo`, and sd-stub enumerates
`.extra.d` through directory `Read` one `EFI_FILE_INFO` record at a time -
a synthetic filesystem must implement all of that surface.

**Rejected: volatile EFI variable.** An earlier design stashed the passphrase
in a volatile (`BOOTSERVICE_ACCESS | RUNTIME_ACCESS`, not `NON_VOLATILE`) EFI
variable, with the initramfs reading it via efivarfs and then deleting it.
This does not work as specified: the UEFI spec makes volatile variables
**read-only after `ExitBootServices`** (a runtime `SetVariable` on them fails,
and deletion is a `SetVariable`), so the wipe is impossible and the passphrase
would sit in the variable store until reboot. Worse, efivarfs exposes
variables as mode-0644 files, making the secret readable by any local user for
the whole session. Do not resurrect this design.

**Fallback kept in reserve** if an sd-stub detail does not cooperate: a
dedicated random keyfile, generated at mkstick time, stored on p3, and
enrolled as an extra p4 keyslot; the loader passes the keyfile (never the
passphrase) through the volatile variable. The wipe is still impossible, but
what leaks is a revocable p4 keyslot key, not the passphrase that also opens
p3 and the MOK key. Strictly worse than the credential route; use only if the
credential route fails.


## 6. ISO path: drag-and-drop mechanism

No host-side registration. The stages:

1. **Scan.** At boot, the exFAT driver enumerates `*.iso` on p2. That listing is
   the menu. Dropping a file on from any OS is the entire "add" workflow.
2. **Present as virtual media.** On selection, register an `EFI_BLOCK_IO`
   (and `EFI_DISK_IO`) device whose reads are served from the ISO file's bytes
   by the exFAT driver. Typed as a CD-ROM (2048-byte blocks).
3. **Let firmware parse it.** `ConnectController` (recursive) on the virtual
   device. The UEFI spec mandates El Torito boot support for CD-ROM media: the
   firmware's *partition* driver recognizes the El Torito boot catalog and
   surfaces the ISO's embedded EFI System Partition image, which the
   firmware's FAT driver then mounts. No ISO9660 *filesystem* driver is
   involved or required, and **we do not write an ISO9660 reader.** Fallback
   kept in pocket for ISOs with a defective El Torito entry: present the
   isohybrid as a 512-byte-sector disk instead and let the firmware's GPT/MBR
   partition path find the same embedded ESP.
4. **Chainload through the verifying `LoadImage`.** `LoadImage` the ISO's
   embedded `\EFI\BOOT\BOOTX64.EFI`, then `StartImage`. We never override or
   disable verification; the call goes through shim as usual (Section 7), so:
   - Secure Boot ON: shim verifies the payload. A distro ISO whose
     `\EFI\BOOT\BOOTX64.EFI` is its own MS-signed shim passes, and that inner
     shim verifies its kernel with its own vendor cert. An unsigned payload is
     refused; the menu entry was already marked unverified and we print why.
   - Secure Boot OFF: shim allows the load, and any ISO boots.

### 6.1 The post-ExitBootServices problem

Stages 2-5 all live in boot services. The virtual CD is a boot-services fiction:
it evaporates the instant the distro kernel calls `ExitBootServices`. The
distro's initramfs then hunts for its squashfs on a CD-ROM that no longer
exists, and drops to an emergency shell. This is the single hardest problem in
the whole project and the reason Ventoy is enormous.

Two handling strategies, and we build the framework for both:

- **Native find-the-ISO (the easy families).** Many distros' stock initramfs can
  locate the ISO *file* on a normal partition given the right cmdline:
  `iso-scan/filename=` (Debian/Ubuntu casper, dracut/Fedora),
  `img_loop=` / `img_dev=` (Arch archiso), `rd.live.image` and friends. For
  these we inject nothing at boot; we just supply the cmdline from a per-family
  template. In the framework this is the degenerate "empty injector" case.
- **Runtime injection (the Ventoy tax).** For families whose initramfs cannot
  find a file-backed ISO, we recreate the ISO as a block device *after*
  ExitBootServices. This means: compute the ISO file's extent map on p2 (the
  list of physical LBA ranges backing the contiguous or fragmented file),
  append a compressed cpio segment to the initrd carrying (a) that extent map,
  (b) an early hook and a small kernel helper that assembles a device-mapper
  target over p2 using those extents and exposes it where the distro expects its
  media, and then let the stock initramfs proceed.

### 6.2 Injection framework (built in MVP, injectors filled in over time)

Because runtime injection is where Ventoy's line count lives, the MVP does not
ship every injector, but it *does* ship the machinery so injectors are pure
data/scripts dropped into a registry, never core surgery:

- **Family detection.** Fingerprint the mounted ISO's ESP / directory layout
  (`/casper`, `/live`, `/arch`, `/LiveOS`, presence of specific bootloaders) to
  classify it into a family.
- **Per-family descriptor.** A registry entry per family carrying: the cmdline
  template, whether it needs injection or just a cmdline, (if injection) the
  hook payload to append, and what the family's stock initramfs can consume:
  whether it includes exFAT support (needed to `iso-scan` a file on p2) and
  device-mapper support (needed by the extent-map hook). We cannot ship kernel
  modules (they must match the running kernel), so these facts gate which
  strategy each family can use and are recorded per family, never assumed.
- **Extent-map computation.** In the exFAT driver, resolve an ISO file to its
  ordered list of physical extents on p2. Needed by any injector; also validates
  the "ISO must be resolvable to raw LBAs" assumption at add-nothing time.
- **initrd cpio append.** Load the ISO's initrd into memory, concatenate our
  cpio segment, pass the combined image to the kernel. Standard early-initramfs
  override mechanism.
- **Assets on p2.** Injection payloads (hook scripts, the helper module blobs)
  live on p2 under a reserved directory so they travel with the stick and can be
  updated without reflashing.

MVP ships: family detection, the descriptor registry, extent-map computation,
the cpio-append path, and native-cmdline injectors for the tested families
(Debian/Ubuntu, Fedora, Arch, SystemRescue; memtest needs no ISO backing at
all). Full runtime-injection payloads for the hard families are stubs behind the
same interface.


## 7. Security model

Governing principle (Section 2): airies-boot never weakens the machine's Secure
Boot posture, and it never holds or uses a signing secret. All control over what
is trusted rests with the user, exercised on a host they control or through the
platform's own physical-presence mechanisms. It rests neither with us (the loader
ships no way to relax enforcement) nor with an attacker (no secret is reachable
from removable media, and no enforcement change is reachable without physical
presence).

Two invariants make this concrete and must never be violated:

1. **The loader verifies; it never signs.** Verification of anything the loader
   launches is performed by shim (Section 7.1). The loader never performs a
   signing operation and never holds a private key. Signing is a private-key
   operation that happens only in Linux userland (Section 7.5).
2. **Enforcement is the platform's, never the loader's.** The loader never
   changes what is allowed to run. It reads Secure Boot state only to inform the
   user. Every relaxation of enforcement is a platform mechanism the user invokes
   with physical presence (Section 7.4).

We do not implement our own verification and we do not disable the platform's.
We launch everything through a shim-verified load path (Section 7.6); shim
enforces or permits according to the firmware `SecureBoot` state. The behavior
we want falls out of deferring to shim, not from adding code.

### 7.1 Trust chain

- **Loader integrity.** firmware -> shim -> our loader. The firmware will not run
  a tampered or substituted loader; ours must be MOK-signed. Honest and intact
  regardless of what we boot next.
- **ISO path.** The loader launches the ISO's payload with the normal verifying
  `LoadImage`:
  - Secure Boot ON -> shim verifies the payload against db / MOK / shim's vendor
    cert / SBAT. It boots only if it verifies. Unsigned payloads are refused.
  - Secure Boot OFF -> shim permits everything, the payload boots.
- **UKI path.** shim -> our loader -> our-signed UKI (personal OS and the signing
  appliance, Section 7.5), verified end to end because we build and sign them.
  Under Secure Boot ON this is enforced; the UKI must carry our signature.
  MOK-signed images are not verifiable by the firmware's own `LoadImage`
  against db alone, so this path depends on the shim-verified load path
  resolved in Section 7.6.

### 7.2 What boots, by Secure Boot state (coverage)

This is not a limit we impose; it is what Secure Boot means. An unsigned payload
does not boot under Secure Boot, whether launched by us or directly.

- **Secure Boot OFF:** every templated ISO drag-and-drops and boots. Nothing to
  uphold, nothing to sign.
- **Secure Boot ON, validation on (default):**
  - *Distro ISOs shipping their own MS-signed shim* (Ubuntu, Fedora, Debian,
    recent Arch, SystemRescue, ...): boot with no preparation, verified.
    Their `\EFI\BOOT\BOOTX64.EFI` is an MS-signed shim, so it passes db; that
    inner shim verifies its own kernel with its embedded vendor cert. Real
    end-to-end Secure Boot, still drag-and-drop. Be blunt about the limit:
    because the distro's own bootloader runs its own `grub.cfg`, we do not
    insert `iso-scan` or inject the initrd on this route, and stock ISO
    configs carry no such argument (they are written for real CD/USB media,
    which no longer exists after `ExitBootServices`). Expect this route to
    verify and launch the kernel and then usually *fail* to find its squashfs
    and drop to an emergency shell (Section 6.1). Under Secure Boot ON,
    blessing is the reliable route to a live desktop; this route is "verified
    launch, best-effort media," and the Manual must not oversell it. (A
    gray-area future idea, deliberately not designed here: overlay a modified
    `grub.cfg` in the bytes the virtual CD serves. Config files are unverified
    data even under Secure Boot, but injecting cmdline into another vendor's
    trust chain deserves its own design discussion before anyone builds it.)
  - *User-blessed media:* the reliable route. The bless step (Section 7.5)
    extracts the ISO's kernel and signs it with the MOK key, so the loader boots
    that signed kernel directly with our `iso-scan`-style cmdline and initrd
    injection, media-finding included. Works for signed and unsigned ISOs alike.
    Blessing runs either on an external host the user controls or on the on-stick
    signing appliance; both keep the private key encrypted and out of the loader.
  - *Unsigned, unblessed media* (memtest, older/niche rescue ISOs): refused, with
    an actionable message pointing at the two doors: bless it, or use the
    permissive opt-in (Section 7.4).
- **Secure Boot ON, validation off (user opt-in, Section 7.4):** shim permits
  everything, so every ISO drag-and-drops with nothing signed, on that machine.

The only combination that cannot exist is "drag-and-drop an arbitrary *unsigned*
ISO with reliable media-finding *and* keep Secure Boot validation on," because
that requires either our signature on the kernel (blessing) or the distro doing
its own media-finding. Everything else has a clean home above.

### 7.3 Discipline (what must stay true)

- No mechanism that relaxes signature enforcement exists anywhere in the loader.
  If one is ever proposed, it is a design regression: it would place an
  enforcement change inside a trusted, MOK-signed binary reachable without
  physical presence, which is exactly the class of thing SBAT revocation exists
  to remove, and it would move control off the user.
- The loader never signs and never holds a private key (Section 7.5).
- The loader reads `SecureBoot` / `SetupMode` / `MokSBState` only to drive UX
  (per-entry verification labels, actionable messages, a state banner), never to
  change what is allowed to run. Enforcement is shim's job.
- The Manual documents each Secure Boot state, how to bless media, and how to use
  the platform opt-out.

### 7.4 User opt-out and permissive mode

A user who wants the classic "boot anything" experience can have it, through
mechanisms the platform already provides. We do not build any of them; we inherit
them for free by deferring to shim, and we make them discoverable.

Two sanctioned mechanisms, both gated by physical presence:

- **Firmware Secure Boot off.** The user disables Secure Boot in firmware setup.
  shim then permits everything; the loader boots any ISO.
- **shim `MokSBState` (`mokutil --disable-validation`).** Confirmed at the
  MokManager screen with a one-time password and physical presence on the next
  boot. This tells shim to stop validating even while firmware Secure Boot stays
  on. After that, the loader boots any ISO, with nothing signed. This is the
  frictionless classic-mode opt-in: one deliberate action, no per-ISO signing.

Why this is a platform opt-out and not an enforcement mechanism we own: the
relaxation is performed by shim, the platform's own maintained component, only
after a human confirms it at the MokManager screen. No attacker without physical
presence can trigger it, and our loader is unchanged either way. This is the line
between a platform opt-out (physical-presence gated, sanctioned) and a relaxation
baked into a signed binary (reachable without physical presence, the thing we
refuse to build).

Properties the user should know, and the Manual must state:

- `MokSBState` is stored in that machine's firmware NVRAM. It is **per-machine**,
  does not travel with the stick, and must be set on each machine where the user
  wants permissive mode. A stick cannot carry "disable everyone's Secure Boot."
- With validation off, that machine runs *all* unsigned EFI code, not only ours.
  It is a machine-level choice, and the loader shows a clear banner when it
  detects the state so it is never a surprise.

What we build here is UX only: detect `SecureBoot` / `MokSBState`, show the
current mode, and when an unsigned selection is refused under enforcement, guide
the user to the two doors (bless, or opt in via the platform). The host tool can
run `mokutil --disable-validation` for the user; whether the loader itself can
request the MokManager prompt by setting the shim request variable is a
convenience to verify, with the host-tool path as the certain fallback.

We ship no in-loader enforcement toggle. Rejected for the reasons in 7.3.

### 7.5 Key model and signing

The whole model rests on an asymmetry:

- The **public cert** travels on the stick (p1) and is what gets enrolled in MOK
  and what shim uses to verify. Public; leaking it does nothing.
- The **private key** stays encrypted on p3 under LUKS. It is never on the
  readable exFAT partition, and it is never in the loader.

**Why the loader never signs.** Signing an ISO's kernel is a private-key
operation, so it requires the private key wherever it runs. Loader integrity
(which Secure Boot provides) is code integrity, not key confidentiality: a
verified, untampered loader still cannot safely hold a signing key, because the
threat is not a tampered loader but offline reading of the key from removable
media, and a key inside the loader would make the loader a passphrase-gated
signing oracle. Both are rejected. The private key stays where a private key
belongs: encrypted at rest, used only in an authenticated Linux userland.

**Where signing happens.** Two userland routes, both keeping the key encrypted at
rest and out of the loader:

1. **External host** the user controls, running the `sign` tool (Section 8). The
   key can additionally sit behind a passphrase or a hardware token there. Right
   for a portable signed toolkit built and maintained from a home machine.
2. **On-stick signing appliance.** A minimal UKI (busybox + cryptsetup + sbsign +
   the portable `sign` tool), built and signed by us so it boots under full
   Secure Boot, with no network and no purpose beyond signing. It is a boot entry
   in the loader menu. On selection it:
   - boots (our-signed UKI, passes Secure Boot),
   - unlocks p3 with the passphrase and reads the MOK private key,
   - lists the pending unsigned ISOs and requires **per-ISO confirmation** (the
     explicit "I vouch for this" act, which also stops a dropped-in malicious ISO
     from being blessed silently),
   - for each confirmed ISO, extracts the kernel, wraps and signs it as a
     minimal UKI with the derived `iso-scan` cmdline embedded, and stages it
     plus its manifest record per Section 8.1,
   - reboots to the loader.

   The appliance makes the stick self-hosting for signing: a portable signed
   toolkit can be extended from the stick itself, with no external box. Because
   the appliance is network-less and single-purpose, the window in which the key
   is decrypted is small and hardened, smaller than booting a full OS to sign.

The `sign` logic is identical in both routes; build it as a portable static
binary that runs both on a host and inside the appliance initramfs, so there is
one implementation.

**Residuals** (acceptable for a personal toolkit, stated for honesty):

- Blessing requires a reboot cycle (drop, sign, reboot, boot). The appliance
  makes the signing leg fast, and the permissive opt-in (7.4) covers the "boot it
  right now without signing" case.
- Concentration: the key on p3 shares the passphrase with the OS. A passphrase
  compromise yields the OS and the key together, but an attacker who has the
  passphrase already controls the OS, so the key is not the weakest link. A
  second passphrase or a hardware token for the key is normal host hygiene the
  user can add; it is not something the loader manages.
- On the bless path the kernel is our-signed, but the initrd it loads from p2
  is not signature-verified (Linux does not verify initrds; virtually every
  distro's normal boot has the same property). An attacker with write access
  to the stick can swap the initrd and get code execution in that boot. This
  matches the status quo of ordinary distro boots; stated for honesty.

### 7.6 Load-path plumbing (verified by the 2026-09-12 spike)

The firmware's native `LoadImage` verifies against db/dbx only; it knows
nothing about MOK. Our loader itself is verified *by shim*, so it is fine
regardless; but our UKIs and blessed kernels are MOK-signed images loaded *by
us*, and they pass a `LoadImage`-style load only if shim provides the
verification. The branches below were the candidate mechanisms; the spike
(full data in `docs/notes/shim-spike.md`, measured on Fedora shim 15.8 and
16.1, openSUSE 16.1, Debian 16.1) settled which ones exist:

- **Branch A: shim hooks the Security/Security2 architectural protocols**
  (`OVERRIDE_SECURITY_POLICY`, a shim compile-time option). **Measured: no
  current distro shim compiles this in** (confirmed by disassembly xref count
  and a runtime probe on all four binaries). Dead in practice.
- **Branch B: shim's image-loader protocol** (`SHIM_IMAGE_LOADER`, first
  ships in shim **16.0**, not 15.7 as this section previously guessed). Shim
  installs a `LoadImage`/`StartImage`-alike that verifies against
  db + MOK + SBAT and performs the PE loading itself. **Measured: present
  and working on Fedora/openSUSE/Debian 16.1; MOK-signed images load, unsigned
  images are refused with `EFI_SECURITY_VIOLATION`, everything loads with SB
  off.** This is the chosen branch.
- **The mechanism the branches missed, found by the spike:** under Secure
  Boot, shim >= 16.0 also **patches `gBS->LoadImage`/`StartImage` in the
  system table in place** (`hook_system_services`); the patched pointers are
  identical to the protocol's. So on shim 16.x a plain `gBS->LoadImage` IS
  the MOK-verifying path, and with SB off the pointers are the firmware's
  own. On 15.8 the hook merely forwards to firmware (MOK never consulted),
  which is why the minimum shim version we ship is 16.0.
- **Branch C (last resort, still unbuilt, requires amending this document):**
  `shim_lock->Verify` plus our own PE loader. Not needed.
- **Branch D: vendor choice is free.** Any MS-signed shim passes db on any
  machine; choose by feature, not sentiment.

Loader shape settled by the spike: locate `SHIM_IMAGE_LOADER` at startup and
prefer it; fall back to `gBS->LoadImage` (which shim has patched under SB on
16.x, and which is the firmware's honest loader otherwise). The invariants
are untouched either way: verification is shim's, enforcement is the
platform's, and the loader neither relaxes nor signs. The phrase "verifying
`LoadImage`" elsewhere in this document means this load path.

Two operational facts from the spike that gate other sections: our loader
binary MUST carry a `.sbat` section or shim 16.x refuses it with the same
`EFI_SECURITY_VIOLATION` a bad signature gives (Section 9 has the build
mechanics); and images loaded *through* the hook are exempt from that
section requirement (measured both ways), so blessed bare kernels are not
SBAT-blocked. Still unverified: real MokManager enrollment on hardware, a
signed bzImage end to end, and `StartImage` semantics of the protocol;
procedures are in `docs/notes/shim-spike.md` Section 9.

Note the ISO path does not depend on any of this: a distro ISO's inner shim
is MS-signed and passes db through the firmware's own `LoadImage` natively.


## 8. Host tooling (userland, C)

EFI-land stays freestanding and lean. The device-configuration tooling is
ordinary Linux userland C and is allowed to shell out to system tools rather
than reimplementing cryptography or filesystem creation:

- `airies-boot-mkstick` (or subcommands): partition (GPT), format p1 FAT32 and
  p2 exFAT (`mkfs.fat`, `mkfs.exfat`), and create p3/p4 via `cryptsetup
  luksFormat` with the correct KDF per partition (`--pbkdf pbkdf2` on p3,
  argon2id default on p4), then `mkfs.ext4` inside each. On p3, pin
  `--pbkdf-force-iterations` to a value budgeted for our in-EFI PBKDF2
  implementation: cryptsetup's host-side benchmark would pick around 2M
  iterations, which is seconds of unlock time in unoptimized freestanding C.
  **Measured and decided (2026-09-13): mkstick pins `--pbkdf-force-iterations
  200000`.** The in-EFI implementation completes the whole unlock-to-mount at
  100k iterations in 0.22 s under OVMF/KVM, so 200k lands near half a second
  with margin for slower firmware clocks, while cryptsetup's 2M default would
  cost 3-6 s per unlock. Revisit only if real-hardware timing disagrees.
- Install shim + MokManager + our loader + cert onto p1, naming our loader to
  match the shim's embedded next-stage path (Section 4).
- Generate the MOK keypair; keep the private key encrypted on p3 (Section 7.5),
  export the public cert to p1 for enrollment.
- Sign our loader, the personal-OS UKI, and the signing-appliance UKI with the
  MOK key via `sbsign`.
- Build/stage the personal-OS UKI (via `ukify` or equivalent) with our initramfs
  handoff hook (Section 5.1) baked in, and build the signing-appliance UKI
  (Section 7.5): a minimal initramfs of busybox + cryptsetup + sbsign + the
  portable `sign` tool.
- `airies-boot sign <iso>`: the bless step (Section 7.5). Extracts the ISO's
  kernel and wraps it, per Section 8.1, into a minimal signed UKI whose
  embedded cmdline carries the derived `iso-scan`-style arguments; the ISO
  stays on p2 for its initramfs to find. Built as a **portable static
  binary** so the identical implementation runs both on an external host and
  inside the on-stick signing appliance. Not needed when Secure Boot is off,
  and not needed for ISOs that already ship their own MS-signed shim and
  find their own media.

### 8.1 Blessed artifacts and the manifest (decided 2026-09-13)

The bless output is always a **signed UKI wrap** (sd-stub + kernel +
embedded `.cmdline` + **embedded `.initrd` by default**, `.sbat` inherited
from the stub), never a bare signed bzImage. Embedding the ISO's initramfs
costs p2 space (~100-250 MB per blessed ISO) but makes the entire blessed
boot chain signed, which closes the unsigned-initrd residual of Section 7.5
for blessed media outright; space is the cheapest resource on the stick.
`--no-initrd` produces the kernel+cmdline-only wrap for space-constrained
sticks, reopening that residual for the ISO's initrd read from p2 at boot
(delivery mechanism for that thinner shape - likely sd-stub companion
pickup via the Section 5.1 machinery - is an unspiked question; do not
build it without verifying sd-stub's actual companion-initrd support).
Two more reasons the wrap is a UKI, one forced and one earned:

- Forced: the shim load path may enforce SBAT on everything it loads, and
  raw bzImages carry no `.sbat` (Section 7.6 spike).
- Earned: p2 is writable, unverified exFAT, so anything not covered by a
  signature is attacker-editable with stick access. Embedding the cmdline in
  the signed wrap means tampering with p2 cannot inject kernel arguments
  into a blessed boot; the kernel + cmdline verify or the boot refuses.
  (The initrd read off p2 remains unverified; that residual is recorded in
  Section 7.5 and matches ordinary distro boots.)

On-stick layout, under the reserved directory of Section 6.2:

```
p2:/airies/blessed/<name>.uki      signed UKI wraps, one per blessed ISO
p2:/airies/blessed/manifest        the manifest, plain ASCII text
```

Manifest format: line-based records, `#` comments, one block per blessed
ISO, keys before the next `iso=` belong to the current block:

```
iso=Linux/archlinux-2026.01.01-x86_64.iso   # path relative to p2 root
size=1538670592                              # byte size at bless time
sha256=<hex>                                 # ISO hash at bless time
uki=archlinux-2026.01.01-x86_64.uki          # file under /airies/blessed/
family=arch                                  # registry id (Section 6.2)
```

Loader behavior: a menu entry whose path matches a manifest record *and*
whose current file size equals `size=` is labeled blessed and boots the
named UKI (served to LoadImage through the synthetic-filesystem machinery
of Section 5.1, since exFAT has no firmware device path; shim verifies as
always). A size mismatch means the ISO changed after blessing: the entry is
labeled stale-blessed and boots as if unblessed. The `sha256=` field is
authoritative for the *bless tool* (re-bless detection and appliance
listing); the loader deliberately checks only the size, because hashing a
multi-GB ISO at menu time is not a boot loader's job, and the security of
the boot does not rest on the manifest at all: everything the manifest can
influence is either signed (the UKI) or already-unverified data an attacker
could edit directly (the manifest itself, the initrd). Corrupt or absent
manifest degrades to "nothing is blessed," never to an error.

Shelling out to `cryptsetup`, `sbsign`, `mkfs.*`, `ukify` is the intended
vendoring line: reimplementing argon2id, Authenticode PE signing, or ext4
creation in-repo would be madness for a config tool that runs on a full Linux
box. The `sign` tool shells out to `sbsign`; it never reimplements signing, in
either the host or appliance route.


## 9. EFI application internals

- **Language/ABI:** freestanding C11, MS x64 ABI, built to PE32+ directly with
  `x86_64-w64-mingw32-gcc`. No gnu-efi, no EDK2. We carry our own minimal UEFI
  headers (the protocols and GUIDs we actually use) under
  `src/efi/`. This avoids gnu-efi's ELF-then-objcopy dance and its
  `uefi_call_wrapper` cruft, which exist to solve problems mingw solved.
- **Build:** Makefile-based. Full strict warning set per repo C standard, adapted
  for freestanding (`-ffreestanding -fno-stack-protector` where the environment
  forces it, `-mno-red-zone`, `-fshort-wchar`). Build variants under `build/`.
- **SBAT:** shim refuses second-stage binaries without a valid `.sbat`
  section (confirmed empirically on 16.1; the refusal is the same opaque
  `EFI_SECURITY_VIOLATION` a bad signature gives). Our loader carries one.
  Build mechanics, measured: a `__attribute__((section(".sbat")))` const
  array in the mingw build works; `objcopy --add-section` post-link does
  NOT produce a section shim accepts. Decide our SBAT component name and
  generation policy once and record them here; bumping the generation is
  the revocation lever. UKIs get `.sbat` from sd-stub via `ukify` for free;
  images loaded through shim's hook (blessed kernels) are exempt from the
  requirement.
- **Crypto in EFI:** LUKS2 unlock needs pbkdf2 and AES-XTS-512 sector
  decryption. AES-NI intrinsics for XTS, with a portable C fallback selected
  by CPUID at runtime. Keep the primitives small and self-contained; this is
  the one place in EFI-land where we write crypto, and it is deliberately
  the minimum: SHA-256 and SHA-512, HMAC and PBKDF2 parameterized over both,
  AES-XTS decrypt. SHA-512 earns its ~150 lines because a LUKS2 header names
  its hash in four independent places and `cryptsetup --hash sha512` (which
  the user's own p4 header used for its digest) switches all four; a loader
  that cannot hash cannot unlock, and the parameterization costs less than
  the support burden of a "sha256-only" constraint on mkstick.
- **Filesystem drivers in EFI:** read-only, always; the stick is configured
  from Linux userland, never written by the loader. Drivers sit behind a
  common vtable (decision 2026-09-13): `fs_ops {name, probe, mount,
  dir_open_root, dir_open, dir_next, file_open, file_read, extents}` over
  the shared byte-read callback, registered in a static table. `extents` is
  an **optional capability**: the injection framework needs file-to-LBA
  resolution on p2, and a driver that cannot provide it (btrfs CoW and
  compression can make "the file's raw LBAs" not exist) still serves the
  menu and boot paths, with the degradation surfaced honestly rather than
  failed weirdly. Consumers select a driver through the registry (role GUID
  hint first, probe order second), never by naming a driver directly, so a
  new filesystem is one driver plus one registry row.
  **Sanctioned driver targets** (write on demand, not ahead of it): exFAT
  and ext4 (implemented), FAT32, xfs, btrfs. Per-target caveats recorded
  now: FAT32 on p2 cannot hold >4 GB ISOs; btrfs likely ships without
  `extents`. A new driver's checklist: EFI-free over the byte-read
  callback, host-testable, constrained feature set matched to what its
  mkfs actually emits (the ext4 driver's approach), cmocka suite with a
  fixture image, and zero warnings under the repo flag set.
- **Device discovery (decision 2026-09-13):** ONE pass, one inventory,
  every consumer reads it. A `discover` module enumerates the Block I/O
  handles once at startup and classifies each: media present, role by GPT
  type GUID (Section 3), LUKS2 magic, filesystem identification through
  the fs_ops registry. The ISO scan, the personal-OS scan, and the
  blessed-boot path all consume the same inventory rows rather than
  running their own enumeration loops, so a device cannot be discovered
  for one purpose and missed for another; classification is eager (cheap
  per-handle reads), mounting stays lazy per consumer. Re-scan is the
  same single entry point run again.
- **Modules:**
  - `menu` - text menu over Simple Text Output/Input. Lists ISOs, the personal-OS
    entry, and the signing-appliance entry (Section 7.5); shows per-entry
    verification labels and the Secure Boot state banner.
  - `exfat` - read-only exFAT, file read + directory enumeration + extent map.
  - `ext4` - read-only ext4 (p3 only; constrained mkfs feature set at install).
  - `luks` - LUKS2 header parse, pbkdf2, AES-XTS decrypt. Unlocks p3 only; the
    module reads the UKI bytes off p3 and never exposes the p3 private key (that
    file is read by the signing appliance in userland, not by the loader).
  - `iso` - virtual BlockIO/DiskIO over an exFAT-backed ISO file; family detect;
    injection framework.
  - `sb` - reads `SecureBoot` / `SetupMode` / `MokSBState` for UX only (per-entry
    verification labels, refusal messages, state banner). No enforcement logic
    and no signing; enforcement is shim's, signing is userland's (Section 7).
  - `loader` - shim-verified load glue (Section 7.6; never relaxes
    verification and never signs, see Section 7), cmdline construction, and
    the virtual credential-filesystem handoff (Section 5.1).


## 10. Test rig

- QEMU + OVMF. Both plain (`/usr/share/edk2/OvmfX64/OVMF_CODE.fd`) and Secure
  Boot (`OVMF_CODE.secboot.fd`) builds are present on this machine. The
  shipped `OVMF_VARS.secboot.fd` is already fully enrolled (MS 2011+2023
  db/KEK, Red Hat PK, populated dbx), so the SB rig needs no key setup
  beyond MOK; `virt-fw-vars` can inject `MokList` into a varstore copy to
  test MOK verification without the interactive MokManager UI (procedure in
  `docs/notes/shim-spike.md`). Both OVMF builds are release builds: the
  `-debugcon` log stays empty, do not chase that.
- Buildable disk image with the four-partition layout, populated by the host
  tool, booted under QEMU with a writable copy of OVMF_VARS for MOK enrollment
  testing.
- Real hardware last, for the shim/MokManager enrollment flow, which is the
  least QEMU-friendly step.


## 11. MVP build order

Status (2026-09-13): steps 1-6 are DONE and proven in the QEMU rig, including
the Secure Boot ON halves (shim plumbing spike results in Section 7.6 and
docs/notes/shim-spike.md; personal-OS unlock, shim-verified UKI launch, and
the sd-stub credential handoff in Sections 5/5.1). Partition identification
per Section 3 is implemented (airies GPT type GUIDs, probe-everything
fallback for unstamped rigs). Step 4's injectors are the framework plus
native-cmdline descriptors, per the plan; nothing consumes the cmdline
templates until the bless path lands. Step 7 is also done: the
airies-boot-mkstick and airies-boot-sign host tools exist, and a
mkstick-created stick boots Secure Boot ON end to end with its own generated
MOK. Next: the loader-side blessed-boot path (consume src/manifest per
Section 8.1), then step 8 (signing appliance), step 9 (permissive-mode UX),
step 10 (real hardware).

1. OVMF hello-world: mingw PE loader that prints and reads a key. Proves
   toolchain + test rig. Then the **shim plumbing spike**, which gates several
   designs (Sections 5.1, 7.6, 8, 9):
   - classify the shim binary we intend to ship: version, Security2 hook
     present, image-loader protocol present (GUID/strings inspection first,
     it is cheap and static);
   - under OVMF secboot, attempt to load a throwaway MOK-signed binary and
     pick the Section 7.6 branch;
   - check whether the chosen path enforces SBAT on a raw signed bzImage
     (decides the bless tool's output format, Section 8);
   - verify sd-stub `.cred` pickup rules and that pickup works from a
     synthetic filesystem (Section 5.1);
   - confirm for the record that volatile EFI variables are read-only after
     `ExitBootServices` (Section 5.1's rejection rationale).
2. Text menu + read-only exFAT: enumerate `*.iso` on p2 and list them.
3. ISO boot: virtual BlockIO + ConnectController + verifying `LoadImage` of the
   embedded bootloader (never relaxes verification). Prove one family end to end
   first with Secure Boot OFF (Arch is a good first target: archiso finds its
   media natively). Then prove the Secure Boot ON case boots a shim-carrying
   distro ISO (e.g. Fedora) and refuses an unsigned one with a clear message.
4. SB-state UX (`sb` module) + injection framework: family detection, descriptor
   registry, extent map, cpio-append. Native-cmdline injectors for the tested
   families; per-entry verification labels; Secure Boot state banner.
5. UKI path: read-only ext4 + LUKS2/pbkdf2/AES-XTS unlock of p3 + LoadImage the
   personal-OS UKI.
6. Passphrase handoff: virtual credential filesystem synthesized by the
   loader, sd-stub `.cred` pickup, stock `systemd-cryptsetup` consumes the
   credential, p4 unlock (Section 5.1).
7. Host tooling: `mkstick`, MOK keygen (private key onto p3, cert onto p1), shim
   install with correct next-stage naming, signing our loader/UKIs, and the
   portable `airies-boot sign` tool for blessing media.
8. Signing appliance: minimal signing-appliance UKI (busybox + cryptsetup +
   sbsign + the portable `sign` tool), as a loader menu entry; unlock p3, per-ISO
   confirm, sign, stage manifest, reboot (Section 7.5).
9. Permissive-mode UX: detect `MokSBState`, banner, and guidance to the platform
   opt-out (Section 7.4).
10. Real-hardware shim/MOK enrollment.
