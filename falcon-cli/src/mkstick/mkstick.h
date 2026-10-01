/* falcon-boot mkstick: the constants the stick's layout is made of, in one
 * place, and the commands' entry points.
 *
 * Everything here is a contract with something outside this file: the loader
 * (which looks for these partition types and these directories), shim (which
 * looks for its own next-stage name on p1), DESIGN Section 3's layout and
 * Section 7.5's key placement. Changing a value here without changing the
 * other side of the contract produces a stick that builds cleanly and does
 * not boot.
 */

#ifndef FALCON_MKSTICK_H
#define FALCON_MKSTICK_H

#include <stddef.h>
#include <stdint.h>

#include <common/opt.h>
#include <common/status.h>
#include <luks/luks.h>

/* ---- partition type GUIDs, in the textual form sgdisk takes ----
 *
 * These are the byte arrays of falcon-core/src/part/part.h written the other
 * way round. They are NOT a second source of truth: mkstick_guid_selfcheck()
 * parses each string back into the 16-byte on-disk form and compares it against
 * the loader's own constant at startup, so a typo here fails immediately and
 * loudly rather than producing a stick the loader cannot recognise.
 *
 * p2's type being Microsoft basic data, and p3's and p4's deliberately not
 * being, is the asymmetry of DESIGN Section 3: p2 has to be visible to
 * Windows (drag-and-drop, and the WinPE jump binary of Section 6.3), and p3
 * and p4 have to be invisible to it, because Windows offers to format a
 * basic-data partition it cannot read - which is what a LUKS volume is. */
#define MKS_TYPE_ESP   "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define MKS_TYPE_ISO   "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"  /* p2, MS basic */
#define MKS_TYPE_SHELF "9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF"  /* p3, falcon */
#define MKS_TYPE_LUKS  "CA7D7CCB-63ED-4C53-861C-1742536059CC"  /* p4, stock */
#define MKS_TYPE_BOOT  "D0ACB249-F895-4E0D-AB0D-CFBB6F48AE36"  /* station */

/* Verifies the strings above against falcon-core/src/part/part.h (and, for the
 * standard types, against the values DESIGN Section 3 records). Called at
 * the top of every command that writes or reads a partition table. */
cli_status mkstick_guid_selfcheck(void);

/* Partition names, written into the GPT entry's name field for a person
 * reading lsblk or a partition editor. Nothing reads them to identify a
 * partition; its record does that (DESIGN Section 3.1). */
#define MKS_LABEL_P1 "FALCON-ESP"
#define MKS_LABEL_P2 "FALCON-ISO"
#define MKS_LABEL_P3 "FALCON-UKI"
#define MKS_LABEL_P4 "FALCON-ROOT"

/* ---- on-stick paths (DESIGN Sections 4, 7.5 and 8.1) ---- */
#define MKS_ESP_BOOT_DIR   "EFI/BOOT"
#define MKS_ESP_SHIM_NAME  "BOOTX64.EFI"   /* removable-media default path */
#define MKS_ESP_CERT_NAME  "falcon-boot.cer"
#define MKS_P3_UKI_DIR     "uki"
#define MKS_P3_KEY_NAME    "falcon-mok.key"   /* mode 0600, LUKS-encrypted */
#define MKS_P3_CERT_NAME   "falcon-mok.crt"
#define MKS_P2_BLESSED_DIR "falcon/blessed"

/* The stick's p3 keyslot cost: argon2id at one fixed cost every machine
 * the stick boots can meet (DESIGN Section 3). This is the one place it
 * is written; mkstick
 * create formats with it, status measures p3's slots against it, and
 * falcon-efi/tools/mk-test-stick.sh reads the numbers out of this file for
 * the rig stick.
 *
 * Measured 2026-09-28 with falcon-core's argon2id built i586 -O2, the code
 * both loaders run: about 42 ms per MiB per pass on the A21m (Pentium III,
 * 548 MHz), linear in memory and passes, so 64 MiB costs 5.4 s at time 2
 * and 10.5 s at time 4 there, against 0.20 s and 0.39 s on an i9.
 *
 * Memory 32 MiB with time 4: the same work as 64 MiB at time 2, since
 * cryptsetup will not go below time 4 (below). Half the memory an attacker
 * needs per guess, still memory-hard, and a 512 MB Pentium III has it free
 * above falcon-bios's 64 MB heap floor many times over.
 *
 * Lanes 4: the loaders and the appliance fill lanes in turn, and on the
 * A21m 4 lanes cost the same as 1 (10474 ms against 10458 ms at 64 MiB,
 * time 4), while cryptsetup on the personal OS runs them in parallel.
 *
 * Time 4: cryptsetup refuses --pbkdf-force-iterations below 4 for argon2
 * ("minimum is 4"), and it is what formats p3, so the memory came down
 * instead of the passes; 64 MiB at time 4 would be 10.5 s on the A21m. */
#define MKS_STICK_KDF_MEMORY_KIB 32768
#define MKS_STICK_KDF_TIME       4
#define MKS_STICK_KDF_LANES      4

#define MKS_STR_(x) #x
#define MKS_STR(x)  MKS_STR_(x)

/* Default partition sizes (DESIGN Section 3: p1 ~64 MB, p3 ~1 GB, p2 and p4
 * bulk). p2 and p4 have no default size: they split whatever is left. */
#define MKS_DEFAULT_P1 ((uint64_t)64 * 1024 * 1024)
#define MKS_DEFAULT_P3 ((uint64_t)1024 * 1024 * 1024)

/* Floors, below which a partition cannot do its job. p3 and p4 carry a LUKS2
 * header, which is 16 MiB of the partition before a filesystem exists. */
#define MKS_MIN_P1 ((uint64_t)16 * 1024 * 1024)
#define MKS_MIN_P2 ((uint64_t) 8 * 1024 * 1024)
#define MKS_MIN_P3 ((uint64_t)48 * 1024 * 1024)
#define MKS_MIN_P4 ((uint64_t)24 * 1024 * 1024)

/* Alignment for every partition start and the slack left at both ends for the
 * primary and backup GPT. 1 MiB is what real media wants and what keeps every
 * start a whole number of 4 KiB filesystem blocks. */
#define MKS_ALIGN ((uint64_t)1024 * 1024)

/* ---- putting falcon-bios on a stick (bios.c) ----
 *
 * `mkstick bios`'s install, as a call, so that any other command that puts
 * the stages on a stick runs the same one rather than a second copy. NULL
 * stages mean the build's
 * (build/bin/falcon-bios-stage{1,2,3}.bin); a NULL backup with no_backup
 * clear means a file named after the target. */
typedef struct {
    const char *target;
    const char *stage1;
    const char *stage2;
    const char *stage3;
    const char *backup;
    int         no_backup;
    int         remove;
    int         reserve;
    int         dry_run;
    int         force;
} mks_bios_req;

cli_status mks_bios_install(const mks_bios_req *req);

/* One LUKS2 keyslot's KDF and cost, for a report: "argon2id, time 4,
 * 64 MiB, 1 lane", "pbkdf2, 200000 iterations" (inspect.c). */
void mks_slot_describe(const luks_slot_info *info, char *buf, size_t cap);

/* ---- the commands ---- */
extern const cli_cmd mks_cmd_bios;
extern const cli_cmd mks_cmd_create;
extern const cli_cmd mks_cmd_ident;
extern const cli_cmd mks_cmd_keygen;
extern const cli_cmd mks_cmd_next_boot;
extern const cli_cmd mks_cmd_station;
extern const cli_cmd mks_cmd_status;
extern const cli_cmd mks_cmd_update;
extern const cli_cmd mks_cmd_varstore;

/* `station install|remove|status`: a second level of dispatch, so main.c
 * hands it the arguments after "station". */
cli_status mks_station_main(int argc, char **argv);

#endif /* FALCON_MKSTICK_H */
