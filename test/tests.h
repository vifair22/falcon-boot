/* Shared declarations for the falcon-boot unit test binary. */

#ifndef FALCON_TEST_TESTS_H
#define FALCON_TEST_TESTS_H

#include <stdint.h>
#include <stddef.h>

/* Each returns the number of failed tests, as cmocka_run_group_tests does. */
int run_synth_tests(void);
int run_file_tests(void);
int run_extent_tests(void);
int run_fixture_tests(void);
int run_cpio_tests(void);
int run_family_tests(void);
int run_isoprobe_tests(void);
int run_wimext_tests(void);
int run_prepcore_tests(void);
int run_appliance_route_tests(void);
int run_manifest_tests(void);
int run_bless_tests(void);
int run_prep_tests(void);
int run_crypto_tests(void);
int run_json_tests(void);
int run_luks_tests(void);
int run_ext4_tests(void);
int run_fs_tests(void);
int run_iso_overlay_tests(void);
int run_part_tests(void);
int run_sb_tests(void);

/* Copies a UCS-2 filename into an ASCII buffer. Every character the tests use
 * is ASCII; anything outside that range is rendered '?' so a mismatch is
 * visible in the assertion message rather than silently truncating. */
void ucs2_to_ascii(const uint16_t *src, char *dst, size_t cap);

#endif /* FALCON_TEST_TESTS_H */
