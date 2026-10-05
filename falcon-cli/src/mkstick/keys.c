#include <mkstick/keys.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/log.h>
#include <common/proc.h>
#include <common/util.h>

cli_status mok_paths(mok_keys *k, const char *dir)
{
    cli_status rc;

    memset(k, 0, sizeof *k);
    if (snprintf(k->dir, sizeof k->dir, "%s", dir) < 0)
        return set_error(CLI_ERR_RANGE, "key directory path too long");

    rc = util_join(k->key, sizeof k->key, dir, "falcon-mok.key");
    if (rc != CLI_OK)
        return rc;
    rc = util_join(k->crt, sizeof k->crt, dir, "falcon-mok.crt");
    if (rc != CLI_OK)
        return rc;
    rc = util_join(k->cer, sizeof k->cer, dir, "falcon-mok.cer");
    return rc;
}

cli_status mok_generate(const mok_keys *k, const char *cn)
{
    char subject[512];
    mode_t old_umask;
    cli_status rc;

    if (util_is_file(k->key))
        return set_error(CLI_ERR_EXISTS,
                         "%s already exists; refusing to replace a MOK key "
                         "that existing signatures depend on", k->key);

    if (snprintf(subject, sizeof subject, "/CN=%s/", cn) < 0)
        return set_error(CLI_ERR_RANGE, "CN too long");

    /* The key must not exist, even for an instant, at a mode another user
     * could read. openssl does create it 0600, but the umask is what makes
     * that true regardless of the openssl version in front of us. */
    old_umask = umask(0077);

    log_info("generating the MOK keypair (RSA-%s, %s days, CN=%s)",
             MOK_DEFAULT_BITS, MOK_DEFAULT_DAYS, cn);
    rc = proc_runl_quiet("openssl", "req", "-new", "-x509",
                         "-newkey", "rsa:" MOK_DEFAULT_BITS,
                         "-nodes", "-sha256",
                         "-days", MOK_DEFAULT_DAYS,
                         "-subj", subject,
                         "-keyout", k->key,
                         "-out", k->crt, PROC_END);
    if (rc != CLI_OK) {
        (void)umask(old_umask);
        return rc;
    }

    /* MokManager reads DER, so both forms are produced: the PEM one is what
     * sbsign takes here, the DER one is what travels to p1 for enrollment. */
    rc = proc_runl_quiet("openssl", "x509", "-in", k->crt,
                         "-outform", "DER", "-out", k->cer, PROC_END);
    (void)umask(old_umask);
    if (rc != CLI_OK)
        return rc;

    if (chmod(k->key, 0600) != 0)
        return set_error_errno(CLI_ERR_IO, "chmod 0600 %s", k->key);
    if (chmod(k->crt, 0644) != 0)
        return set_error_errno(CLI_ERR_IO, "chmod %s", k->crt);
    if (chmod(k->cer, 0644) != 0)
        return set_error_errno(CLI_ERR_IO, "chmod %s", k->cer);

    return CLI_OK;
}

/* Big enough for an RSA-4096 public key in PEM, and any smaller one. */
#define PUBKEY_PEM_MAX 2048u

cli_status mok_check_pair(const char *key, const char *cert)
{
    char from_key[PUBKEY_PEM_MAX];
    char from_cert[PUBKEY_PEM_MAX];
    size_t nk = 0;
    size_t nc = 0;
    cli_status rc;

    if (!util_is_file(key))
        return set_error(CLI_ERR_NOTFOUND, "no MOK key at %s", key);
    if (!util_is_file(cert))
        return set_error(CLI_ERR_NOTFOUND, "no MOK certificate at %s", cert);
    rc = proc_capture_l(from_key, sizeof from_key, &nk, "openssl", "pkey",
                        "-in", key, "-pubout", PROC_END);
    if (rc != CLI_OK)
        return rc;
    rc = proc_capture_l(from_cert, sizeof from_cert, &nc, "openssl", "x509",
                        "-in", cert, "-noout", "-pubkey", PROC_END);
    if (rc != CLI_OK)
        return rc;
    if (nk == 0 || nk != nc || memcmp(from_key, from_cert, nk) != 0)
        return set_error(CLI_ERR_USAGE, "%s is not the private key of %s",
                         key, cert);
    return CLI_OK;
}

cli_status mok_import(const mok_keys *k, const char *key, const char *cert)
{
    mode_t old_umask;
    cli_status rc;

    if (util_is_file(k->key))
        return set_error(CLI_ERR_EXISTS,
                         "%s already exists; refusing to replace a MOK key "
                         "that existing signatures depend on", k->key);
    rc = mok_check_pair(key, cert);
    if (rc != CLI_OK)
        return rc;

    log_info("using the existing MOK from %s", cert);
    old_umask = umask(0077);
    rc = util_copy_file(key, k->key, 0600);
    if (rc == CLI_OK)
        rc = util_copy_file(cert, k->crt, 0644);
    if (rc == CLI_OK)
        rc = proc_runl_quiet("openssl", "x509", "-in", k->crt,
                             "-outform", "DER", "-out", k->cer, PROC_END);
    (void)umask(old_umask);
    if (rc != CLI_OK)
        return rc;
    if (chmod(k->cer, 0644) != 0)
        return set_error_errno(CLI_ERR_IO, "chmod %s", k->cer);
    return CLI_OK;
}

cli_status mok_sign(const mok_keys *k, const char *in, const char *out)
{
    cli_status rc;

    if (!util_is_file(in))
        return set_error(CLI_ERR_NOTFOUND, "nothing to sign at %s", in);

    log_debug("sbsign %s -> %s", in, out);
    /* sbsign warns "data remaining: gaps between PE/COFF sections?" about our
     * mingw output. It says the same about Fedora's own gnu-efi binaries and
     * both verify under shim, so the warning is noise rather than a
     * signal. */
    rc = proc_runl_quiet("sbsign", "--key", k->key, "--cert", k->crt,
                         "--output", out, in, PROC_END);
    if (rc != CLI_OK)
        return rc;
    if (!util_is_file(out))
        return set_error(CLI_ERR_IO, "sbsign reported success but %s is not "
                         "there", out);
    return CLI_OK;
}
