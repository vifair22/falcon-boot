# Argon2id keyslots and a TPM-backed TOTP check

Status: 2026-09-28. Two pieces of one work stream, in this order: Argon2id
first (it unblocks station installs), then TOTP. Both are built. Section 1
is in every build and rig-proven under both loaders; the stick's p3 is
argon2id at 32 MiB, time 4, 4 lanes (open item 1). Section 2 is built for the
UEFI station and rig-proven under swtpm (2.0 and 1.2, Secure Boot off and
on); docs/DESIGN.md Section 13.15 is now its record, and where the two
differ the design section wins. Two things changed on the way, both in
13.15: enrollment seals to PCR values replayed from the firmware's event
log up to the station's own measurement, not to the PCRs the OS reads
(which have moved on by then); and a 1.2 SRK whose secret is SHA-1 of the
zero bytes (swtpm_setup's --srk-well-known) is found at enrollment and
recorded as `srkauth = sha1-zero`.

## 1. Argon2id

### Why

falcon unlocks LUKS2 with pbkdf2 keyslots only. `luks.c` recognises an
argon2id keyslot and refuses it (`LUKS_ERR_UNSUPPORTED_KDF`), and DESIGN
Section 3 pins the stick's p3 to pbkdf2 for that reason ("single-threaded
EFI environment with no tuned memory allocator"). A station's shelf is made
by `cryptsetup` on the machine, whose default is argon2id, so a shelf
formatted the ordinary way, or re-keyed later, locks falcon out; `mkstick
status` already says "keyslot 1: argon2id ... the loader skips it". Keeping a
pbkdf2 slot beside an argon2id one is no answer: an attacker takes the weaker
slot, which throws away what argon2id buys.

### What

- `falcon-core/src/crypto/`: BLAKE2b (RFC 7693) and Argon2id (RFC 9106,
  version 0x13), freestanding, no allocation: the caller hands over the
  memory block. Lanes are filled one after another (no threads in a loader);
  the result is the same as a parallel fill, only slower.
- `falcon-core/src/luks/`: an argon2id keyslot is unlocked with its header's
  `time`, `memory` (KiB) and `cpus` (lanes). argon2i stays refused (LUKS2
  offers it, nothing here makes it).
- Both loaders. The code is core; what differs is where the memory comes
  from: the UEFI loader asks the firmware (`AllocatePages`), falcon-bios its
  heap (`mem/heap.c`) over E820. A slot that asks for more than the machine
  can give is refused with a page that says so ("this keyslot needs N MiB;
  this machine has M"), never a hang.
- A machine too slow or too small for any sensible cost uses a plaintext
  shelf, which both stations already support. The costs below only have to
  work for machines worth encrypting on.

### Costs, and who sets them

- The stick: one fixed, conservative cost that every machine it boots can
  meet, falcon-bios on a 512 MB Pentium III included. Numbers to be set by
  measurement (open item 1). mkstick create writes p3 argon2id at that cost;
  the DESIGN Section 3 rule becomes "argon2id at the stick's fixed cost".
- A station: sized from the box running the install (cryptsetup-benchmark
  style: memory, then time to a target), except where the install is for
  another machine, which `--no-scan` already means (the A21m's install ran on
  the desktop with its disk on a USB bridge; the desktop's numbers would not
  have fitted it). There mkstick takes a conservative default unless given
  `--kdf-memory` / `--kdf-time`, and says which it chose. `station status`
  shows the slot's cost so a mismatch is visible.
- mkstick creates and re-keys shelves with these costs itself; a shelf made
  by hand with `cryptsetup` is opened at whatever it asks for, within the
  limits above.

### As built (2026-09-28)

- Both loaders unlock argon2id and pbkdf2 (dd195ce). A slot whose memory
  cannot be had is refused on the route's failure page, "unlocking the
  encrypted partition", with the advice "this keyslot needs N MiB of
  memory; this machine has M MiB free." M is the largest free conventional
  run in the UEFI memory map, and on falcon-bios the largest usable E820 run
  between 64 MB and 4 GiB, which is what the heap can take.
- The stick: mkstick create writes p3 argon2id at 32 MiB, time 4, 4 lanes
  (`MKS_STICK_KDF_*` in `falcon-cli/src/mkstick/mkstick.h`, the one place;
  the rig stick reads it from there). Open item 1 has the numbers.
- `mkstick status` gives each p3 keyslot's KDF and cost, calls only argon2i
  and unknown KDFs unusable, and warns when an argon2id slot asks for more
  than the stick's 32 MiB.
- Stations: mkstick creates and re-keys no shelf today (`station install
  --shelf` names an existing one), so the sizing above is not built.
  `station status` shows each shelf keyslot's KDF and cost, and warns when
  an argon2id slot needs more memory than the machine running it has.
- Rigs: the rig stick's p3 (32 MiB, time 4, 4 lanes) and the station
  disk's shelf (64 MiB, time 4, 4 lanes) are argon2id; `make test-core` adds kdf-memory-uefi and
  kdf-memory-bios, a 512 MiB slot in a 384 MiB machine, refused with the
  page.

### Tests

- RFC 9106 test vectors for Argon2id, RFC 7693's for BLAKE2b.
- A keyslot made by `cryptsetup --pbkdf argon2id` (the existing
  `build/test-luks-argon.img` fixture is one) unlocked by `luks/`.
- Rigs: the stick with an argon2id p3 under both loaders (`make
  test-core`); a station on an argon2id shelf (UEFI and BIOS station rigs);
  the refusal page for a slot larger than the machine.

## 2. TOTP from the TPM

### Why

The personal-OS passphrase is typed into falcon's own prompt (the shelf
unlock), so the check that the machine has not been tampered with has to
come before that prompt, which only falcon can show. Reading PCRs and
comparing them proves nothing: falcon reads them through the firmware, and a
tampered firmware can report good values. What firmware cannot do is make
the TPM release a secret sealed to values it never measured. So the seed of
a TOTP (RFC 6238) is sealed to the PCRs of a known-good boot; falcon asks
the TPM for it and shows the current code, and the user compares it with an
authenticator before typing the passphrase. A label, like the trust
predictions (DESIGN Section 7.8): it enforces nothing and signs nothing.

### Decided (2026-09-28)

- UEFI stations only (not the stick: a seed unseals only on the TPM it was
  sealed on, and a station is one machine). TPM 2.0 and 1.2.
- Not falcon-bios, for now. Sized 2026-09-28 at about a third more than the
  UEFI work: TIS and CRB drivers of its own (the TCG BIOS INT 1Ah interface
  is uneven and 1.2-only on most machines), stage 2 measuring stage 3 into a
  PCR within its ~33 KB budget (the BIOS measures only the MBR), and a
  refusal on the many BIOS machines that measure nothing. The one BIOS
  station here, the A21m, has no TPM to prove it on.
- The PCRs are chosen at enrollment (typically 0, 2, 4, 7; 4 and 7 catch a
  changed boot chain or Secure Boot keys at the price of a reseal after each
  falcon or shim update; 1 catches firmware settings and changes often).
- The unseal travels in an encrypted session, so a TPM-bus interposer does
  not learn the seed.
- The clock is UTC. A machine that keeps local time will show the wrong
  code; if that bites, it is surveyed then.

### Shape

- `falcon-core/src/tpm/`: command marshalling for TPM 2.0 and 1.2, the PCR
  policy, the session, the NV read, host-tested against swtpm. Two
  transports: the UEFI loader through the firmware (`EFI_TCG2_PROTOCOL`
  SubmitCommand for 2.0, `EFI_TCG_PROTOCOL` PassThroughToTpm for 1.2), the
  enrollment through `/dev/tpmrm0` (`/dev/tpm0` for 1.2). No TPM driver in
  falcon.
- The seed (20 bytes) lives in a TPM NV index readable only when the PCRs
  match: a PolicyPCR read policy on 2.0, `pcrInfoRead` on 1.2. Nothing
  sealed sits on disk to keep in step.
- Encrypted session: on 2.0, a policy session salted to a TPM key (ECDH,
  P-256) with AES-128-CFB parameter encryption of the NV read's response,
  and the response HMAC checked; on 1.2, an encrypting transport session
  (open item 4). The TPM key is pinned at enrollment: its name (2.0) or
  its public key (1.2) goes into STATION.INI with the PCRs and the index,
  so an interposer cannot hand falcon a key of its own.
- The code: HMAC-SHA1 (already in core) over 30-second steps of UTC from the
  firmware's clock, 6 digits.
- Enrollment, from the machine's OS or DevLink recovery:
  `mkstick station totp enroll --pcrs 0,2,4,7`, `reseal`, `remove`. It
  creates the index, writes the seed and shows it once as a QR code for the
  authenticator. The chosen PCRs and the index go into STATION.INI, so the
  station knows what it is checking.
- In the loader: the code under the banner, refreshed each step, and again
  at the passphrase prompt. If the TPM will not release the seed, both say
  "firmware, its settings or the boot chain changed since enrollment" and
  the station boots on regardless: a warning, never enforcement.
- Rig: QEMU with swtpm in 2.0 and 1.2 modes under OVMF with TPM support: a
  matching boot shows the code the host computes from the seed; a changed
  PCR shows the warning; the unseal's response on the wire is ciphertext.

## Open items

1. Settled 2026-09-28: the stick's p3 is argon2id at 32 MiB, time 4, 4
   lanes, the same work as the 64 MiB, time 2 first chosen (about 5.4 s
   on the A21m). Measured with falcon-core's argon2id built i586 -O2 static, the
   code both loaders run. On the A21m (Pentium III, 548 MHz): 16 MiB t=2
   1343 ms, 32 MiB t=2 2698 ms, 64 MiB t=2 5418 ms, 64 MiB t=4 10458 ms,
   64 MiB t=4 p=4 10474 ms, 128 MiB t=2 10812 ms, 256 MiB t=2 21594 ms:
   linear, about 42 ms per MiB per pass, and lanes filled in turn cost
   nothing. The same binary on an i9 (binhost): 64 MiB t=2 201 ms, t=4
   385 ms. The decision was time 2; cryptsetup refuses
   `--pbkdf-force-iterations` below 4 for argon2 ("Forced iteration count
   is too low for argon2id (minimum is 4)"), and cryptsetup formats p3, so
   time is 4, its floor, and the memory came down to 32 MiB to keep the
   A21m at about 5.4 s (64 MiB at time 4 would be 10.5 s). The QEMU/KVM
   rig on the desktop, from "unlocking..." to the chooser (64 MiB, time 4,
   4 lanes, from `make test-core`, polled at about 0.1 s): UEFI
   0.23-0.24 s, falcon-bios 0.43-0.44 s. Both loaders
   refuse a 512 MiB slot in a 384 MiB machine with the page (UEFI: 295 MiB
   free; falcon-bios: 319 MiB).
2. The station sizing's target time, and its conservative default for
   `--no-scan`.
3. Settled 2026-09-28, against swtpm 0.10.2 (`make test-tpm`). 2.0: index
   0x01800FA1 in the owner range (tpm2-totp's default is 0x018094AF), SHA-256
   name, attributes OWNERWRITE, WRITEALL, POLICYREAD and NO_DA, authPolicy
   the PolicyPCR digest of the enrolled PCRs' SHA-256 values. 1.2: index
   0x20000FA1 (the U bit, D clear), TPM_NV_PER_OWNERWRITE, `pcrInfoRead`
   with the composite as digestAtRelease and every locality, no read
   authorisation. On both, the owner authorises writing as well as defining
   and removing, so enrollment needs the one secret it already needs and
   mkstick keeps none; reseal is remove and define, since the policy is
   fixed at definition. A 1.2 TPM with nvLocked clear skips every NV check,
   PCRs included, so enrollment refuses it.
4. Settled 2026-09-28, same rig: TPM_EstablishTransport with
   TPM_TRANSPORT_ENCRYPT and TPM_ALG_MGF1 (the scheme every 1.2 TPM must
   have; AES-CTR is optional), the session secret RSAES-OAEP-encrypted
   (SHA-1, "TCPA") to the SRK, authorised with the SRK's usage secret, the
   well-known 20 zero bytes. The SRK's public key is not readable at boot
   (readSRKPub is off once the TPM is owned), so enrollment reads it with
   the owner's authorisation (TPM_OwnerReadInternalPub) and STATION.INI
   keeps it; that is also the pin. The NV read is wrapped with no
   authorisation of its own. The 1.2 enrollment write travels in the clear
   (from the machine's own OS); the 2.0 one is encrypted.
5. Settled 2026-09-28: qrencode when it is on PATH, and the base32 secret
   and the otpauth URI always; mkstick says when there is no QR code.
6. Tooling for the rigs. Settled 2026-09-28: no OVMF build is needed.
   Measured with `falcon-efi/tools/tpm-probe/` under swtpm 0.10.2 and QEMU
   10.2, Gentoo's edk2-bin-202408 (`OVMF_CODE.fd`, `OVMF_CODE.secboot.fd`)
   and Debian trixie's ovmf 2025.02-8+deb13u1 (`OVMF_CODE_4M.fd`, the
   harness image's) both support both versions: with a TPM 2.0 (TIS or
   CRB) they install `EFI_TCG2_PROTOCOL`, with a TPM 1.2
   `EFI_TCG_PROTOCOL`, and PCR0 read back through SubmitCommand or
   PassThroughToTpm is measured. A fresh swtpm state needs no
   `swtpm_setup`. EDK2's `EFI_TCG2_BOOT_SERVICE_CAPABILITY` is naturally
   aligned (36 bytes), not packed. swtpm is installed on the workstation
   and added to the harness image (not yet rebuilt). The rig is
   `make test-tpm-rig` (`falcon-efi/tools/run-tpm-rig.sh`, swtpm through
   `falcon-efi/tools/swtpm.sh`): the probe alone and started by the UEFI
   station, 2.0 and 1.2, Secure Boot off and on; the loader's cases go
   into it as station cases. The TPM library's own flows run against
   swtpm directly, with no firmware, in `make test-tpm`
   (`falcon-core/tools/run-tpm-rig.sh`).
