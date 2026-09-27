/* falcon-boot-mkstick: the device-configuration tool of DESIGN Section 8.
 *
 * Ordinary Linux userland C. It is allowed to shell out to sgdisk, mkfs.*,
 * cryptsetup, openssl and sbsign rather than reimplementing partitioning,
 * filesystem creation, argon2id or Authenticode signing, and DESIGN Section 8
 * says so explicitly: that is the vendoring line for a config tool that runs
 * on a full Linux box.
 *
 * What it must never become. The loader verifies and never signs; this tool
 * signs and never verifies-and-permits. Nothing here can relax what the
 * platform enforces, and the private key it generates lands inside p3's LUKS
 * volume and in no other place unless the user explicitly asks for a second
 * copy (--key-out) and is told what that means.
 */

#include <stdio.h>
#include <string.h>

#include <common/log.h>
#include <common/opt.h>
#include <mkstick/cleanup.h>
#include <mkstick/mkstick.h>

static const cli_cmd *const commands[] = {
    &mks_cmd_bios,
    &mks_cmd_create,
    &mks_cmd_ident,
    &mks_cmd_keygen,
    &mks_cmd_next_boot,
    &mks_cmd_station,
    &mks_cmd_status,
    &mks_cmd_varstore
};

static const char *const tagline =
    "Build and inspect a falcon-boot stick (docs/DESIGN.md Section 8).";

static const char *const global_help =
    "  --verbose                    show every command this tool runs\n"
    "  --quiet                      warnings and errors only\n"
    "  --help                       this help, or the command's own\n";

int main(int argc, char **argv)
{
    cli_status rc;

    cli_help_register("falcon-boot-mkstick", tagline, commands,
                      sizeof commands / sizeof commands[0], global_help);
    cleanup_init();

    if (argc >= 2 && strcmp(argv[1], "station") == 0)
        rc = mks_station_main(argc - 2, argv + 2);
    else
        rc = cli_dispatch(commands, sizeof commands / sizeof commands[0],
                          "falcon-boot-mkstick", argc - 1, argv + 1);
    if (rc == CLI_OK)
        return 0;

    cli_log_failure(rc);
    /* Context-sensitive help: whichever topic was active when the failure
     * happened, which cli_dispatch set before parsing and before running. A
     * usage error gets the help that would have prevented it; a runtime
     * failure does not, because the command line was not the problem. */
    if (rc == CLI_ERR_USAGE) {
        (void)fputc('\n', stdout);
        cli_help_print();
    }
    return 1;
}
