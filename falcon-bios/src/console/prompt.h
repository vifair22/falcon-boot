/* falcon-bios: the masked passphrase prompt. See prompt.c. */

#ifndef FALCON_BIOS_PROMPT_H
#define FALCON_BIOS_PROMPT_H

#include <stddef.h>

/* Reads a line with a masked echo into out (NUL-terminated, at most cap-1
 * characters) and its length into *len, at the cursor: the caller has
 * written the prompt. 0 when the user pressed enter, whatever they typed,
 * including nothing; -1 for escape, with out wiped. */
int prompt_passphrase(char *out, size_t cap, size_t *len);

#endif /* FALCON_BIOS_PROMPT_H */
