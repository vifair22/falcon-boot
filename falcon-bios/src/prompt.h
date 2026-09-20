/* falcon-bios: the masked passphrase prompt. See prompt.c. */

#ifndef FALCON_BIOS_PROMPT_H
#define FALCON_BIOS_PROMPT_H

#include <stddef.h>

/* Reads a line with a masked echo into out (NUL-terminated, at most cap-1
 * characters). 0 when something was typed, nonzero when the user pressed
 * escape or entered nothing. The caller owns the buffer and is the one that
 * must wipe it. */
int prompt_passphrase(const char *label, char *out, size_t cap, size_t *len);

#endif /* FALCON_BIOS_PROMPT_H */
