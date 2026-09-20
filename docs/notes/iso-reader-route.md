# The ISO reader: taking the survey off the firmware (2026-09-19)

Decided 2026-09-19. The loader asks the firmware to parse a medium in two
places. One of them has to (the chainload, DESIGN Section 7.6). The other is
the menu's survey, which reads a handful of files to draw a label, and it is
the flakiest firmware surface the loader touches. This note is the route for
moving the survey onto falcon-core's own readers, and the abstraction it
leaves behind is the one a BIOS build would need (`bios-route.md` Section 1).

Two goals, in this order:

1. The label path stops depending on the firmware's ISO9660, UDF and FAT
   drivers, its El Torito walk and its `ConnectController`.
2. What replaces it is a medium abstraction over one read callback, so the
   same code answers "what is on this medium" whether the bytes arrive from a
   p2 file under UEFI or from `INT 13h` under BIOS.

## 1. What the firmware reads for us today

Measured against master 1ccece5.

| path | who parses the medium | must it be the firmware? |
| --- | --- | --- |
| `iso_survey_media` (family, signature, trust prediction) | firmware | **no** |
| `iso_boot` chainload | firmware, then `LoadImage` by device path | **yes** |
| `direct/` | falcon-core's `iso9660`/`udf` | - |
| `bless/`, `bsd/`, `win/` | falcon-core | - |

The survey's sequence, per menu entry, at draw time
(`falcon-efi/src/iso/iso.c`):

1. publish the virtual block device (`iso_open`);
2. `ConnectController`, so the firmware's partition driver walks the El Torito
   catalog and its filesystem drivers bind to what it finds;
3. `esp_open_root` on each resulting handle, then `family_detect` over
   `probe_exists_ci`/`probe_read_ci`, which are `EFI_FILE_PROTOCOL` calls;
4. `root_signature` and `root_trust`, reading `/EFI/BOOT/BOOTX64.EFI` back out
   through the same protocol;
5. tear the device down again.

Three workarounds in the tree exist only because of that sequence, and each
names its own cause:

- **`PROBE_LIMIT 24`** (`falcon-efi/src/efi/main.c:46`). "Each probe builds a
  virtual medium, makes the firmware bind its partition and FAT drivers to it
  and tears the whole thing down again, so the cost is bounded by a constant
  rather than by how many images someone dropped on the stick." Entries past
  the limit are drawn unlabelled.
- **`probe_open_ci`'s case-folding retry.** EDK2's UDF driver is
  case-sensitive: `\EFI\BOOT\BOOTX64.EFI` on Windows install media is
  `EFI_NOT_FOUND` while `\efi\boot\bootx64.efi` opens. Measured on OVMF.
- **`select_boot_volume`.** Windows media publishes two volumes and the
  firmware's enumeration order does not say which one boots.

And there is already a precedent for the replacement. `core_family`
(`iso/iso.c:1273`, added in 1eb4af9 for M7) mounts the image with falcon-core
after teardown, because a FreeBSD ISO publishes nothing to the firmware but a
1.6 MB FAT boot image and can therefore only ever be "unknown" to it. That is
the new primary path already written, wearing a fallback's clothes.

## 2. What stays the firmware's, and is not up for discussion

The chainload. `iso_boot` publishes the device, connects drivers, builds a
device path with `build_load_path` and hands it to `LoadImage`. Invariant 2
(DESIGN Section 7.2) is that the platform decides what may run, and it decides
by being handed a path to a volume it mounted itself. A medium falcon-core
parsed is not a device path, so nothing in this note touches the boot path.

`select_boot_volume` stays with it, for the same reason: the question "which
of the firmware's volumes will `LoadImage` accept" is only meaningful about
the firmware's volumes.

So the split after this work is:

    survey   falcon-core reads the image file on p2
    boot     publish -> ConnectController -> LoadImage (unchanged)

## 3. What falcon-core is missing

`falcon-core/src/fs/` holds exfat, ext4, iso9660 and udf. Two gaps stop it
answering everything the firmware answers today.

**No FAT12/16/32 driver.** Checked 2026-09-19: the only hits for FAT in the
tree are a rejected design in `media/bsdmd.h` and a `mkfs.vfat` call in
`falcon-cli/src/mkstick/create.c`. exFAT is a different filesystem and its
driver does not read FAT. This matters three times over:

- the El Torito boot image is a FAT volume on Windows media (carrying
  `cdboot.efi`) and on FreeBSD media (carrying `loader.efi`), so without FAT
  core cannot read the volume that actually boots;
- p1 is vfat (`FALCON-ESP`), and nothing in falcon-core can read it. The
  loader reads its own ESP through the firmware's Simple File System protocol,
  which a BIOS build would not have;
- `bios-route.md` Section 6 lists El Torito as net-new in core but does not
  cost FAT, which is an omission in that note.

**No El Torito boot catalog parser.** Confirmed: the phrase appears in
falcon-efi comments describing what the firmware does on our behalf, and in
one comment in `media/bootfs.h`. There is no parser, because under UEFI the
firmware's partition driver is the parser.

## 4. The corpus's El Torito catalogs, measured

Surveyed 2026-09-19 across all 36 corpus ISOs, before writing a line of the
parser, because the M7 lesson is that generalising from one medium is how the
roadmap acquires a claim nobody measured. Every one of the 36 parsed. Four
findings constrain the design.

**4.1 The shape is uniform.** Every medium carries exactly two bootable
entries: a default entry with platform x86 and a final section header holding
exactly one section entry. Nothing in the corpus uses more than one section
header, and no medium carries a second UEFI entry.

**4.2 Emulation is always none.** Media type 0 (no emulation) on every entry of
every medium. Not one 1.2 MB, 1.44 MB, 2.88 MB or hard-disk emulation entry
exists in the corpus. The parser should still report the others rather than
assume, but no consumer has to handle them.

The default entry's image is raw code (isolinux, `etfsboot.com`), not a
filesystem, and `fs_identify` correctly declines all 36 of them. That needs no
special case: under UEFI it is a volume that does not mount, and under BIOS it
is exactly the thing to jump to (`bios-route.md` Section 4.1).

**4.3 The boot image is always FAT, and all three widths appear.** FAT12 on 26
media, FAT16 on 6 (Fedora, Rocky, Alma, openSUSE, agama, CachyOS), FAT32 on 2
(both Arch snapshots). So stage B needs FAT12, FAT16 and FAT32, measured
rather than assumed, and the fixture needs one of each.

**4.4 `sector_count` cannot size the volume.** It is 0 on three media (both
Arch, and Void) and 1 on four (both Windows, both ESXi), where 1 sector is 512
bytes and the FAT volume is plainly larger. The field says what the firmware
should load, and for a UEFI entry the firmware ignores it and mounts the
volume instead. So `medium_vol.len` comes from the mounted filesystem's own
total-sectors field, never from the catalog, and the parser must not reject an
entry whose count is zero.

**4.5 Platform ID cannot select the entry.** OPNsense 25.1 declares its UEFI
FAT boot image in a section header reading `91 00 01 00`: final header,
platform **0x00 (x86)**, one entry. FreeBSD 15.1's is `91 ef 01 00` with
byte-identical geometry otherwise (same LBA 20, same 4096 sectors, same
FAT12). OPNsense mis-masters the field, the firmware does not care, and a
parser that selected on `platform == 0xEF` would find no boot volume there at
all.

The rule that follows: walk the section entries in order and `fs_identify`
each one, take what mounts. Platform ID is reported and never branched on. A
parser stricter than the firmware is a parser that loses media the firmware
handles.

## 5. The medium contract

A CD is not one filesystem. It is a primary ISO9660 tree, possibly a
supplementary Joliet tree, possibly SUSP/Rock Ridge over the primary one,
possibly a UDF volume in the same bytes, and possibly an El Torito boot image
that is a filesystem of its own at some offset. `ConnectController` is what
turns that into a set of handles. To stop calling it we build the set.

    /* falcon-core/src/media/medium.h */
    typedef struct {
        const fs_ops *ops;
        uint64_t      base;   /* byte offset of this volume in the medium */
        uint64_t      len;    /* 0 when the volume runs to the end */
        fs_vol        vol;
    } medium_vol;

    typedef struct {
        fs_read_fn read;      /* the whole medium, byte-addressed */
        void      *ctx;
        uint64_t   size;
        medium_vol vol[MEDIUM_MAX_VOLS];
        unsigned   n;
        unsigned   boot;      /* index of the El Torito image, or n */
    } medium;

    int medium_open(medium *m, fs_read_fn read, void *ctx, uint64_t size);

`medium_open` does `ConnectController`'s job in code we own: probe iso9660 at
offset 0, probe udf at offset 0, parse the boot catalog, and `fs_identify` the
boot image at its own offset rather than naming FAT. Each volume gets a small
offset shim over the single read callback, which is the only new plumbing the
idea needs.

`MEDIUM_MAX_VOLS` is 4, which is `ISO_MAX_VOLUMES` in `iso/iso.c` and the same
argument in a different setting. There the bound caps what an unexpected
firmware can cost per menu entry; here nothing unexpected can happen, because
we do the enumerating, so it is only "a medium publishes one or two and the
loader has no heap to grow into". Keeping the number equal means stage D
changes no cap.

Consumers do not change. `bootfs_probe` already adapts an `fs_vol` to a
`family_probe`, and `bootfs_src` already adapts one to a `bootcfg_src`. Both
convergence points exist because `core_family` needed the first one. The new
module supplies the volume, not a new interface.

**This is the per-platform boundary.** Under UEFI, `read`/`ctx` are an open
`fs_file` on p2. Under BIOS they are `INT 13h` behind the same signature. The
module cannot tell, which is the claim `bios-route.md` Section 1 makes about
the tree and this is the part of it that is not yet true.

## 6. Name space precedence: already decided, do not re-decide

`falcon-core/src/fs/iso9660.h` states the rule the driver already implements:
Rock Ridge on the primary tree when the root's "." record opens with a SUSP
"SP" entry, else Joliet when a supplementary descriptor carries one of the
three UCS-2 escapes, else the primary tree as recorded. One tree is picked at
mount.

What is new is the rule ACROSS drivers, since a medium can carry both an
ISO9660 tree and a UDF one and `medium_open` will mount both:

1. UDF when present, because a medium that has one puts its real content
   there. Microsoft's `oscdimg` leaves the ISO9660 namespace holding a single
   stub `README.TXT`.
2. ISO9660 otherwise.
3. The El Torito boot image last, and only for questions about the boot image
   (the signature and trust probes read `/EFI/BOOT/BOOTX64.EFI` from it).

`family_detect` still walks every volume until one classifies, as
`media_family` does today, because the fingerprint may live in a namespace
that is not the one the loader boots from.

## 7. Staging

A, B and C touch no boot path and add no firmware call. D is where regression
risk lives.

All four stages are done and measured: `media/eltorito.c` (14 tests),
`fs/fat.c` (11 tests across all three widths), `media/medium.c` (7 tests) and
the survey itself. Each was checked against the corpus rather than against
the fixtures alone; Section 11 records what those runs found and Section 8
records the acceptance run.

| stage | what | new | touched |
| --- | --- | --- | --- |
| A | `media/eltorito.c`, boot catalog parser | ~200 + tests | 0 |
| B | `fs/fat.c`, read-only FAT12/16/32 with LFN, behind `fs_ops` | ~800 + tests | ~20 (registry) |
| C | `media/medium.c`, the volume set | ~250 + tests | 0 |
| D | `iso_survey_media` moves onto `medium` | ~100 | ~300 removed |

**That deletion list was wrong, and the correction is worth keeping.** It
assumed every firmware probe helper existed only for the survey. Two of them
do not:

- `select_boot_volume` is on the CHAINLOAD path, deciding which of the
  firmware's volumes gets handed to `LoadImage`, and it reaches
  `esp_open_root` and `probe_exists`, which reach `probe_open`, `probe_root`
  and `path_to_ucs2`.
- `media_family` fills `iso_boot_result.family`, the "family: X" line the
  menu prints after a chainload returns. It reaches `root_family`, which
  reaches `probe_exists_ci` and `probe_read_ci`, which reach `probe_open_ci`,
  `path_fold_lower` and `file_read_at`.

So stage D removes four functions, not fifteen: `root_signature`,
`root_trust`, `efi_file_read_cb` and `probe_read`, plus `core_family`, which
becomes the whole survey rather than its tail. `PROBE_LIMIT` rises from 24 to
256.

The win was never the deletion count. It is that the survey no longer
publishes a block device, no longer calls `ConnectController`, and no longer
needs a case-folding retry, because `fs_lookup` matches
ASCII-case-insensitively where EDK2's UDF driver does not. The helpers that
remain are the firmware's half of a boot, which Section 2 always said stays.

Stage B's fixture follows the pattern of `falcon-core/tools/mk-test-exfat.sh`
and its siblings: a `mkfs.vfat` image built by the Makefile, with FAT12, FAT16
and FAT32 variants so the cluster-size and root-directory differences between
them are each covered rather than assumed.

## 8. The acceptance test, and its result

`tools/coverage/run-corpus.sh` over the full corpus, with `docs/coverage.md`
diffed against the version generated before stage D. The survey's job is to
produce a label, so the test is that every label is the same or better, and
that no cell's route or verdict moves.

**Run 2026-09-20 against the baseline at d869309:**

    unchanged cells: 216   improved: 0   regressed: 0
    new rows: bazzite-stable

Not one cell moved. The survey no longer publishes a block device, no longer
calls `ConnectController` and no longer reads anything through
`EFI_FILE_PROTOCOL`, and the scorecard is byte-identical apart from the row
that was added to the corpus in the same session.

The rows that were worth watching, because they are the ones the firmware
answered differently from core:

- **Windows**, where `select_boot_volume` and the UDF case-folding retry were
  both load-bearing. Core picks UDF by the rule in Section 6 and `fs_lookup`
  is case-insensitive, so neither workaround is needed on the label path.
  Unchanged.
- **FreeBSD, pfSense, OPNsense**, which classified through `core_family`
  before and so should be unchanged by construction. They are.
- **OPNsense** specifically, the one medium in the corpus that would break a
  platform-ID-keyed El Torito parser (Section 4.5). Unchanged.

Both runs were made on binhost-builder in the harness container, which is
what makes the comparison mean anything: the same toolchain, the same OVMF,
the same corpus verified by SHA-256.
## 9. Risks

**9.1 A FAT driver is a real driver.** ~800 lines under the 80% coverage gate
and the strict-warning bundle, reading attacker-controlled bytes off removable
media, like every other driver in `fs/`. It is the best-documented filesystem
there is and this one is read-only, but it is not a weekend.

**9.2 The survey's answer must not change.** Section 7 is the gate. The
failure mode to fear is not a crash, it is a medium that quietly classifies as
something else and boots by a different route because of it.

**9.3 The corpus is 36 media, and the world is not.** Section 4 measured what
is there, which is how 4.4 and 4.5 were found rather than tripped over. It
says nothing about media nobody has dropped on the stick yet: an emulation
entry, a second section header, a boot image that is not FAT. The parser
should report those honestly and let the caller get nothing useful, rather
than refuse the medium or assert.

**9.4 falcon-core grows a driver it does not need for UEFI alone.** FAT is
justified by the El Torito volume, by p1 and by BIOS. Only the first of those
is load-bearing today. If the BIOS work is never taken, stage B is a driver
carried for one use.

## 10. Open questions

1. **Does `direct/` move onto `medium` too?** It mounts iso9660 or udf by hand
   today and would be simpler on the new module, but it works, it is on a boot
   path, and there is no reliability argument for touching it. Probably later,
   probably as its own change.
2. **Does the loader read its own ESP through `fs/fat.c`?** It has no reason
   to under UEFI, where the firmware's Simple File System protocol is right
   there and is the one surface we have never seen misbehave. A BIOS build
   would have to. Leave it alone until there is a BIOS build.
3. **Where does the ISO label come from after stage D?** `fs_ops.label` is
   optional and the media drivers carry it, so the answer is likely "the
   chosen volume's", but falcon-hook finds its medium by label and that path
   must not change spelling.

## 11. What stages A to C measured against the corpus

Each stage was run over all 36 corpus ISOs rather than trusting its fixtures,
for the reason Section 4 gives. Recorded here because two of the three runs
changed something.

**11.1 El Torito (stage A).** The parser was diffed against an independent
implementation over every medium: identical on all 72 entries. Section 4 is
that survey, and 4.4 and 4.5 came out of it before any code was written.

**11.2 FAT (stage B).** Every corpus ISO's boot image mounts and lists:
`mounted=36 failed=0`, FAT12, FAT16 and FAT32, Windows and pfSense and
openSUSE and Arch among them. That is the first time falcon-core has read the
volume the firmware boots from.

**11.3 The volume set (stage C).** `medium_open` over all 36: nothing
unmounted, and `/EFI/BOOT/BOOTX64.EFI` resolvable through the boot volume on
every single one, which is the file the chainload is handed and the file the
signature and trust probes read. Windows media yields three volumes
(`udf + iso9660 + boot`) and classifies from the UDF one, which is the
precedence rule of Section 6 doing its job on the medium that needs it.
Thirty of the 36 classify to a known family; the six that do not are the two
ESXi installers and the four Tier B installers that have no fingerprint in
`family.c` today, which is unchanged from what the firmware path reports.

**11.4 A pre-existing mislabel, surfaced by 11.3 and then fixed.** Manjaro
and MX classified as `memtest`. Neither had a layout fingerprint in
`family.c`, so both fell through to the last-resort memtest check, and both
ship `/boot/memtest` - the memtest86+ kernel image a distro bundles beside
itself rather than as its identity. `family_detect` over the same ISO9660
tree answered the same way before any of this work, so it was never a stage
D regression.

Fixed by giving each the row it was missing: `FAMILY_MANJARO`
(`/manjaro/x86_64`, or `/.miso` beside `/manjaro`) and `FAMILY_ANTIX`
(`/antiX/linuxfs`), both checked before the memtest last resort. The
behaviour columns are deliberately unchanged from what memtest gave them,
because both media already booted direct and blessed in every Secure Boot
state with no cmdline fragment: `needs_injection` has no consumer in the
tree at all, and `cmdline_template` is read only by the sign tool, so a
fragment invented here would change what a blessed UKI carries for no
measured reason. The label is now right and nothing else moved.
