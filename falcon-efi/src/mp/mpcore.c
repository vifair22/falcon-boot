/* falcon-boot: which processors take argon2id lanes. See mpcore.h. */

#include <mp/mpcore.h>

/* Whether cpus[j], j < i, is on the same core as cpus[i]. */
static int core_seen(const mp_cpu *cpus, size_t i)
{
    size_t j;

    for (j = 0; j < i; j++) {
        if (cpus[j].package == cpus[i].package &&
            cpus[j].core == cpus[i].core)
            return 1;
    }
    return 0;
}

/* Whether a worker already picked, cpus[j] for j < i, is on cpus[i]'s
 * core. */
static int core_taken(const mp_cpu *cpus, const uint8_t *worker, size_t i)
{
    size_t j;

    for (j = 0; j < i; j++) {
        if (worker[j] && cpus[j].package == cpus[i].package &&
            cpus[j].core == cpus[i].core)
            return 1;
    }
    return 0;
}

uint32_t mp_pick_workers(const mp_cpu *cpus, size_t n, uint8_t *worker)
{
    const mp_cpu *bsp = NULL;
    uint32_t count = 0;
    size_t i;
    int distinct = 0;

    for (i = 0; i < n; i++) {
        worker[i] = 0;
        if (cpus[i].bsp)
            bsp = &cpus[i];
    }
    /* The topology tells cores apart when two processors report different
     * ones; a firmware that reports none puts every processor on one. */
    for (i = 1; i < n && !distinct; i++)
        distinct = !core_seen(cpus, i);

    for (i = 0; i < n && count < MP_WORKERS_MAX; i++) {
        const mp_cpu *c = &cpus[i];

        if (c->bsp || !c->usable)
            continue;
        if (distinct) {
            if (bsp != NULL && c->package == bsp->package &&
                c->core == bsp->core)
                continue;
            if (core_taken(cpus, worker, i))
                continue;
        }
        worker[i] = 1;
        count++;
    }
    return count;
}
