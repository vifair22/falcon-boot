/* Shared declarations for the falcon-boot unit test binary. */

#ifndef FALCON_TEST_TESTS_H
#define FALCON_TEST_TESTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* What a fixture-backed suite returns when its image is absent.
 *
 * Zero by default, so `make -C test run` still succeeds on a machine that
 * cannot build the images: mk-test-exfat.sh, mk-test-ext4.sh and
 * mk-test-luks.sh each need mkfs and a loop mount, so each needs sudo, and a
 * suite that cannot be run is not a suite that failed.
 *
 * That default is also how this tree reported green for a long time with four
 * suites switched off and falcon-core/src/fs/ext4.c, falcon-core/src/luks and
 * falcon-core/src/luks/af at zero coverage, because nothing built the images
 * and nothing said so louder than a printf. FALCON_TEST_STRICT=1 turns a skip
 * into a failure. The coverage target sets it, since a coverage number measured
 * with a quarter of the suite missing is worse than no number, and CI should
 * set it too.
 *
 * Build the images with `make test-fixtures` from the repo root. */
static inline int test_skip_rc(void)
{
    const char *s = getenv("FALCON_TEST_STRICT");

    return (s != NULL && s[0] != '\0' && s[0] != '0') ? 1 : 0;
}

/* Each returns the number of failed tests, as cmocka_run_group_tests does. */
int run_synth_tests(void);
int run_file_tests(void);
int run_extent_tests(void);
int run_fixture_tests(void);
int run_cpio_tests(void);
int run_family_tests(void);
int run_isoprobe_tests(void);
int run_payload_tests(void);
int run_uki_tests(void);
int run_util_tests(void);
int run_appliance_route_tests(void);
int run_manifest_tests(void);
int run_bless_tests(void);
int run_next_tests(void);
int run_crypto_tests(void);
int run_json_tests(void);
int run_luks_tests(void);
int run_ext4_tests(void);
int run_media_fs_tests(void);
int run_iso9660_tests(void);
int run_udf_tests(void);
int run_udfedit_tests(void);
int run_bootcfg_tests(void);
int run_extmap_tests(void);
int run_hookcmd_tests(void);
int run_bootfs_tests(void);
int run_blesslist_tests(void);
int run_wim_tests(void);
int run_hookcore_tests(void);
int run_fs_tests(void);
int run_iso_overlay_tests(void);
int run_part_tests(void);
int run_sb_tests(void);
int run_osrel_tests(void);
int run_pe_tests(void);
int run_trust_tests(void);

/* Copies a UCS-2 filename into an ASCII buffer. Every character the tests use
 * is ASCII; anything outside that range is rendered '?' so a mismatch is
 * visible in the assertion message rather than silently truncating. */
void ucs2_to_ascii(const uint16_t *src, char *dst, size_t cap);

#endif /* FALCON_TEST_TESTS_H */
