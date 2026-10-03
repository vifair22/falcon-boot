/* falcon-boot: argon2id's lanes on the other processors. See mp.h. */

#include <mp/mp.h>

#include <crypto/crypto.h>
#include <crypto/xstate.h>

/* On each application processor the firmware starts: a worker if mpcore
 * picked it and it can run the boot processor's level, else back at once.
 * WhoAmI is asked of the protocol the boot processor located, which every
 * AP may call. */
static void EFIAPI ap_proc(void *arg)
{
    mp_lanes *m = (mp_lanes *)arg;
    xstate_saved xs;
    UINTN me = 0;

    if (m->mp->WhoAmI(m->mp, &me) != EFI_SUCCESS || me >= MP_TABLE_MAX ||
        !m->worker[me])
        return;
    xstate_borrow(&xs);
    (void)lanes_serve(&m->pool, crypto_simd_level() >= m->pool.level);
    xstate_return(&xs);
}

const argon2_runner *mp_lanes_start(EFI_BOOT_SERVICES *bs, mp_lanes *m,
                                    int level)
{
    EFI_GUID guid = EFI_MP_SERVICES_PROTOCOL_GUID;
    mp_cpu cpus[MP_TABLE_MAX];
    UINTN n = 0, enabled = 0, i;

    m->mp = (EFI_MP_SERVICES_PROTOCOL *)0;
    m->done = (EFI_EVENT)0;
    m->workers = 0;
    m->started = 0;
    if (bs->LocateProtocol(&guid, (void *)0, (void **)&m->mp) != EFI_SUCCESS ||
        m->mp == (EFI_MP_SERVICES_PROTOCOL *)0 ||
        m->mp->GetNumberOfProcessors(m->mp, &n, &enabled) != EFI_SUCCESS ||
        enabled < 2u)
        return (const argon2_runner *)0;
    if (n > MP_TABLE_MAX)
        n = MP_TABLE_MAX;
    for (i = 0; i < n; i++) {
        EFI_PROCESSOR_INFORMATION info;

        if (m->mp->GetProcessorInfo(m->mp, i, &info) != EFI_SUCCESS) {
            cpus[i].bsp = 0;
            cpus[i].usable = 0;
            cpus[i].package = cpus[i].core = cpus[i].thread = 0;
            continue;
        }
        cpus[i].package = info.Location.Package;
        cpus[i].core = info.Location.Core;
        cpus[i].thread = info.Location.Thread;
        cpus[i].bsp = (info.StatusFlag & PROCESSOR_AS_BSP_BIT) != 0u;
        cpus[i].usable =
            (info.StatusFlag & (PROCESSOR_ENABLED_BIT |
                                PROCESSOR_HEALTH_STATUS_BIT)) ==
            (PROCESSOR_ENABLED_BIT | PROCESSOR_HEALTH_STATUS_BIT);
    }
    m->workers = mp_pick_workers(cpus, n, m->worker);
    if (m->workers == 0)
        return (const argon2_runner *)0;

    lanes_init(&m->pool, m->workers, level);
    m->runner.run = lanes_run;
    m->runner.self = &m->pool;
    /* Not blocking: the boot processor takes lanes too, and a blocking
     * StartupAllAPs would hold it until the workers return, which they
     * do only at mp_lanes_stop. */
    if (bs->CreateEvent(0u, (EFI_TPL)0, (EFI_EVENT_NOTIFY)0, (void *)0,
                        &m->done) != EFI_SUCCESS)
        return (const argon2_runner *)0;
    if (m->mp->StartupAllAPs(m->mp, ap_proc, FALSE, m->done, 0u, m,
                             (UINTN **)0) != EFI_SUCCESS) {
        (void)bs->CloseEvent(m->done);
        return (const argon2_runner *)0;
    }
    m->started = 1;
    return &m->runner;
}

void mp_lanes_stop(EFI_BOOT_SERVICES *bs, mp_lanes *m)
{
    UINTN index = 0;

    if (!m->started)
        return;
    lanes_stop(&m->pool);
    (void)bs->WaitForEvent(1u, &m->done, &index);
    (void)bs->CloseEvent(m->done);
    m->started = 0;
}

uint32_t mp_lanes_count(const mp_lanes *m)
{
    return 1u + (m->started ? lanes_joined(&m->pool) : 0u);
}
