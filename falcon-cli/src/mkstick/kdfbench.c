/* falcon-boot mkstick: `kdf-bench`, falcon-core's argon2id timed over a grid
 * of memory sizes and pass counts.
 *
 * The point is the code, not the machine's other options: every cell runs
 * argon2id_simd from falcon-core, the object the loaders link, at the block
 * function level the loaders would pick on this processor (or the one
 * --level names), so the table says what an unlock at the loader's prompt
 * costs. cryptsetup's own benchmark answers for Linux's implementation.
 *
 * Lanes run as the loaders run them. --threads 1 is one after another on
 * one processor, which is what a loader does with no other processor to
 * start. More threads is the loaders' own lane pool (crypto/lanes.c), its
 * workers threads here where they are processors there, the calling
 * thread taking lanes too.
 *
 * The memory is allocated once, at the largest size asked for, and touched
 * before the first cell, so the kernel's first-touch page faults are not
 * timed: a loader's memory comes from the firmware or the E820 map already
 * mapped. Each cell is the fastest of --repeat runs.
 */

#include <mkstick/mkstick.h>
#include <mkstick/kdfbenchcore.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <common/log.h>
#include <crypto/argon2.h>
#include <crypto/crypto.h>
#include <crypto/lanes.h>

#define DEFAULT_MEMORY  "64,128,256,512,1024"
#define DEFAULT_PASSES  "1,2,4,8"
#define DEFAULT_LANES   4u

/* ---- the workers ---- */

/* The loaders' pool (crypto/lanes.c), its workers threads here where they
 * are processors there. */
typedef struct {
    lanes_pool pool;
    pthread_t  th[LANES_MAX];
    unsigned   nth;
} bench_pool;

static void *worker(void *arg)
{
    (void)lanes_serve((lanes_pool *)arg, 1);
    return NULL;
}

static void pool_stop(bench_pool *bp)
{
    unsigned i;

    lanes_stop(&bp->pool);
    for (i = 0; i < bp->nth; i++)
        (void)pthread_join(bp->th[i], NULL);
    bp->nth = 0;
}

static cli_status pool_start(bench_pool *bp, unsigned workers, int level)
{
    unsigned i;

    bp->nth = 0;
    lanes_init(&bp->pool, workers, level);
    for (i = 0; i < workers; i++) {
        int e = pthread_create(&bp->th[i], NULL, worker, &bp->pool);

        if (e != 0) {
            pool_stop(bp);
            errno = e;
            return set_error_errno(CLI_ERR_INTERNAL, "starting thread %u",
                                   i + 1u);
        }
        bp->nth++;
    }
    return CLI_OK;
}

/* ---- the command ---- */

static const cli_opt bench_opts[] = {
    { "memory", CLI_OPT_VALUE, 0, "<MiB,...>",
      "memory sizes, the table's rows (default " DEFAULT_MEMORY ")" },
    { "time", CLI_OPT_VALUE, 0, "<n,...>",
      "pass counts, its columns (default " DEFAULT_PASSES ")" },
    { "lanes", CLI_OPT_VALUE, 0, "<n>",
      "lanes, cryptsetup's --pbkdf-parallel (default 4)" },
    { "threads", CLI_OPT_VALUE, 0, "<n>",
      "threads the lanes run on; 1 is one after another (default: the "
      "lanes, at most this machine's processors)" },
    { "level", CLI_OPT_VALUE, 0, "<name>",
      "block function: auto, portable, sse2, ssse3, avx2 (default auto)" },
    { "repeat", CLI_OPT_VALUE, 0, "<n>",
      "runs a cell, the fastest kept (default 1)" }
};

static cli_status list_opt(const cli_args *a, const char *name,
                           const char *dflt, uint32_t lo, uint32_t hi,
                           uint32_t *out, size_t *n)
{
    int rc = kdfb_parse_list(cli_value(a, name, dflt), lo, hi, out,
                             KDFB_MAX_COLS, n);

    if (rc == KDFB_OK)
        return CLI_OK;
    return set_error(CLI_ERR_USAGE, "--%s: %s", name,
                     rc == KDFB_ERR_RANGE ? "a value is out of range" :
                     rc == KDFB_ERR_MANY ? "too many values" :
                     "expected decimal numbers separated by commas");
}

static double now_s(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static cli_status bench_run(const cli_args *a)
{
    uint32_t mem[KDFB_MAX_COLS], passes[KDFB_MAX_COLS];
    uint32_t lanes, threads, repeat, max_kib = 0;
    size_t nmem, npass, i, j;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int level, eff;
    argon2_runner runner = { lanes_run, NULL };
    bench_pool bp;
    void *block = NULL;
    char line[256];
    cli_status rc;
    size_t one;

    rc = list_opt(a, "memory", DEFAULT_MEMORY, 1u, KDFB_MEM_MAX_MIB, mem,
                  &nmem);
    if (rc == CLI_OK)
        rc = list_opt(a, "time", DEFAULT_PASSES, 1u, KDFB_TIME_MAX, passes,
                      &npass);
    if (rc == CLI_OK)
        rc = list_opt(a, "lanes", "4", 1u, KDFB_LANES_MAX, &lanes, &one);
    if (rc == CLI_OK)
        rc = list_opt(a, "repeat", "1", 1u, 100u, &repeat, &one);
    if (rc != CLI_OK)
        return rc;
    if (ncpu < 1)
        ncpu = 1;
    threads = lanes < (uint32_t)ncpu ? lanes : (uint32_t)ncpu;
    if (cli_value(a, "threads", NULL) != NULL) {
        rc = list_opt(a, "threads", NULL, 1u, LANES_MAX + 1u, &threads,
                      &one);
        if (rc != CLI_OK)
            return rc;
    }
    if (kdfb_parse_level(cli_value(a, "level", "auto"), &level) != KDFB_OK)
        return set_error(CLI_ERR_USAGE, "--level: auto, portable, sse2, "
                         "ssse3 or avx2");
    eff = (level == ARGON2_SIMD_AUTO) ? crypto_simd_level() : level;
    if (eff > crypto_simd_level())
        return set_error(CLI_ERR_INVALID, "--level %s: this processor "
                         "does not run it", crypto_simd_name(level));

    for (i = 0; i < nmem; i++) {
        uint32_t kib = argon2_mem_kib(mem[i] * 1024u, lanes);

        if (kib == 0)
            return set_error(CLI_ERR_USAGE, "--memory %lu MiB is too little "
                             "for %lu lanes", (unsigned long)mem[i],
                             (unsigned long)lanes);
        if (kib > max_kib)
            max_kib = kib;
    }
    if (posix_memalign(&block, 64, (size_t)max_kib * 1024u) != 0)
        return set_error(CLI_ERR_NOMEM, "%lu MiB for the largest row",
                         (unsigned long)(max_kib / 1024u));
    memset(block, 0x5a, (size_t)max_kib * 1024u);

    if (threads > 1u) {
        rc = pool_start(&bp, threads - 1u, eff);
        if (rc != CLI_OK) {
            free(block);
            return rc;
        }
        runner.self = &bp.pool;
    }

    (void)printf("argon2id in falcon-core: %s block function, %lu lanes on "
                 "%lu thread%s\n", crypto_simd_name(eff),
                 (unsigned long)lanes, (unsigned long)threads,
                 threads == 1u ? " (one after another)" : "s");
    (void)printf("seconds an unlock attempt takes; rows memory, columns "
                 "passes\n\n");
    kdfb_format_head(line, sizeof line, passes, npass);
    (void)printf("%s\n", line);
    (void)fflush(stdout);

    for (i = 0; i < nmem && rc == CLI_OK; i++) {
        double secs[KDFB_MAX_COLS] = { 0 };

        for (j = 0; j < npass; j++) {
            argon2_params ap;
            uint8_t out[32];
            uint32_t r;
            double best = -1.0;

            memset(&ap, 0, sizeof ap);
            ap.pwd = "kdf-bench";
            ap.pwd_len = 9;
            ap.salt = "falcon-kdf-bench";
            ap.salt_len = 16;
            ap.t_cost = passes[j];
            ap.m_cost = mem[i] * 1024u;
            ap.lanes = lanes;
            for (r = 0; r < repeat; r++) {
                double t0 = now_s(), dt;
                int e = argon2id_simd(&ap, block, max_kib,
                                      threads > 1u ? &runner : NULL, level,
                                      out, sizeof out);

                dt = now_s() - t0;
                if (e != CRYPTO_OK) {
                    rc = set_error(CLI_ERR_INTERNAL, "argon2id failed (%d) "
                                   "at %lu MiB, t=%lu", e,
                                   (unsigned long)mem[i],
                                   (unsigned long)passes[j]);
                    break;
                }
                if (best < 0.0 || dt < best)
                    best = dt;
            }
            secs[j] = best;
        }
        if (rc != CLI_OK)
            break;
        kdfb_format_row(line, sizeof line, mem[i], secs, npass);
        (void)printf("%s\n", line);
        (void)fflush(stdout);
    }

    if (rc == CLI_OK)
        (void)printf("\ncryptsetup luksAddKey --pbkdf argon2id "
                     "--pbkdf-memory <MiB x 1024> --pbkdf-force-iterations "
                     "<t> --pbkdf-parallel %lu\n", (unsigned long)lanes);
    if (threads > 1u)
        pool_stop(&bp);
    free(block);
    return rc;
}

const cli_cmd mks_cmd_kdf_bench = {
    "kdf-bench",
    "time falcon-core's argon2id over memory sizes and pass counts",
    "",
    bench_opts,
    sizeof bench_opts / sizeof bench_opts[0],
    0, 0,
    "Prints the seconds one argon2id derivation takes in falcon-core, the\n"
    "code both loaders unlock a shelf with, for each memory size (rows) and\n"
    "pass count (columns). --threads 1 is one lane after another; the\n"
    "default runs a lane a thread, up to this machine's processors.\n",
    bench_run
};
