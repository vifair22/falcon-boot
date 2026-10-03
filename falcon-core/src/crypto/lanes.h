/* falcon-boot: argon2id's lanes on more than one processor.
 *
 * Purpose. An argon2_runner (argon2.h) whose lanes are taken by every
 * processor in a pool: the one that calls argon2id and any number of
 * workers. The platform's part is only starting the workers and giving each
 * a call to lanes_serve: the UEFI loader through the firmware's MP
 * services, falcon-bios by starting the other processors itself, the host
 * tools and tests with threads. The synchronisation is here, once, which is
 * what lets the host tests run the code the loaders do.
 *
 * Reasoning. The workers are started once for a whole unlock and spin on
 * shared memory between slices rather than being dispatched a slice at a
 * time. A firmware's dispatch is slow to report back (edk2 polls a
 * non-blocking dispatch every 100 ms, and a blocking one leaves the caller
 * idle), and an unlock has four slices a pass. Spinning costs nothing that
 * matters for the second or two an unlock takes.
 *
 * A slice is posted as one 32-bit word, the slice's lane count in the high
 * half and the next lane to take in the low half, and lanes are taken by
 * adding one to it. 32 bits because that is the widest atomic every build
 * has inline: the i586 one would make a 64-bit add a call into libatomic,
 * which a freestanding loader does not link. The low half never reaches
 * the high one: a lane past the last is taken at most once a participant a
 * slice, and a slice of more than LANES_POOLED lanes is not pooled at all
 * but run in turn by the caller. Taking a lane and learning how many there are is then
 * one atomic step: a worker arriving late never pairs a lane number from
 * one slice with another slice's count, which between two keyslots with
 * different lane counts would run a lane twice and derive a wrong key. A
 * taker that gets a valid lane holds it until the lane is counted as
 * finished, and the caller posts nothing new until every lane is, so the
 * job and its context read after the take are that lane's slice's.
 *
 * The workers run whatever argon2id's job is, so a worker's processor must
 * be able to run the block function the caller's level picked: a worker
 * that cannot (lanes_serve's ok argument) serves nothing, and the lanes go
 * to those that can.
 *
 * Interactions. argon2.c's segment job runs on whichever processor takes
 * the lane; the job wipes its own scratch and registers there.
 */

#ifndef FALCON_CRYPTO_LANES_H
#define FALCON_CRYPTO_LANES_H

#include <stdint.h>

#include <crypto/argon2.h>

/* Workers a platform puts in a pool at most: more processors than any
 * keyslot has lanes only spin. cryptsetup's default is 4 lanes. */
#define LANES_MAX 63u

/* The most lanes a slice may have and still be shared out. */
#define LANES_POOLED 0x7fffu

typedef struct {
    uint32_t   slot;      /* lanes << 16 | next lane to take */
    uint32_t   finished;  /* lanes of the posted slice done */
    uint32_t   stop;      /* 1: workers return from lanes_serve */
    uint32_t   tickets;   /* workers admitted so far */
    uint32_t   joined;    /* workers serving */
    uint32_t   workers;   /* admitted at most */
    int        level;     /* the block function level the caller runs */
    argon2_job job;
    void      *ctx;
} lanes_pool;

/* Cost, in KiB of memory times passes, below which an argon2id slot is
 * not worth the workers: starting them and seeing them back costs a
 * firmware up to a tenth of a second (edk2 notices a finished dispatch on
 * a 100 ms timer), which is more than a cheap slot saves. 512 MiB-passes
 * is about a fifth of a second on one current core with AVX2. */
#define LANES_WORTH_KIB_PASSES (512ull * 1024u)

/* Whether a slot of this cost runs faster on a pool: more than one lane,
 * and at least LANES_WORTH_KIB_PASSES. */
int lanes_worth(uint32_t memory_kib, uint32_t passes, uint32_t lanes);

/* A pool admitting at most workers workers, for derivations whose block
 * function is level (crypto_simd_level on the calling processor). */
void lanes_init(lanes_pool *p, uint32_t workers, int level);

/* What each worker runs, on its own processor, once. Returns at once when
 * the pool already has its workers or ok is 0 (this processor cannot run
 * p->level), else serves slices until lanes_stop. 1 when it served. */
int lanes_serve(lanes_pool *p, int ok);

/* The argon2_runner run function; self is the pool. */
void lanes_run(void *self, argon2_job job, void *ctx, uint32_t lanes);

/* Workers serving now. */
uint32_t lanes_joined(const lanes_pool *p);

/* Tells the workers to return. The platform then waits for them by its own
 * means before the pool's memory is reused. */
void lanes_stop(lanes_pool *p);

#endif /* FALCON_CRYPTO_LANES_H */
