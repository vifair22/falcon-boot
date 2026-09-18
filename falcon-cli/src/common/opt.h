/* falcon-boot host tooling: command dispatch, whitelist option parsing and
 * context-sensitive help.
 *
 * The shape is the repo's CLI standard, and each piece of it is here for a
 * reason worth restating:
 *
 *   - Dispatch is a table of {name, handler} records rather than an if-chain,
 *     so the set of commands is data that the help text and the parser can
 *     both read, and adding a command cannot forget to add its help.
 *   - Options are a static whitelist per command. Anything not in the array
 *     is refused by name instead of ignored, because a silently-ignored
 *     --p3-siz would produce a stick with the wrong layout and no complaint.
 *   - The active help topic is set BEFORE the command runs, so any failure
 *     inside the command can print that command's help rather than the
 *     program's, without every error path having to know which command it is.
 *   - --help short-circuits before validation, so `mkstick create --help`
 *     answers the question instead of complaining that <target> is missing.
 */

#ifndef FALCON_CLI_OPT_H
#define FALCON_CLI_OPT_H

#include <stddef.h>

#include <common/status.h>

typedef enum {
    CLI_OPT_FLAG  = 0,  /* --force */
    CLI_OPT_VALUE = 1   /* --shim <dir>, also --shim=<dir> */
} cli_opt_kind;

typedef struct {
    const char  *name;      /* long name, without the leading "--" */
    cli_opt_kind kind;
    int          repeatable;/* nonzero: may appear more than once */
    const char  *arg;       /* placeholder shown in help, VALUE options only */
    const char  *help;      /* one line */
} cli_opt;

#define CLI_MAX_OPTS   32u
#define CLI_MAX_VALUES 64u
#define CLI_MAX_POS     8u

struct cli_cmd;

/* Parsed command line for one command. Values point into argv, which outlives
 * every use of them, so nothing here is owned or freed. */
typedef struct {
    const struct cli_cmd *cmd;
    const char           *prog;
    int                   help_requested;
    unsigned              seen[CLI_MAX_OPTS];
    const char           *val[CLI_MAX_OPTS][CLI_MAX_VALUES];
    size_t                nval[CLI_MAX_OPTS];
    const char           *pos[CLI_MAX_POS];
    size_t                npos;
} cli_args;

typedef struct cli_cmd {
    const char    *name;
    const char    *summary;   /* one line, for the command list */
    const char    *usage;     /* positional part, e.g. "<target>" */
    const cli_opt *opts;
    size_t         nopts;
    size_t         min_pos;
    size_t         max_pos;
    const char    *detail;    /* multi-line body, printed after the options */
    cli_status   (*run)(const cli_args *a);
} cli_cmd;

/* The context-sensitive help topic. NULL means "the program itself". */
void           cli_help_set_topic(const cli_cmd *cmd);
const cli_cmd *cli_help_topic(void);

/* Prints whichever topic is active: the command's help, or the program's
 * command list when none is set. Always to stdout, because help is what the
 * user asked for; the caller decides the exit status. */
void cli_help_print(void);

/* Registers the program's identity and command table once, at startup, so
 * the help printer can render the command list from the same data dispatch
 * uses. */
void cli_help_register(const char *prog, const char *tagline,
                       const cli_cmd *const *cmds, size_t ncmds,
                       const char *global_help);

/* Whitelist parse of one command's arguments. argv/argc cover the arguments
 * AFTER the command name. Returns CLI_ERR_USAGE with the error buffer set on
 * anything unknown, missing or repeated-but-not-repeatable. */
cli_status cli_parse(const cli_cmd *cmd, const char *prog,
                     int argc, char **argv, cli_args *out);

/* Accessors. `name` must be an option of the command that was parsed;
 * asking for one that is not is a programming error and aborts loudly under
 * a debug build rather than returning a plausible default. */
int         cli_flag(const cli_args *a, const char *name);
const char *cli_value(const cli_args *a, const char *name, const char *dflt);
size_t      cli_count(const cli_args *a, const char *name);
const char *cli_at(const cli_args *a, const char *name, size_t i);

/* Finds the command, sets the help topic, parses, runs. Global flags
 * (--verbose, --quiet, --help) are stripped before the command sees argv, so
 * they work in any position without every command whitelisting them. */
cli_status cli_dispatch(const cli_cmd *const *cmds, size_t ncmds,
                        const char *prog, int argc, char **argv);

#endif /* FALCON_CLI_OPT_H */
