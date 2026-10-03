/* falcon-boot mkstick: the parts of `kdf-bench` that decide rather than
 * measure, split out so the host tests run them (kdfbench.c is the timing
 * and the threads).
 *
 * `kdf-bench` times falcon-core's own argon2id over a grid of memory sizes
 * and pass counts and prints the seconds each cell takes, so a keyslot cost
 * can be chosen for a machine from what the loaders would take to unlock
 * it, not from what cryptsetup's benchmark says Linux would. It recommends
 * nothing: the table is the answer.
 */

#ifndef FALCON_MKSTICK_KDFBENCHCORE_H
#define FALCON_MKSTICK_KDFBENCHCORE_H

#include <stddef.h>
#include <stdint.h>

#define KDFB_MAX_COLS     16u     /* values in one list */
#define KDFB_MEM_MAX_MIB  4096u   /* falcon-bios places nothing at 4 GiB */
#define KDFB_TIME_MAX     64u
#define KDFB_LANES_MAX    64u

enum {
    KDFB_OK        = 0,
    KDFB_ERR_EMPTY = -1,     /* nothing, or an empty item ("1,,2") */
    KDFB_ERR_NUM   = -2,     /* not a decimal number */
    KDFB_ERR_RANGE = -3,     /* outside lo..hi */
    KDFB_ERR_MANY  = -4,     /* more than cap values */
    KDFB_ERR_LEVEL = -5      /* not a level name */
};

/* "64,128,1024": up to cap decimal values in lo..hi into out, in the
 * order given, *n of them. */
int kdfb_parse_list(const char *s, uint32_t lo, uint32_t hi, uint32_t *out,
                    size_t cap, size_t *n);

/* "auto", "portable", "sse2", "ssse3", "avx2" as argon2id_simd takes them
 * (ARGON2_SIMD_AUTO or a CRYPTO_SIMD_*) into *level. */
int kdfb_parse_level(const char *s, int *level);

/* The table's head: "  memory" and a column per pass count. */
void kdfb_format_head(char *buf, size_t cap, const uint32_t *passes,
                      size_t n);

/* One row: mib and a column per cell, seconds to two places; a negative
 * cell is one that was not run ("-"), as when the memory could not be had. */
void kdfb_format_row(char *buf, size_t cap, uint32_t mib, const double *secs,
                     size_t n);

#endif /* FALCON_MKSTICK_KDFBENCHCORE_H */
