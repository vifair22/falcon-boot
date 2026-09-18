/* falcon-boot host tooling: leveled logging with the stream split the repo's
 * CLI standard asks for.
 *
 * DEBUG and INFO are normal output and go to stdout; WARN and ERROR are
 * diagnostics and go to stderr, so that `mkstick status ... > report.txt`
 * captures the report and still shows the complaints on the terminal. No
 * colour codes anywhere: the output is read as often through a pipe and a CI
 * log as it is on a terminal.
 */

#ifndef FALCON_CLI_LOG_H
#define FALCON_CLI_LOG_H

#include <common/status.h>

typedef enum {
    CLI_LOG_DEBUG = 0,
    CLI_LOG_INFO  = 1,
    CLI_LOG_WARN  = 2,
    CLI_LOG_ERROR = 3
} cli_log_level;

void          cli_log_set_level(cli_log_level level);
cli_log_level cli_log_level_get(void);

void cli_log(cli_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* A blank separator line at INFO level. A function rather than log_info("")
 * because an empty format string is (rightly) a -Wformat-zero-length warning,
 * and the repo builds warning-free. */
void cli_log_blank(void);

#define log_debug(...) cli_log(CLI_LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  cli_log(CLI_LOG_INFO, __VA_ARGS__)
#define log_warn(...)  cli_log(CLI_LOG_WARN, __VA_ARGS__)
#define log_error(...) cli_log(CLI_LOG_ERROR, __VA_ARGS__)

/* Print the recorded error sentence, once, at the CLI boundary. */
void cli_log_failure(cli_status code);

#endif /* FALCON_CLI_LOG_H */
