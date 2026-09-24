/* The firmware half of the NXT rows. See nxt.h, and nxtcore.h for the
 * parsing. */

#include <nxt/nxt.h>
#include <loader/loadops.h>

#define GLOBAL_VARIABLE_GUID \
    { 0x8be4df61, 0x93ca, 0x11d2, \
      { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } }

/* BootOrder has one entry per option the firmware knows, OS or not; this is
 * the most read, and the rest are ignored. */
#define ORDER_MAX 64u

/* A load option is a header, a description and a device path; a kilobyte
 * holds any real one. A larger one is skipped rather than truncated, since
 * nxt_option_parse would refuse the cut copy anyway. */
#define OPTION_MAX 1024u

static void option_name(CHAR16 name[9], UINT16 option)
{
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    UINTN i;

    name[0] = L'B';
    name[1] = L'o';
    name[2] = L'o';
    name[3] = L't';
    for (i = 0; i < 4u; i++)
        name[4u + i] = hex[(option >> (12u - 4u * i)) & 0xFu];
    name[8] = 0;
}

UINTN nxt_scan(EFI_SYSTEM_TABLE *st, nxt_row *rows, UINTN cap)
{
    static UINT8 order_raw[ORDER_MAX * 2u];
    static UINT8 opt[OPTION_MAX];
    UINT16 order[ORDER_MAX];
    EFI_GUID g = GLOBAL_VARIABLE_GUID;
    UINT16 current = 0;
    UINTN size;
    UINTN n = 0;
    UINTN count;
    UINTN i;
    BOOLEAN have_current;

    if (st == (EFI_SYSTEM_TABLE *)0 || rows == (nxt_row *)0 || cap == 0)
        return 0;

    /* BootOrder longer than the buffer comes back BUFFER_TOO_SMALL with
     * nothing read; the options past ORDER_MAX are the least preferred, and
     * the firmware's own menu still has them. */
    size = sizeof order_raw;
    if (st->RuntimeServices->GetVariable(L"BootOrder", &g, (UINT32 *)0, &size,
                                         order_raw) != EFI_SUCCESS)
        return 0;
    count = nxt_order_parse(order_raw, size, order, ORDER_MAX);

    /* This boot's own entry is falcon; offering it would be a row that
     * reboots into this screen. */
    size = sizeof current;
    have_current = (st->RuntimeServices->GetVariable(
                        L"BootCurrent", &g, (UINT32 *)0, &size, &current) ==
                        EFI_SUCCESS &&
                    size == sizeof current)
                       ? TRUE
                       : FALSE;

    for (i = 0; i < count && n < cap; i++) {
        CHAR16 name[9];
        uint32_t attrs = 0;

        if (have_current && order[i] == current)
            continue;
        option_name(name, order[i]);
        size = sizeof opt;
        if (st->RuntimeServices->GetVariable(name, &g, (UINT32 *)0, &size,
                                             opt) != EFI_SUCCESS)
            continue;
        if (nxt_option_parse(opt, size, &attrs, rows[n].label) != NXT_OK)
            continue;
        if (!nxt_option_offered(attrs))
            continue;
        if (rows[n].label[0] == 0) {
            UINTN k;

            for (k = 0; k < 9u; k++)
                rows[n].label[k] = name[k];
        }
        rows[n].option = order[i];
        n++;
    }
    return n;
}

EFI_STATUS nxt_boot(EFI_SYSTEM_TABLE *st, const nxt_row *row,
                    const CHAR16 **stage)
{
    EFI_STATUS status;

    *stage = L"setting BootNext";
    status = loader_boot_next_set(st, row->option);
    if (EFI_ERROR(status))
        return status;

    *stage = L"restarting";
    st->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0,
                                     (void *)0);
    /* Not reached on any firmware that implements ResetSystem. One that
     * returns has left BootNext set, which is undone so an unrelated
     * reboot later does not land on this entry. */
    loader_boot_next_clear(st);
    return EFI_DEVICE_ERROR;
}
