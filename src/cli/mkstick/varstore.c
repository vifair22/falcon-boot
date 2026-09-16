/* falcon-boot mkstick: `varstore`, a TEST-RIG helper and nothing else.
 *
 * Why it lives in this tool. Proving that a stick mkstick built actually
 * boots under Secure Boot means booting it in QEMU with our certificate in
 * MOK. On real firmware the user does that through MokManager, interactively,
 * on first boot; a scripted proof cannot drive an interactive UI, so the
 * MokList variable is written straight into a copy of the OVMF varstore.
 *
 * What that shortcut does and does not skip. It skips the ENROLLMENT UI only.
 * Shim still reads MokList and still performs every verification itself, so
 * what a run against this varstore measures is the real MOK load path.
 * Testing the enrollment UX is a separate, interactive job (DESIGN Section 11
 * step 10).
 *
 * This command never touches a stick, never generates or reads a private key,
 * and is not part of building one. It shells out to virt-fw-vars from the
 * python venv the shim spike already creates, because writing an EFI variable
 * store is not something this repo should own.
 */

#include <cli/mkstick/mkstick.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cli/common/log.h>
#include <cli/common/proc.h>
#include <cli/common/util.h>
#include <cli/mkstick/disk.h>

#define DEFAULT_VENV_TOOL "build/shim/venv/bin/virt-fw-vars"
#define DEFAULT_OVMF_DIR  "/usr/share/edk2/OvmfX64"

/* The owner GUID on a MokList entry is informational; this is shim's own
 * vendor GUID, which is what MokManager itself would stamp there. */
#define MOK_OWNER_GUID "605dab50-e046-4300-abb6-3dd810dd8b23"

static const cli_opt varstore_opts[] = {
    { "template", CLI_OPT_VALUE, 0, "<file>",
      "OVMF Secure Boot varstore to start from "
      "(default $OVMF_DIR/OVMF_VARS.secboot.fd)" },
    { "virt-fw-vars", CLI_OPT_VALUE, 0, "<path>",
      "the virt-fw-vars executable (default " DEFAULT_VENV_TOOL ")" }
};

static cli_status varstore_run(const cli_args *a)
{
    const char *cert = a->pos[0];
    const char *out = a->pos[1];
    const char *tool = cli_value(a, "virt-fw-vars", DEFAULT_VENV_TOOL);
    const char *template_ = cli_value(a, "template", NULL);
    char derived[MKS_PATH_MAX];
    cli_status rc;

    if (!util_is_file(cert))
        return set_error(CLI_ERR_NOTFOUND, "no certificate at %s", cert);
    if (!proc_exists(tool))
        return set_error(CLI_ERR_NOTFOUND,
                         "%s is not there; tools/mk-test-mok.sh creates the "
                         "venv this expects, or pass --virt-fw-vars",
                         tool);

    if (template_ == NULL) {
        const char *dir = getenv("OVMF_DIR");

        rc = util_join(derived, sizeof derived,
                       (dir != NULL) ? dir : DEFAULT_OVMF_DIR,
                       "OVMF_VARS.secboot.fd");
        if (rc != CLI_OK)
            return rc;
        template_ = derived;
    }
    if (!util_is_file(template_))
        return set_error(CLI_ERR_NOTFOUND,
                         "no Secure Boot varstore at %s (set OVMF_DIR or pass "
                         "--template)", template_);

    /* Always rebuilt from the pristine template, so a re-run cannot stack
     * duplicate MokList entries or inherit state an earlier boot wrote. */
    rc = util_copy_file(template_, out, 0644);
    if (rc != CLI_OK)
        return rc;

    /* The certificate given here is the PUBLIC one. Handing this command a
     * private key would simply produce a varstore virt-fw-vars refuses. */
    rc = proc_runl_quiet(tool, "--inplace", out,
                         "--add-mok", MOK_OWNER_GUID, cert, PROC_END);
    if (rc != CLI_OK)
        return rc;

    log_info("%s now trusts %s as a MOK", out, cert);
    /* VARS_FILE names a SEPARATE writable copy on purpose: the varstore QEMU
     * boots is written to (boot order, MokSBState), and pointing it at this
     * file would leave the template carrying a previous run's state. */
    log_info("boot it with: MOK_VARS=%s VARS_FILE=build/vars-run.fd "
             "tools/run-qemu.sh --stick --secboot", out);
    return CLI_OK;
}

const cli_cmd mks_cmd_varstore = {
    "varstore",
    "TEST RIG: seed an OVMF varstore with a certificate as a MOK",
    "<cert.crt> <out.fd>",
    varstore_opts,
    sizeof varstore_opts / sizeof varstore_opts[0],
    2, 2,
    "Not part of building a stick. It lets a QEMU run skip the interactive\n"
    "MokManager enrollment that a real machine does on first boot.\n",
    varstore_run
};
