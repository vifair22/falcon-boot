/* falcon-boot appliance: what its modules share (docs/DESIGN.md Section 7.5).
 *
 * The appliance is PID 1 in an initramfs, one program split by job: main.c
 * (the flow, early setup, teardown), console.c (every console, and reading
 * from them), stick.c (finding p2 and p3), run.c (the helper programs),
 * survey.c (p2 against the manifest), list.c (the bless list the loader
 * delivered), select.c (the pending list and its gate), shelf.c (the MOK
 * keypair off p3, read in process), bless.c (the work), route.c (what each
 * file is owed, host-tested). main.c carries the design
 * notes for the whole program.
 */

#ifndef FALCON_APPLIANCE_H
#define FALCON_APPLIANCE_H

#include <manifest/manifest.h>
#include <media/family.h>
#include <sign/payload.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---- fixed layout of this initramfs and of the stick ---- */

#define P2_MNT   "/mnt/p2"
#define SIGN_TOOL "/usr/bin/falcon-boot-sign-static"

#define MANIFEST_PATH P2_MNT "/falcon/blessed/manifest"

/* Reserved directory of DESIGN Section 6.2; never a bless candidate. */
#define RESERVED_DIR "falcon"

#define PASS_MAX     512u
#define UNLOCK_TRIES 3

/* The loader-delivered credential (DESIGN Section 5.1): sd-stub packs the
 * companion file into the initramfs under this name; systemd would strip
 * the .cred suffix on import, but this init reads the file itself. */
#define CRED_FILE "cryptsetup.passphrase.cred"

#define MAX_CANDIDATES 64u
#define MAX_RECORDS    256u
#define MANIFEST_MAX   (1u << 20)

/* p2 is a drag-and-drop library, so the tree is the user's, not ours. Eight
 * levels is deeper than any ISO collection and stops a symlink-free but
 * pathological tree from recursing this program to death. */
#define WALK_MAX_DEPTH 8

/* Where the build script stages the kernel modules this flow needs. Arch's
 * kernel ships exFAT, the loop driver and FAT as modules, so the appliance
 * carries them rather than assuming: without exfat there is no p2, and
 * without loop and vfat there is no way into the filesystem inside a .img
 * payload, which is where falcon-cli/src/sign/payload.c has to write. p3
 * needs none: it is decrypted in process (shelf.c).
 *
 * The files are named NN-<module>.ko and loaded in name order, because that
 * order is a dependency order the build script computed from the kernel's own
 * modules.dep. Resolving dependencies here would mean parsing modules.dep in
 * PID 1 to learn something that is fixed the moment the image is built. */
#define MODULE_DIR "/modules"

/* Device-node paths. Sized for a kernel block-device name (DISK_NAME_LEN is
 * 32, but sysfs entries are bounded only by NAME_MAX) plus "/dev/" and a
 * partition suffix, so no name a disk can carry gets silently truncated into
 * the path of some other device. */
#define NODE_MAX 288u

typedef struct {
    char p2[NODE_MAX];    /* device node of the exFAT ISO library */
    char p3[NODE_MAX];    /* device node of the LUKS2 UKI shelf */
} stick_parts;

typedef struct {
    char              rel[MANIFEST_PATH_MAX]; /* path relative to the p2 root */
    uint64_t          size;
    manifest_bless    state;                /* NONE or STALE; FRESH is done */
    family_id         fam;
    /* NONE for an ISO. A payload is signed in place, which decides what the
     * review has to disclose and which arguments the tool is given, so the
     * shape travels with the candidate rather than being re-derived from the
     * name at each of those points. */
    sgn_payload_shape shape;
    int               selected;             /* marked by hand; never preset */
} candidate;

#define MIB ((uint64_t)1 << 20)

/* ---- survey state (survey.c) ---- */

extern manifest_record g_records[MAX_RECORDS];
extern size_t          g_record_n;
/* The bless queue, which everything downstream of the survey works on. */
extern candidate       g_cand[MAX_CANDIDATES];
extern size_t          g_cand_n;
extern size_t          g_done_blessed;
extern size_t          g_windows;       /* Windows media: nothing to bless */
extern size_t          g_unreadable;
extern char            g_manifest[MANIFEST_MAX];

/* ---- functions one module calls in another ---- */

/* console.c: every console, and reading from them */
void say(const char *fmt, ...);
void console_open(void);
ssize_t read_line(char *buf, size_t cap);
ssize_t read_secret(char *buf, size_t cap);

/* stick.c: finding the stick, by the install ID the loader handed over
 * (stickid.h). */
enum {
    STICK_FOUND     = 1,
    STICK_NOT_FOUND = 0,   /* no p2 and p3 carrying the ID appeared */
    STICK_NO_ID     = -1   /* the loader handed over no install ID */
};
int find_stick(stick_parts *out);
void say_block_devices(void);
int exfat_uuid(const char *node, char *out, size_t cap);

/* run.c: the helper programs */
int run(char *const argv[], const char *in, size_t in_len, const char *echo);

/* survey.c: p2 against the manifest */
void load_manifest(void);
void walk(const char *abs, const char *rel, unsigned depth);
/* One file, as the walk would see it: into a queue, or reported as done. */
void survey_file(const char *abs, const char *rel, uint64_t size);

/* list.c: the bless list the loader delivered (falcon-core/src/manifest/
 * blesslist.h). 1 when there was one, with every acceptable path on it
 * surveyed; 0 when there was none, which is the interactive appliance. */
int bless_list_survey(void);
void payload_prefilter(void);

/* select.c: the pending list and its gate */
int choose(candidate *q, size_t n);
size_t selected_count(const candidate *q, size_t n);
int cand_cmp(const void *a, const void *b);

/* shelf.c: p3 through falcon-core's LUKS2 and ext4, nothing mapped or
 * mounted. The keypair is handed on as two sealed, inheritable memfds, named
 * to the sign tool by the /proc/self/fd paths in key and cert. */
enum {
    SHELF_OK       = 0,
    SHELF_ERR      = -1,
    SHELF_NOT_LUKS = -2,   /* p3 is not a LUKS2 volume this reader takes */
    SHELF_WRONG    = -3,   /* no keyslot opened with that passphrase */
    SHELF_NO_KEY   = -4    /* unlocked, but no MOK keypair on it */
};
typedef struct {
    int  key_fd;
    int  cert_fd;
    char key[32];
    char cert[32];
} shelf_keys;
int shelf_open(const char *p3);
int shelf_unlock(const void *pass, size_t len);
int shelf_keypair(shelf_keys *k);
void shelf_keys_close(shelf_keys *k);
void shelf_close(void);

/* bless.c: the work */
/* The keypair while the bless half runs; its paths are what select.c and
 * survey.c hand the sign tool too. */
extern shelf_keys g_keys;
/* confirmed: the loader's menu already asked (the list came from there), so
 * the queue is signed as it stands, without the mark-and-review. Everything
 * it opens on p3 is closed again before it returns. */
void bless_half(const stick_parts *parts, int confirmed, size_t *signed_n,
                size_t *failed_n, size_t *skipped_n);

#endif /* FALCON_APPLIANCE_H */
