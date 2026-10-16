/* falcon-boot: Argon2id (RFC 9106, version 0x13).
 *
 * Purpose. The KDF of a LUKS2 keyslot made the ordinary way: `cryptsetup
 * luksFormat` defaults to argon2id, and a station's shelf is made by
 * cryptsetup on the machine (DESIGN Section 3). luks.c
 * derives the keyslot key with this.
 *
 * Reasoning. The module never allocates. Argon2's cost is its memory, tens
 * of megabytes to a gigabyte, and where that comes from is the one thing the
 * three environments disagree on: the UEFI loader asks the firmware for
 * pages, falcon-bios takes a run of its E820 map, a host tool mallocs. So the
 * caller hands over the block, argon2_mem_kib says how big it must be, and
 * this file only fills it.
 *
 * Lanes. Every lane's segment of slice s is filled before any lane's
 * segment of slice s+1, which is all the specification's synchronisation
 * points ask: a segment only references other lanes' blocks from slices
 * already finished, so within a slice the lanes are independent and may run
 * in any order or at once. How they run is the caller's (argon2_runner): in
 * turn on one processor when it gives none, one lane a processor where the
 * platform can start others. The module has no idea of a processor; the
 * runner is the one place that does, which keeps this file the same in the
 * loaders, the host tools and the benchmark.
 *
 * Block function. G, the compression every block goes through, comes in
 * portable C and in SSE2, SSSE3 and AVX2 (argon2_simd.h), one picked per
 * derivation by what the processor can run (crypto_simd_level). The output
 * is the same whichever runs; only the time differs.
 *
 * Stack. One 1 KiB block in argon2id, and five in each segment's job
 * (argon2_job), about 5.5 KiB on whichever stack runs it: the EFI build's
 * 32 KiB frame limit, falcon-bios's stage 3 stack and the 32 KiB edk2 gives
 * an application processor all have room, and nothing here scales with the
 * cost parameters.
 *
 * Interactions. Built on blake2b.c; called from falcon-core/src/luks/luks.c.
 */

#ifndef FALCON_CRYPTO_ARGON2_H
#define FALCON_CRYPTO_ARGON2_H

#include <stdint.h>
#include <stddef.h>

#define ARGON2_BLOCK_BYTES 1024u
#define ARGON2_VERSION     0x13u
#define ARGON2_MIN_SALT    8u        /* the reference implementation's floor */
#define ARGON2_MIN_OUT     4u
#define ARGON2_MAX_LANES   0xffffffu

/* The inputs of RFC 9106 Section 3.1. secret (K) and ad (X) may be NULL
 * with a zero length; LUKS2 uses neither. m_cost is in KiB, one block each. */
typedef struct {
    const void *pwd;
    size_t      pwd_len;
    const void *salt;
    size_t      salt_len;
    const void *secret;
    size_t      secret_len;
    const void *ad;
    size_t      ad_len;
    uint32_t    t_cost;       /* passes, >= 1 */
    uint32_t    m_cost;       /* KiB, >= 8 * lanes */
    uint32_t    lanes;        /* parallelism p, 1..2^24-1 */
    /* Called every few dozen blocks while the derivation runs, and only on
     * the processor that called argon2id: the loaders turn a spinner from
     * it (DESIGN Section 7.10). NULL for none. */
    void      (*tick)(void);
} argon2_params;

/* The memory argon2id fills, in KiB: m_cost rounded down to a multiple of
 * 4 * lanes (m' in the RFC). 0 when m_cost or lanes are out of range. */
uint32_t argon2_mem_kib(uint32_t m_cost, uint32_t lanes);

/* One segment: the job for lane `lane` of the slice a runner was handed.
 * ctx is argon2id's and opaque to the runner. A runner adds
 * ARGON2_LANE_CALLER to the lane when it runs the job on the processor that
 * called argon2id, the one place argon2_params.tick may run; a lane is at
 * most 24 bits, so the flag is clear of it. */
typedef void (*argon2_job)(void *ctx, uint32_t lane);

#define ARGON2_LANE_CALLER 0x80000000u

/* How one slice's segments are run.
 *
 * run calls job(ctx, l) exactly once for every l in 0..lanes-1, in any
 * order, on any processor, and returns only once every call has returned;
 * l carries ARGON2_LANE_CALLER where the call is on the calling processor.
 * Exactly once is not a nicety: after the first pass a segment XORs into
 * the blocks it fills, so a lane run twice is a wrong key, not a slow one.
 * The runner also owns memory ordering: what was written before run is
 * visible to every job, and what every job wrote is visible to the caller
 * when run returns. self is the runner's own state. */
typedef struct {
    void (*run)(void *self, argon2_job job, void *ctx, uint32_t lanes);
    void  *self;
} argon2_runner;

/* Derives out_len bytes (>= 4) into out.
 *
 * run says how a slice's lanes are run; NULL runs them in turn on the
 * calling processor. Any runner gives the same output.
 *
 * mem is the caller's, at least argon2_mem_kib(p->m_cost, p->lanes) KiB,
 * mem_kib says how much it is, and it must be 8-byte aligned. It is wiped
 * before return on every path that wrote to it, so the caller frees it as
 * it is.
 *
 * CRYPTO_OK; CRYPTO_ERR_BAD_ARG for parameters the RFC rules out;
 * CRYPTO_ERR_NOSPACE when mem_kib is short; CRYPTO_ERR_UNSUPPORTED when the
 * memory would not fit this build's address space. */
int argon2id(const argon2_params *p, void *mem, uint32_t mem_kib,
             const argon2_runner *run, uint8_t *out, size_t out_len);

/* argon2id with the block function's instruction set named: a
 * CRYPTO_SIMD_* level (crypto.h), or ARGON2_SIMD_AUTO for the highest this
 * processor can run now, which is what argon2id uses. Every level gives the
 * same output; naming one is for the tests, which run each the build host
 * has, and the benchmark, which times them. A level above what the
 * processor can run is CRYPTO_ERR_UNSUPPORTED, before mem is touched. */
#define ARGON2_SIMD_AUTO (-1)

int argon2id_simd(const argon2_params *p, void *mem, uint32_t mem_kib,
                  const argon2_runner *run, int simd, uint8_t *out,
                  size_t out_len);

#endif /* FALCON_CRYPTO_ARGON2_H */
