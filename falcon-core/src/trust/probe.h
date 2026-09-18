/* falcon-boot: predicting one image, from its bytes (DESIGN Section 7.8).
 *
 * The glue between "a file somewhere" and trust_evaluate: read a header window,
 * find the certificate table and .sbat, read those, hash the image when it is
 * small enough, and evaluate. Every read goes through a callback, so the same
 * code serves an ISO's BOOTX64.EFI through the firmware's File Protocol and a
 * payload or blessed UKI on p2 through falcon-core/src/fs, and is tested
 * against files on the host.
 *
 * Every failure is TRUST_NO_VERDICT. A file that cannot be read, a layout
 * shim would not hash, a certificate table past AC_CERTS_MAX: none of them is
 * a reason to paint a row yellow.
 *
 * Not reentrant: the buffers are static, because together they are a few
 * hundred kilobytes and the loader's stack is no place for them.
 */

#ifndef FALCON_SECBOOT_PROBE_H
#define FALCON_SECBOOT_PROBE_H

#include <trust/authenticode.h>
#include <trust/trust.h>

/* The Authenticode hash is computed only up to this size. First stages are
 * shims, systemd-boot, grub and bootmgr, all far below it; a 250 MB blessed
 * UKI is above it, and reading all of it to check a hash dbx will not contain
 * would cost seconds per menu row. */
#define TRUST_HASH_MAX ((uint64_t)32u << 20)

void trust_probe(const trust_stores *st, ac_read_fn read, void *ctx,
                 uint64_t size, trust_result *out);

#endif /* FALCON_SECBOOT_PROBE_H */
