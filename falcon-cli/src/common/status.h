/* falcon-boot host tooling: status codes and the thread-local error buffer.
 *
 * The repo's C error convention, in its hosted form: every function that can
 * fail returns a cli_status (OK is zero, errors negative), the function that
 * actually discovered the failure writes the human sentence, and every caller
 * above it propagates the code without touching the message. Only the CLI
 * layer reads the buffer, and only to print it once before turning the code
 * into an exit status.
 *
 * The buffer is thread-local so that the convention survives if any part of
 * this tooling ever grows a worker thread; today it is single-threaded and
 * that costs nothing.
 */

#ifndef FALCON_CLI_STATUS_H
#define FALCON_CLI_STATUS_H

typedef enum {
    CLI_OK           =   0,
    CLI_ERR_USAGE    =  -1,  /* the command line was wrong */
    CLI_ERR_IO       =  -2,  /* a read, write, open or stat failed */
    CLI_ERR_EXEC     =  -3,  /* a child command failed or could not start */
    CLI_ERR_NOTFOUND =  -4,  /* a required input is missing */
    CLI_ERR_INVALID  =  -5,  /* input exists but is not what it must be */
    CLI_ERR_PERM     =  -6,  /* privilege we do not have */
    CLI_ERR_BUSY     =  -7,  /* device in use (mounted, held open) */
    CLI_ERR_EXISTS   =  -8,  /* refusing to overwrite without --force */
    CLI_ERR_NOMEM    =  -9,
    CLI_ERR_RANGE    = -10,  /* a size or count outside what we accept */
    CLI_ERR_INTERNAL = -11   /* a bug here, not a bad input */
} cli_status;

/* Short symbolic name, for --verbose diagnostics only. */
const char *cli_status_name(cli_status s);

/* Record the failure. Both return `code`, so the deepest function can write
 *   return set_error(CLI_ERR_IO, "...");
 * and read as one statement. The _errno variant appends ": " and strerror of
 * the CURRENT errno, which is why it must be called before anything else that
 * could disturb it. */
cli_status cli_set_error(cli_status code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
cli_status cli_set_error_errno(cli_status code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* The sentence for the most recent failure, or "" if none was recorded. */
const char *cli_error_msg(void);
cli_status  cli_error_code(void);
void        cli_error_clear(void);

/* The names the repo standard uses at call sites. */
#define set_error(code, ...)       cli_set_error((code), __VA_ARGS__)
#define set_error_errno(code, ...) cli_set_error_errno((code), __VA_ARGS__)

#endif /* FALCON_CLI_STATUS_H */
