/* falcon-boot host tooling: running the system tools we deliberately do not
 * reimplement (DESIGN Section 8).
 *
 * Every child is started with fork + execvp and an argv ARRAY. There is no
 * system(), no popen() and no command string anywhere in this tree, and that
 * is a security property rather than a style preference: the arguments are
 * user-supplied paths and passphrase-adjacent values, and a shell in the
 * middle would turn a file named `; rm -rf ~` into exactly what it looks
 * like. An argv array has no parsing step to exploit.
 *
 * Failures name the command that failed and how it ended, because "cryptsetup
 * failed" with no exit status is a support ticket and "cryptsetup exited 1"
 * is a diagnosis.
 */

#ifndef FALCON_CLI_PROC_H
#define FALCON_CLI_PROC_H

#include <stddef.h>

#include <cli/common/status.h>

typedef struct {
    const char *const *argv;    /* NULL-terminated; argv[0] is the program */

    /* Bytes fed to the child's stdin, or NULL for /dev/null. Written in full
     * before the child's stdout is read, which is safe only because every use
     * here is a passphrase-sized write that fits in one pipe buffer. */
    const void *in;
    size_t      in_len;

    /* Capture the child's stdout into this buffer instead of inheriting ours.
     * Output past the cap is dropped and reported as CLI_ERR_RANGE. */
    char   *out;
    size_t  out_cap;
    size_t *out_len;

    /* Send the child's stdout to /dev/null. Ignored when `out` is set. */
    int quiet_stdout;
} proc_spec;

cli_status proc_run(const proc_spec *spec);

/* The sentinel every variadic wrapper below ends with. Typed rather than a
 * bare NULL because a bare NULL may be a plain int, and an int where the
 * callee reads a pointer is undefined behaviour on any ABI where the two are
 * different widths. proc_runl("sgdisk", "-Z", path, PROC_END). */
#define PROC_END ((const char *)NULL)

/* Convenience wrappers. The variadic ones take a PROC_END-terminated argument
 * list. */
cli_status proc_runl(const char *prog, ...) __attribute__((sentinel));
cli_status proc_runl_quiet(const char *prog, ...) __attribute__((sentinel));
cli_status proc_input_l(const void *in, size_t in_len, const char *prog, ...)
    __attribute__((sentinel));
cli_status proc_capture_l(char *out, size_t cap, size_t *len,
                          const char *prog, ...) __attribute__((sentinel));

/* Is this program on PATH? Used for the up-front tool check, so a create run
 * fails in the first second rather than after formatting three partitions. */
int proc_exists(const char *prog);

#endif /* FALCON_CLI_PROC_H */
