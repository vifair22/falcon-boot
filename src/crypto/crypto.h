/* falcon-boot: shared crypto plumbing (docs/DESIGN.md Sections 5, 9).
 *
 * Purpose. The LUKS2 unlock of p3 is the one place in EFI-land where this
 * repo writes cryptography, and Section 9 fixes the scope: SHA-2, PBKDF2-HMAC
 * and AES-XTS *decrypt*, nothing else. This header holds the three things
 * every primitive here needs and nothing that belongs to one of them.
 *
 * Reasoning. The module is EFI-free on purpose, exactly like src/exfat: it
 * touches no firmware service, allocates nothing, and every context is
 * caller-allocated. That is what lets the host test suite run the identical
 * object code the loader runs, against volumes cryptsetup itself created,
 * which is the only honest way to validate hand-written crypto.
 *
 * Interactions. src/crypto/{sha256,sha512,hash,hmac,pbkdf2,aes,xts,aesni}
 * all include this; src/luks consumes the module through hash.h, pbkdf2.h and
 * xts.h and is the only in-repo caller.
 */

#ifndef FALCON_CRYPTO_H
#define FALCON_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    CRYPTO_OK              = 0,
    CRYPTO_ERR_BAD_ARG     = -1,
    CRYPTO_ERR_UNSUPPORTED = -2, /* a key length or hash we do not implement */
};

/* Zeroes n bytes and is not allowed to be optimised away.
 *
 * Every buffer that has held the passphrase, a keyslot key, an AF stripe or
 * the volume master key goes through this on every exit path, success or
 * failure. A plain memset() on a buffer that is dead afterwards is exactly
 * the store the optimiser is entitled to delete, and in EFI-land the memory
 * is not even unmapped afterwards: it stays in the firmware's pool until the
 * OS reclaims it. The implementation calls memset through a volatile function
 * pointer, so the compiler cannot prove what the call does and must emit it.
 */
void crypto_wipe(void *p, size_t n);

/* Constant-time equality; 1 when the n bytes match, 0 otherwise.
 *
 * Used for the LUKS2 digest comparison. The timing channel there is weak
 * (an attacker with the header can grind offline far faster than they can
 * time our loader), but a variable-time compare on a secret-derived value is
 * the kind of thing that gets copied into a context where it does matter.
 */
int crypto_ct_eq(const void *a, const void *b, size_t n);

/* Runtime CPU feature probe for the AES-NI dispatch in xts.c.
 *
 * This lives here, in a translation unit built with the repo's baseline
 * flags, and deliberately NOT in aesni.c: aesni.c is compiled with -maes
 * -msse2 -mssse3, which permits the compiler to emit those instructions
 * anywhere in that object, including inside the probe itself. A probe that
 * faults on the CPUs it exists to detect would be worse than no probe.
 */
int crypto_has_aesni(void);

#endif /* FALCON_CRYPTO_H */
