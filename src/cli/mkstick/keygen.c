/* falcon-boot mkstick: `keygen`, the MOK keypair on its own.
 *
 * `create` generates a keypair for the stick it builds. This command exists
 * for the other route DESIGN Section 7.5 describes: an external host the user
 * controls, where the key can sit behind a passphrase or a hardware token and
 * where the `sign` tool runs to bless media. Same key material, same two
 * output forms (PEM for sbsign, DER for MokManager), no stick involved.
 *
 * It needs no privileges and touches no device. The warning it prints is not
 * decoration: a private key written to an ordinary directory is exactly the
 * thing Section 7.5 keeps off the readable partition, and the moment it
 * exists outside a LUKS volume its protection becomes the user's.
 */

#include <cli/mkstick/mkstick.h>

#include <cli/common/log.h>
#include <cli/common/proc.h>
#include <cli/common/util.h>
#include <cli/mkstick/keys.h>

static const cli_opt keygen_opts[] = {
    { "cn", CLI_OPT_VALUE, 0, "<name>",
      "subject CN of the certificate (default \"" MOK_DEFAULT_CN "\")" }
};

static cli_status keygen_run(const cli_args *a)
{
    const char *dir = a->pos[0];
    mok_keys keys;
    cli_status rc;

    if (!proc_exists("openssl"))
        return set_error(CLI_ERR_NOTFOUND,
                         "openssl is not on PATH; mkstick shells out to it "
                         "rather than reimplementing key generation");

    rc = util_mkdir_p(dir, 0700);
    if (rc != CLI_OK)
        return rc;
    rc = mok_paths(&keys, dir);
    if (rc != CLI_OK)
        return rc;
    rc = mok_generate(&keys, cli_value(a, "cn", MOK_DEFAULT_CN));
    if (rc != CLI_OK)
        return rc;

    log_info("%s  private key (mode 0600)", keys.key);
    log_info("%s  certificate, PEM: what sbsign takes", keys.crt);
    log_info("%s  certificate, DER: what MokManager enrolls", keys.cer);
    log_warn("the PRIVATE key is now in an ordinary directory. DESIGN Section "
             "7.5 keeps it encrypted on p3 for a reason; protecting this copy "
             "is yours to do.");
    return CLI_OK;
}

const cli_cmd mks_cmd_keygen = {
    "keygen",
    "generate a MOK keypair for the host-side signing route",
    "<dir>",
    keygen_opts,
    sizeof keygen_opts / sizeof keygen_opts[0],
    1, 1,
    "Writes falcon-mok.key (0600), falcon-mok.crt (PEM) and falcon-mok.cer\n"
    "(DER) into <dir>, which is created if it does not exist. Refuses to\n"
    "overwrite an existing key: replacing one orphans every signature already\n"
    "made with it.\n",
    keygen_run
};
