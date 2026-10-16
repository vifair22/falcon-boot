/* falcon-bios: what a boot route reports when it comes back.
 *
 * The UEFI loader's routes report the same three things (falcon-efi's
 * direct_result, bsd_result, win_result, pos_result), and main.c says them
 * the same way: "<route> failed while <stage>", then the advice. There is no
 * status code beside the stage because there is no firmware status to give;
 * what the UEFI loader's hex number tells a reader, the advice says here.
 */

#ifndef FALCON_BIOS_RESULT_H
#define FALCON_BIOS_RESULT_H

#include <media/bootcfg.h>
#include <tui/tui.h>

/* Room for advice built at run time: a sentence and a path on the medium. */
#define BOOT_ADVICE_MAX 256u

typedef struct {
    const char *stage;     /* the step the route was on */
    const char *advice;    /* what to do about it, or NULL */
    char        title[BOOTCFG_TITLE_MAX];  /* a direct boot's entry, or "" */
    char        buf[BOOT_ADVICE_MAX];      /* advice, when it is built */
    /* Set when the route stopped before it committed to the medium, because
     * the medium is not one it can take: the caller goes on to the next
     * route, as the UEFI loader does (docs/Manual.md, Section 2.3). */
    int         fallthrough;
} boot_result;

/* A stage of the route: a step on its page (falcon-core's tui_step), and
 * what a failure names. */
static inline void boot_stage(boot_result *r, const char *stage)
{
    r->stage = stage;
    tui_step_ascii(stage);
}

/* Before a route runs. */
static inline void boot_result_init(boot_result *r)
{
    r->stage = "starting";
    r->advice = (const char *)0;
    r->fallthrough = 0;
    r->title[0] = '\0';
    r->buf[0] = '\0';
}

/* Sets the advice to a, then b, then a full stop, cut to fit. b may be
 * NULL. */
static inline void boot_result_advise(boot_result *r, const char *a,
                                      const char *b)
{
    size_t n = 0;

    while (a != (const char *)0 && *a != '\0' && n + 2u < BOOT_ADVICE_MAX)
        r->buf[n++] = *a++;
    while (b != (const char *)0 && *b != '\0' && n + 2u < BOOT_ADVICE_MAX)
        r->buf[n++] = *b++;
    r->buf[n++] = '.';
    r->buf[n] = '\0';
    r->advice = r->buf;
}

#endif /* FALCON_BIOS_RESULT_H */
