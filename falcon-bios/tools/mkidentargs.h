/* falcon-bios-mkident's argument rules, apart from its file handling so the
 * host tests reach them (falcon-bios/tests/mkident_test.c). See mkident.c. */

#ifndef FALCON_BIOS_MKIDENTARGS_H
#define FALCON_BIOS_MKIDENTARGS_H

#include <stdint.h>

#include <ident/ident.h>

/* A record from the command line's words: a role name (boot, library,
 * shelf), 32 lowercase hex digits, and the stage 3 and marker sectors as
 * decimal (or 0x hex) numbers below 65536, which must both be 0 unless the
 * role is boot, as the decoders require. 0 on success; otherwise nonzero
 * with *why saying which word was wrong. */
int mkident_record(const char *role, const char *id_hex, const char *stage3,
                   const char *marker, ident_rec *out, const char **why);

/* A byte offset: decimal or 0x hex, a multiple of 512. 0 on success. */
int mkident_offset(const char *s, uint64_t *out);

#endif /* FALCON_BIOS_MKIDENTARGS_H */
