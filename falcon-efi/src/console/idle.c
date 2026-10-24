/* falcon-boot: the idle power-off. See idle.h. */

#include <console/idle.h>

static EFI_SYSTEM_TABLE *g_st;
static EFI_EVENT g_ev;
static UINT64 g_100ns;
static void (*g_hook)(void);

void idle_start(EFI_SYSTEM_TABLE *st, UINT32 seconds)
{
    g_st = st;
    if (seconds == 0) {
        if (g_ev != (EFI_EVENT)0) {
            st->BootServices->SetTimer(g_ev, TimerCancel, 0);
            st->BootServices->CloseEvent(g_ev);
            g_ev = (EFI_EVENT)0;
        }
        return;
    }
    g_100ns = (UINT64)seconds * 10000000u;
    if (g_ev == (EFI_EVENT)0 &&
        st->BootServices->CreateEvent(EVT_TIMER, 0, (EFI_EVENT_NOTIFY)0,
                                      (void *)0, &g_ev) != EFI_SUCCESS) {
        g_ev = (EFI_EVENT)0;
        return;
    }
    idle_touch();
}

void idle_set_hook(void (*fn)(void))
{
    g_hook = fn;
}

void idle_touch(void)
{
    if (g_ev == (EFI_EVENT)0)
        return;
    /* A timer that fired and was not waited on stays signalled through
     * SetTimer; CheckEvent is what clears it. */
    (void)g_st->BootServices->CheckEvent(g_ev);
    (void)g_st->BootServices->SetTimer(g_ev, TimerRelative, g_100ns);
}

EFI_EVENT idle_event(void)
{
    return g_ev;
}

void idle_expired(void)
{
    if (g_hook != (void (*)(void))0)
        g_hook();
    g_st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0,
                                       (void *)0);
    idle_touch();
}

void idle_power_off(void)
{
    if (g_hook != (void (*)(void))0)
        g_hook();
    g_st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0,
                                       (void *)0);
    g_st->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0,
                                       (void *)0);
    for (;;) {
#if defined(__aarch64__)
        __asm__ __volatile__("wfi");
#else
        __asm__ __volatile__("hlt");
#endif
    }
}
