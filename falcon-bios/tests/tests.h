/* falcon-bios's host test suite. */

#ifndef FALCON_BIOS_TESTS_H
#define FALCON_BIOS_TESTS_H

int run_vcdcore_tests(void);
int run_vfdcore_tests(void);
int run_linuxcore_tests(void);
int run_e820_tests(void);
int run_jumpini_tests(void);
int run_jmod_tests(void);
int run_lowmem_tests(void);
int run_atacore_tests(void);
int run_usbcore_tests(void);
int run_pcccore_tests(void);

#endif /* FALCON_BIOS_TESTS_H */
