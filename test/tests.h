/* Shared declarations for the exFAT unit test binary. */

#ifndef AIRIES_TEST_TESTS_H
#define AIRIES_TEST_TESTS_H

#include <stdint.h>
#include <stddef.h>

/* Each returns the number of failed tests, as cmocka_run_group_tests does. */
int run_synth_tests(void);
int run_fixture_tests(void);

/* Copies a UCS-2 filename into an ASCII buffer. Every character the tests use
 * is ASCII; anything outside that range is rendered '?' so a mismatch is
 * visible in the assertion message rather than silently truncating. */
void ucs2_to_ascii(const uint16_t *src, char *dst, size_t cap);

#endif /* AIRIES_TEST_TESTS_H */
