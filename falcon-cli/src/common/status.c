#include <common/status.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CLI_ERR_CAP 1024u

/* One buffer per thread. Sized for a sentence plus a long path plus a
 * strerror tail; anything longer is truncated rather than allocated for,
 * because an error path is the worst place to need a heap. */
static _Thread_local char       g_msg[CLI_ERR_CAP];
static _Thread_local cli_status g_code = CLI_OK;

const char *cli_status_name(cli_status s)
{
    switch (s) {
    case CLI_OK:           return "OK";
    case CLI_ERR_USAGE:    return "USAGE";
    case CLI_ERR_IO:       return "IO";
    case CLI_ERR_EXEC:     return "EXEC";
    case CLI_ERR_NOTFOUND: return "NOTFOUND";
    case CLI_ERR_INVALID:  return "INVALID";
    case CLI_ERR_PERM:     return "PERM";
    case CLI_ERR_BUSY:     return "BUSY";
    case CLI_ERR_EXISTS:   return "EXISTS";
    case CLI_ERR_NOMEM:    return "NOMEM";
    case CLI_ERR_RANGE:    return "RANGE";
    case CLI_ERR_INTERNAL: return "INTERNAL";
    }
    return "UNKNOWN";
}

cli_status cli_set_error(cli_status code, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    g_code = code;
    return code;
}

cli_status cli_set_error_errno(cli_status code, const char *fmt, ...)
{
    /* Captured first: vsnprintf is entitled to clobber errno. */
    const int saved = errno;
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);

    if (n > 0) {
        size_t used = (size_t)n;
        if (used < sizeof g_msg - 1)
            (void)snprintf(g_msg + used, sizeof g_msg - used, ": %s",
                           strerror(saved));
    }
    g_code = code;
    errno = saved;
    return code;
}

const char *cli_error_msg(void)
{
    return g_msg;
}

cli_status cli_error_code(void)
{
    return g_code;
}

void cli_error_clear(void)
{
    g_msg[0] = '\0';
    g_code = CLI_OK;
}
