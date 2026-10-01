/* falcon-boot: one-time codes, HOTP (RFC 4226) and TOTP (RFC 6238), both
 * HMAC-SHA1.
 *
 * Purpose. The station's tamper check (DESIGN Section 12.15):
 * the TPM releases a 20-byte seed only when the boot measured what it did at
 * enrollment, and falcon shows the code the seed gives for the current
 * 30-second step. The user compares it with an authenticator that holds the
 * same seed.
 *
 * Reasoning. HMAC-SHA1 with 6 digits is what every authenticator app does by
 * default, so an enrolled seed needs no parameters beside it. The time is an
 * input, seconds since the Unix epoch in UTC; where it comes from is the
 * caller's business (the firmware clock in the loader).
 *
 * Interactions. The UEFI station's TOTP lines (falcon-efi/src/totp,
 * docs/DESIGN.md Section 12.15); vectors from RFC 4226 Appendix D and RFC
 * 6238 Appendix B in falcon-core/tests/tpmcrypto_test.c.
 */

#ifndef FALCON_CRYPTO_OTP_H
#define FALCON_CRYPTO_OTP_H

#include <stdint.h>
#include <stddef.h>

#define TOTP_STEP   30u
#define TOTP_DIGITS 6u

/* digits is 6 to 9 (a 31-bit truncation holds nine). */
int hotp_sha1(const uint8_t *key, size_t key_len, uint64_t counter,
              unsigned int digits, uint32_t *code);

/* The code for the step holding unix_time; step is in seconds. */
int totp_sha1(const uint8_t *key, size_t key_len, uint64_t unix_time,
              unsigned int step, unsigned int digits, uint32_t *code);

/* Seconds since the Unix epoch of a UTC calendar time, which is the form
 * the firmware's clock gives (EFI_TIME). 1970 to 9999; a day past its
 * month's end is refused except 29 February, which is taken on trust
 * rather than checked against the leap rule, since a clock that says it is
 * holds it. */
int otp_unix_time(unsigned int year, unsigned int month, unsigned int day,
                  unsigned int hour, unsigned int minute, unsigned int second,
                  uint64_t *out);

#endif /* FALCON_CRYPTO_OTP_H */
