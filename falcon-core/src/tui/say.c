/* falcon-core/src/tui: the sentences every loader says. See tui.h.
 *
 * One place for the words of the screens both loaders show, so a sentence
 * changed is changed on every loader at once.
 */

#include <tui/internal.h>

static const uint16_t k_continue[] = u"press any key to continue.";
static const uint16_t k_for_menu[] = u"press any key for the menu.";
static const uint16_t k_no_default[] = u"The menu will wait with no default.";

/* "<route> failed", into dst. */
static void failed_title(uint16_t *dst, size_t cap, const uint16_t *route,
                         const char *route_a)
{
    static const uint16_t tail[] = u" failed";
    size_t n = 0;
    size_t i;

    for (i = 0; route != (const uint16_t *)0 && route[i] != 0 &&
                n + 8u < cap; i++)
        dst[n++] = route[i];
    for (i = 0; route_a != (const char *)0 && route_a[i] != '\0' &&
                n + 8u < cap; i++)
        dst[n++] = (uint16_t)(unsigned char)route_a[i];
    for (i = 0; tail[i] != 0; i++)
        dst[n++] = tail[i];
    dst[n] = 0;
}

void tui_route_failed(const uint16_t *label, const uint16_t *route,
                      const uint16_t *stage, uint64_t status,
                      int with_status, const uint16_t *advice)
{
    uint16_t title[64];

    failed_title(title, sizeof title / sizeof title[0], route,
                 (const char *)0);
    tui_report(TUI_FAIL, title, label);
    tui_report_failed(route, stage, status, with_status);
    tui_report_advice(advice);
    tui_report_end((const uint16_t *)0);
}

void tui_report_failed_a(const char *what, const char *stage)
{
    tui_style(TUI_PLAIN);
    tui_out_ascii(what);
    tui_out(u" failed while ");
    tui_out_ascii(stage != (const char *)0 ? stage : "starting");
    tui_out(u".\n");
}

void tui_route_failed_a(const uint16_t *label, const char *route,
                        const char *stage, const char *advice)
{
    uint16_t title[64];

    failed_title(title, sizeof title / sizeof title[0], (const uint16_t *)0,
                 route);
    tui_report(TUI_FAIL, title, label);
    tui_report_failed_a(route, stage);
    if (advice != (const char *)0) {
        tui_out(u"\n");
        tui_style(TUI_DIM);
        tui_out_ascii(advice);
        tui_out(u"\n");
        tui_style(TUI_PLAIN);
    }
    tui_report_end((const uint16_t *)0);
}

void tui_route_exited(const uint16_t *label, const uint16_t *what,
                      uint64_t status, const uint16_t *advice)
{
    tui_report(TUI_INFO, u"returned", label);
    tui_report_exited(what, status);
    tui_report_advice(advice);
    tui_report_end((const uint16_t *)0);
}

int tui_fw_setup_ask(void)
{
    return tui_ask(u"Firmware setup",
                   u"Reboot into this machine's firmware setup?",
                   u"y to reboot, any other key to go back.") == 'y';
}

void tui_fw_setup_refused(void)
{
    tui_report(TUI_INFO, u"Firmware setup", (const uint16_t *)0);
    tui_out(u"The firmware refused the request, so nothing was changed. "
            u"Use its own key at power-on instead.\n");
    tui_report_end(k_for_menu);
    (void)tui_wait();
}

void tui_notice(const uint16_t *title, const uint16_t *text)
{
    tui_report(TUI_INFO, title, (const uint16_t *)0);
    tui_out(text);
    tui_out(u"\n");
    tui_report_end(k_continue);
    (void)tui_wait();
}

void tui_notice_a(const uint16_t *title, const char *text)
{
    tui_report(TUI_INFO, title, (const uint16_t *)0);
    tui_out_ascii(text);
    tui_out(u"\n");
    tui_report_end(k_continue);
    (void)tui_wait();
}

/* A STATION.INI notice's ending. */
static void no_default(void)
{
    tui_out(u"\n\n");
    tui_out(k_no_default);
    tui_out(u"\n");
    tui_report_end(k_continue);
    (void)tui_wait();
}

void tui_ini_unread(int too_big)
{
    tui_report(TUI_INFO, u"STATION.INI", (const uint16_t *)0);
    tui_out(too_big ? u"STATION.INI is larger than 16 KB and was not read."
                    : u"STATION.INI could not be read.");
    no_default();
}

void tui_ini_line(const char *file, unsigned line, const char *why)
{
    uint16_t title[16];
    size_t i;

    for (i = 0; file[i] != '\0' && i + 1u < sizeof title / sizeof title[0];
         i++)
        title[i] = (uint16_t)(unsigned char)file[i];
    title[i] = 0;
    tui_report(TUI_INFO, title, (const uint16_t *)0);
    tui_out_ascii(file);
    tui_out(u" line ");
    tui_out_dec(line);
    tui_out(u": ");
    tui_out_ascii(why);
    tui_out(u".");
    no_default();
}

void tui_ini_default(const char *name)
{
    tui_report(TUI_INFO, u"STATION.INI", (const uint16_t *)0);
    tui_out(u"STATION.INI's default, ");
    tui_out_ascii(name);
    tui_out(u", names no entry on this machine.");
    no_default();
}

void tui_next_unknown(const char *name)
{
    tui_report(TUI_INFO, u"Next boot", (const uint16_t *)0);
    tui_out(u"the next-boot marker names ");
    tui_out_ascii(name);
    tui_out(u", and no entry has that name.\n"
            u"It has been cleared; nothing was booted from it.\n");
    tui_report_end(k_for_menu);
    (void)tui_wait();
}

void tui_next_note(const char *note, int cleared)
{
    tui_report(TUI_INFO, u"Next boot", (const uint16_t *)0);
    tui_out_ascii(note);
    tui_out(u"\n");
    if (cleared)
        tui_out(u"It has been cleared; nothing was booted from it.\n");
    tui_report_end(k_for_menu);
    (void)tui_wait();
}

void tui_leaving(int nothing)
{
    tui_page(u"falcon-boot", u"");
    tui_out(u"falcon-boot: returning to firmware.\n");
    if (nothing)
        tui_out(u"note: no personal OS shelf and no other boot entry was "
                u"found.\n");
}
