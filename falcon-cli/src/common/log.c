#include <common/log.h>

#include <stdarg.h>
#include <stdio.h>

static cli_log_level g_level = CLI_LOG_INFO;

void cli_log_set_level(cli_log_level level)
{
    g_level = level;
}

cli_log_level cli_log_level_get(void)
{
    return g_level;
}

void cli_log(cli_log_level level, const char *fmt, ...)
{
    FILE *stream;
    va_list ap;

    if (level < g_level)
        return;

    stream = (level >= CLI_LOG_WARN) ? stderr : stdout;
    if (level == CLI_LOG_WARN)
        (void)fputs("warning: ", stream);
    else if (level == CLI_LOG_ERROR)
        (void)fputs("error: ", stream);
    else if (level == CLI_LOG_DEBUG)
        (void)fputs("debug: ", stream);

    va_start(ap, fmt);
    (void)vfprintf(stream, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stream);
    (void)fflush(stream);
}

void cli_log_blank(void)
{
    if (CLI_LOG_INFO < g_level)
        return;
    (void)fputc('\n', stdout);
    (void)fflush(stdout);
}

void cli_log_failure(cli_status code)
{
    const char *msg = cli_error_msg();

    if (msg[0] != '\0')
        log_error("%s", msg);
    else
        log_error("failed (%s)", cli_status_name(code));

    if (g_level <= CLI_LOG_DEBUG)
        log_debug("status %s (%d)", cli_status_name(code), (int)code);
}
