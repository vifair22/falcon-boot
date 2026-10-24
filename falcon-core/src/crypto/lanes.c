/* falcon-boot: argon2id's lanes on more than one processor. See lanes.h.
 *
 * Every shared field is read and written with the __atomic builtins, never
 * plainly: a worker spins on them while another processor writes, and the
 * orderings below are what make a slice's job, context and memory visible
 * to whoever takes one of its lanes. */

#include <crypto/lanes.h>

#include <stddef.h>

static void relax(void)
{
#if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

int lanes_worth(uint32_t memory_kib, uint32_t passes, uint32_t lanes)
{
    return lanes > 1u &&
           (uint64_t)memory_kib * passes >= LANES_WORTH_KIB_PASSES;
}

void lanes_init(lanes_pool *p, uint32_t workers, int level)
{
    p->slot = 0;
    p->finished = 0;
    p->stop = 0;
    p->tickets = 0;
    p->joined = 0;
    p->workers = workers;
    p->level = level;
    p->job = NULL;
    p->ctx = NULL;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Takes lanes of whatever slice is posted until none is left; `caller` is
 * ARGON2_LANE_CALLER when this is the processor that called argon2id. */
static void take(lanes_pool *p, uint32_t caller)
{
    for (;;) {
        uint32_t v = __atomic_fetch_add(&p->slot, 1u, __ATOMIC_ACQ_REL);
        uint32_t lane = v & 0xffffu;
        uint32_t lanes = v >> 16;
        argon2_job job;
        void *ctx;

        if (lane >= lanes)
            return;
        job = __atomic_load_n(&p->job, __ATOMIC_ACQUIRE);
        ctx = __atomic_load_n(&p->ctx, __ATOMIC_ACQUIRE);
        job(ctx, lane | caller);
        (void)__atomic_add_fetch(&p->finished, 1u, __ATOMIC_RELEASE);
    }
}

int lanes_serve(lanes_pool *p, int ok)
{
    uint32_t ticket;

    if (!ok)
        return 0;
    ticket = __atomic_fetch_add(&p->tickets, 1u, __ATOMIC_ACQ_REL);
    if (ticket >= p->workers)
        return 0;
    (void)__atomic_add_fetch(&p->joined, 1u, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE)) {
        uint32_t v = __atomic_load_n(&p->slot, __ATOMIC_ACQUIRE);

        /* Lanes left in the posted slice: take them. Otherwise only this
         * load runs, so a waiting worker never writes the word, and the
         * count of lanes taken past the last one stays small. */
        if ((v & 0xffffu) < (v >> 16))
            take(p, 0);
        else
            relax();
    }
    return 1;
}

void lanes_run(void *self, argon2_job job, void *ctx, uint32_t lanes)
{
    lanes_pool *p = (lanes_pool *)self;
    uint32_t l;

    if (lanes > LANES_POOLED) {
        for (l = 0; l < lanes; l++)
            job(ctx, l | ARGON2_LANE_CALLER);
        return;
    }
    __atomic_store_n(&p->job, job, __ATOMIC_RELEASE);
    __atomic_store_n(&p->ctx, ctx, __ATOMIC_RELEASE);
    __atomic_store_n(&p->finished, 0u, __ATOMIC_RELEASE);
    /* Last: the post. Everything above, and every block the previous
     * slices wrote, is visible to whoever takes from the new word. */
    __atomic_store_n(&p->slot, lanes << 16, __ATOMIC_SEQ_CST);

    take(p, ARGON2_LANE_CALLER);
    while (__atomic_load_n(&p->finished, __ATOMIC_ACQUIRE) != lanes)
        relax();
}

uint32_t lanes_joined(const lanes_pool *p)
{
    return __atomic_load_n(&p->joined, __ATOMIC_ACQUIRE);
}

void lanes_stop(lanes_pool *p)
{
    __atomic_store_n(&p->stop, 1u, __ATOMIC_RELEASE);
}
