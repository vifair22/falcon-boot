/* falcon-boot-sign prepare: Windows install media, made findable.
 *
 * The other half of DESIGN Section 8's "one command, two outputs". Blessing
 * signs; preparation patches, and the two share nothing but the manifest and
 * the ISO reader. Keeping them in separate translation units is not tidiness:
 * preparation holds no key, loads no key and prompts for nothing, and the
 * cheapest way to keep that true is for the code to have no way to reach the
 * signing path at all. It runs unchanged on a --p3-plain stick, where there is
 * no private key to hold.
 */

#ifndef FALCON_CLI_SIGN_PREPARE_H
#define FALCON_CLI_SIGN_PREPARE_H

#include <stdio.h>

/* Runs the prepare subcommand. argv[0] is the subcommand's own name, as the
 * dispatcher in main.c hands it over. Returns an SGN_* status. */
int sgn_prepare_main(int argc, char **argv);

/* The subcommand's own usage block, so main.c can print it under --help
 * without knowing any of its options. */
void sgn_prepare_usage(FILE *f);

#endif /* FALCON_CLI_SIGN_PREPARE_H */
