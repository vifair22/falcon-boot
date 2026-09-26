/* falcon jump: JUMP.INI, the floppy's optional default and countdown
 * (docs/DESIGN.md Section 14.7).
 *
 *     default=hdd        ; hdd, memtest, cd or usb
 *     timeout=5          ; seconds; any key stops the countdown
 *
 * Strict on purpose. A key the file does not know, a value it cannot parse,
 * a key given twice, or a timeout with no default is an error naming the
 * line, and the result is then no default and no countdown: the menu waits.
 * A typo must not become a different default silently.
 *
 * Keys and values are matched without regard to case, since the file may
 * well have been written on DOS. A ';' or '#' starts a comment anywhere on a
 * line. Blank lines are ignored, and so is a line ending in CR.
 */

#ifndef FALCON_BIOS_JUMPINI_H
#define FALCON_BIOS_JUMPINI_H

#include <stddef.h>

typedef enum {
    JUMP_DEF_NONE = 0,
    JUMP_DEF_HDD,         /* the first disk with boot code */
    JUMP_DEF_MEMTEST,
    JUMP_DEF_CD,
    JUMP_DEF_USB
} jump_default;

/* The longest timeout accepted: an hour, which is already past any use. */
#define JUMP_TIMEOUT_MAX 3600u

#define JUMP_INI_WHY_MAX 80u

typedef struct {
    jump_default def;
    unsigned     timeout;          /* seconds; 0 is no countdown */
    unsigned     err_line;         /* 1-based; 0 when there was no error */
    char         why[JUMP_INI_WHY_MAX];  /* the error, as a sentence */
} jump_cfg;

/* Parses `len` bytes of `text`. 0 on success. -1 on an error, with err_line
 * and why filled in and def and timeout reset to none. */
int jump_ini_parse(const char *text, size_t len, jump_cfg *out);

/* The value `default=` takes for `d`, as the file spells it. */
const char *jump_default_name(jump_default d);

#endif /* FALCON_BIOS_JUMPINI_H */
