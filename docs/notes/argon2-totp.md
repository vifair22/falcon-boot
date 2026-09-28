# Argon2id keyslots and a TPM-backed TOTP check

Status: design, 2026-09-28. Nothing here is built yet. Two pieces of one
work stream, in this order: Argon2id first (it unblocks station installs),
then TOTP.

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
- Encrypted session: on 2.0, a session salted to a TPM key (ECDH, P-256)
  with AES-128-CFB parameter encryption of the NV read's response; on 1.2,
  a transport session (open item 4).
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

1. The stick's fixed argon2id cost, from measurements: time on the A21m
   under falcon-bios and on a current machine under UEFI, memory against a
   512 MB machine.
2. The station sizing's target time, and its conservative default for
   `--no-scan`.
3. The TPM NV index handles (2.0 owner range; 1.2 index), and what
   authorises writing them (owner auth, or an index auth kept by mkstick).
4. The 1.2 encrypted session: transport session setup (TPM_EstablishTransport
   needs RSA encryption to a TPM key; which key, and whether its public part
   is readable without owner auth).
5. The QR code at enrollment: a dependency (qrencode) or falcon's own.
6. Tooling for the rigs: swtpm on the workstation and in the harness image,
   and an OVMF build with TPM 1.2 and 2.0 support.
