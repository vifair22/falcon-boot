/* falcon-boot: read-only LUKS2 unlock (docs/DESIGN.md Sections 3, 5, 9).
 *
 * Purpose. Unlocks p3, the LUKS2+ext4 partition holding the personal-OS
 * UKI, the signing-appliance UKI and the encrypted MOK private key, and
 * then presents the decrypted data segment as an ordinary byte-read
 * callback so a read-only ext4 driver can sit on top of it without knowing
 * anything about encryption.
 *
 * Deliberately EFI-free, exactly like falcon-core/src/fs/exfat.c: every device
 * access goes through a read callback and every context is caller-allocated, so
 * the same object code runs in the loader and in a host test against volumes
 * cryptsetup itself created. Hand-written crypto that has not been checked
 * against the real producer is not worth having.
 *
 * Scope. Read only, one passphrase attempt at a time, pbkdf2 and argon2id
 * keyslots and aes-xts-plain64 segments. An argon2id slot is what a default
 * `cryptsetup luksFormat` produces, which is how a station's shelf is made
 * (DESIGN Section 3). Its memory is the caller's
 * (luks_kdf_mem): this module allocates nothing, and the three places it
 * runs get memory three different ways. A slot that needs more than the
 * caller can give is reported as LUKS_ERR_KDF_MEMORY with the amount, never
 * as a wrong passphrase. argon2i is recognised and refused as
 * LUKS_ERR_UNSUPPORTED_KDF: LUKS2 accepts it, and nothing here makes it.
 *
 * The loader never writes a LUKS volume, never adds or kills a keyslot and
 * never re-encrypts. All of that is Section 8 host tooling.
 *
 * Interactions. Built on falcon-core/src/crypto (hash, pbkdf2, argon2, xts),
 * falcon-core/src/luks/json.c and falcon-core/src/luks/af.c. Its output feeds
 * the read-only ext4 driver and, above that, the UKI load path of Section 5.
 */

#ifndef FALCON_LUKS_H
#define FALCON_LUKS_H

#include <stdint.h>
#include <stddef.h>

#include <crypto/argon2.h>
#include <crypto/xts.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    LUKS_OK                  = 0,
    LUKS_ERR_IO              = -1,  /* read callback failed */
    LUKS_ERR_BAD_HEADER      = -2,  /* magic, version or checksum */
    LUKS_ERR_CORRUPT         = -3,  /* metadata violates the format */
    LUKS_ERR_BAD_ARG         = -4,
    LUKS_ERR_RANGE           = -5,  /* read outside the data segment */
    LUKS_ERR_NOSPACE         = -6,  /* caller's buffer too small */
    LUKS_ERR_UNSUPPORTED     = -7,  /* well-formed, but a feature we lack */
    LUKS_ERR_UNSUPPORTED_KDF = -8,  /* argon2i or unknown; see the scope note */
    LUKS_ERR_NOKEY           = -9,  /* no keyslot opened with this passphrase */
    LUKS_ERR_NOTFOUND        = -10, /* no token of the type asked for */
    LUKS_ERR_KDF_MEMORY      = -11, /* argon2id slot, memory not supplied;
                                     * luks_vol.kdf_need_kib says how much */
};

/* Reads len bytes at byte offset off, relative to the start of the LUKS
 * device. Returns 0 on success, nonzero on failure. Identical in shape to
 * exfat_read_fn, and luks_read below is a valid instance of it, which is
 * how the ext4 driver stacks on this unchanged. */
typedef int (*luks_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

#define LUKS_HDR_BIN       4096u     /* binary header; JSON area follows */
#define LUKS_MAX_KEY       64u       /* aes-xts-plain64, --key-size 512 */
#define LUKS_MAX_SALT      64u       /* the format's fixed salt field */
#define LUKS_MAX_DIGEST    64u
#define LUKS_MAX_SECTOR    4096u     /* largest segment sector_size seen */
/* A PBKDF2 count above this is refused as unsupported rather than run. The
 * count comes from the medium, and 2^32 of them is over an hour per keyslot
 * on a slow machine with nothing on screen to say why. mkstick budgets 200000
 * and cryptsetup's two-second benchmark on a fast CPU lands in the low
 * millions, so this is far above anything a real volume carries. */
#define LUKS_MAX_ITERATIONS (1u << 24)
#define LUKS_KEYSLOT_UNIT  512u      /* keyslot areas are always 512-based */
/* argon2id costs above these are refused as unsupported rather than run,
 * for the reason LUKS_MAX_ITERATIONS gives. The memory is cryptsetup's own
 * ceiling (4 GiB), and so is the lane count's order: it writes at most 4.
 * cryptsetup's benchmark puts time between 4 and the low tens; 256 passes
 * over a gigabyte is minutes on a fast machine. */
#define LUKS_MAX_ARGON2_KIB   (4u * 1024u * 1024u)
#define LUKS_MAX_ARGON2_TIME  256u
#define LUKS_MAX_ARGON2_LANES 16u

/* A JSON area this size covers cryptsetup's 12 KB default with room to
 * spare. luks_open takes the buffer from the caller rather than embedding
 * it, because in the loader it comes from the firmware pool and on the host
 * it is a static, and because a header may legitimately be larger. */
#define LUKS_JSON_CAP      16384u

typedef enum {
    LUKS_KDF_PBKDF2 = 0,
    LUKS_KDF_ARGON2I,
    LUKS_KDF_ARGON2ID,
    LUKS_KDF_UNKNOWN
} luks_kdf_type;

typedef struct {
    luks_read_fn read;
    void        *read_ctx;

    /* Which header copy was accepted, and what it said. Both copies are
     * checked; the one with the higher sequence id wins, which is how
     * cryptsetup itself resolves a header write interrupted midway. */
    uint64_t hdr_off;
    uint64_t hdr_size;
    uint64_t seqid;
    uint8_t  used_secondary;

    char    *json;            /* caller's buffer, NUL-terminated text */
    size_t   json_len;        /* text length, excluding the zero padding */
    size_t   json_area;       /* hdr_size - LUKS_HDR_BIN, checksum coverage */

    /* Data segment (segment 0 of the digest that verified). */
    uint64_t seg_offset;      /* device byte offset of plaintext offset 0 */
    uint64_t seg_size;        /* bytes; 0 when the header says "dynamic" */
    uint64_t seg_iv_tweak;    /* in 512-byte units, as dm-crypt takes it */
    uint32_t seg_sector_size; /* 512 or 4096 */
    uint8_t  seg_dynamic;
    uint8_t  seg_iv_large;    /* the segment's iv_large_sectors flag */

    /* Set by luks_unlock when it returns LUKS_ERR_KDF_MEMORY: the KiB the
     * least demanding argon2id slot it could not run asked for. 0 otherwise. */
    uint32_t kdf_need_kib;
} luks_vol;

/* What the platform gives a keyslot: an argon2id slot's memory, how its
 * lanes run, and the block every slot's area is read into.
 *
 * get returns a block of kib KiB, 8-byte aligned, or NULL when it cannot be
 * had; put takes it back, already wiped, and may be NULL. The UEFI loader
 * answers with AllocatePages, falcon-bios with its heap, a host tool with
 * malloc. One block is outstanding at a time: an argon2id slot's is put
 * back before its area's is asked for, and both before luks_unlock moves
 * to the next slot. An area block refused is not an error; the area is
 * then read a sector at a time.
 *
 * run is handed to argon2id as it is (argon2.h): NULL runs the lanes in
 * turn on the calling processor. tick is argon2_params.tick, for the
 * loaders' spinner; NULL for none. */
typedef struct {
    void *(*get)(void *ctx, uint32_t kib);
    void  (*put)(void *ctx, void *mem, uint32_t kib);
    void  *ctx;
    const argon2_runner *run;
    void  (*tick)(void);
} luks_kdf_mem;

/* The recovered volume key. Contains key material: crypto_wipe it when the
 * device it opened is closed. */
typedef struct {
    uint8_t  key[LUKS_MAX_KEY];
    uint32_t len;
} luks_master_key;

/* What one keyslot is, for the menu's diagnostics and for explaining a
 * refusal without a second parse. */
typedef struct {
    uint32_t      id;          /* the slot's JSON member name as a number */
    luks_kdf_type kdf;
    uint32_t      kdf_iterations;  /* pbkdf2 only */
    uint32_t      kdf_time;        /* argon2 only: passes */
    uint32_t      kdf_memory_kib;  /* argon2 only: the header's memory */
    uint32_t      kdf_lanes;       /* argon2 only: the header's cpus */
    uint32_t      key_size;    /* bytes of master key stored */
    uint32_t      af_stripes;
    uint8_t       usable;      /* 0 when this loader cannot process it */
} luks_slot_info;

/* Reads and validates both header copies, parses the metadata, and records
 * the data segment. json_buf must be at least the header's JSON area size
 * (LUKS_JSON_CAP covers cryptsetup's default) and stays owned by the caller
 * for the lifetime of v: the parsed views point into it. */
int luks_open(luks_vol *v, luks_read_fn read, void *read_ctx,
              char *json_buf, size_t json_cap);

/* Enumeration, for UX. i runs from 0; returns LUKS_ERR_NOTFOUND-equivalent
 * LUKS_ERR_RANGE past the end. */
int luks_slot_count(const luks_vol *v, size_t *count);
int luks_slot_at(const luks_vol *v, size_t i, luks_slot_info *out);

/* One passphrase attempt across every usable keyslot.
 *
 * On success mk holds the volume key and *slot_id names the keyslot that
 * opened. The master key is only reported after the header's digest object
 * verifies it, so a success here means the passphrase was right, not merely
 * that a decryption produced bytes.
 *
 * mem supplies an argon2id slot's memory; NULL is allowed, and then every
 * argon2id slot is one the caller could not supply.
 *
 * Failure modes worth distinguishing at the UX layer, in the order they
 * take precedence:
 *   LUKS_ERR_KDF_MEMORY       an argon2id slot could not be run for want of
 *                             memory; v->kdf_need_kib is the smallest ask.
 *                             It outranks NOKEY because the passphrase may
 *                             be that slot's, and nothing else can tell
 *   LUKS_ERR_NOKEY            every usable keyslot rejected the passphrase
 *   LUKS_ERR_UNSUPPORTED_KDF  no usable keyslot existed; all were argon2i
 *                             or a KDF nobody here knows
 *   LUKS_ERR_UNSUPPORTED      the cipher or a hash is outside our scope
 *
 * Stack cost is a few kilobytes: the keyslot area is read into a block of
 * mem's, or a LUKS_KEYSLOT_UNIT sector at a time without one, and decrypted
 * straight into the AF merge, so the 250 KB anti-forensic expansion is
 * never on the stack; argon2id works in the caller's block with about
 * 3.5 KiB of its own. */
int luks_unlock(luks_vol *v, const void *pass, size_t pass_len,
                const luks_kdf_mem *mem, luks_master_key *mk,
                uint32_t *slot_id);

/* The falcon identity token (DESIGN Section 3.1), read from the header's
 * JSON with no key:
 *
 *   "tokens": { "<n>": { "type": "falcon", "keyslots": [], "version": 1,
 *                        "role": "shelf", "install_id": "<32 hex>" } }
 *
 * The first token whose type is "falcon" is the one; tokens of any other
 * type are passed over, and members the token does not need are ignored,
 * in any order. *role and *id point into the volume's JSON buffer, as the
 * raw bytes between the quotes (an escape sequence is not decoded, so it
 * reaches ident_from_strings as written and is refused there); they live
 * as long as luks_open's buffer does.
 *
 * LUKS_ERR_NOTFOUND when there is no falcon token; LUKS_ERR_CORRUPT when
 * there is one but "version" is not a JSON integer or "role" or
 * "install_id" is missing or not a string. The values themselves are
 * judged by ident_from_strings, which this does not call, so luks/ does not
 * depend on ident/. */
int luks_falcon_token(const luks_vol *v, uint32_t *version,
                      const char **role, size_t *role_len,
                      const char **id, size_t *id_len);

/* The decrypting reader over the data segment. Holds key material. */
typedef struct {
    luks_vol *vol;
    xts_ctx   xts;
    uint32_t  sector_size;
    uint64_t  sec_cached;     /* plaintext sector index in sec_buf, ~0 none */
    uint8_t   sec_buf[LUKS_MAX_SECTOR];
} luks_dev;

int  luks_dev_open(luks_dev *d, luks_vol *v, const luks_master_key *mk);
void luks_dev_close(luks_dev *d);

/* Reads len plaintext bytes at segment-relative offset off. Signature is
 * exactly luks_read_fn / exfat_read_fn, so &dev passes straight to
 * exfat_mount or an ext4 mount as the read callback and its context. */
int  luks_read(void *ctx, uint64_t off, uint64_t len, void *buf);

#endif /* FALCON_LUKS_H */
