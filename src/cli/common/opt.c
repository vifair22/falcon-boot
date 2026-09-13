#include <cli/common/opt.h>

#include <stdio.h>
#include <string.h>

#include <cli/common/log.h>

static const cli_cmd        *g_topic;
static const char           *g_prog = "falcon-boot-tool";
static const char           *g_tagline = "";
static const char           *g_global_help = "";
static const cli_cmd *const *g_cmds;
static size_t                g_ncmds;

void cli_help_set_topic(const cli_cmd *cmd)
{
    g_topic = cmd;
}

const cli_cmd *cli_help_topic(void)
{
    return g_topic;
}

void cli_help_register(const char *prog, const char *tagline,
                       const cli_cmd *const *cmds, size_t ncmds,
                       const char *global_help)
{
    g_prog = prog;
    g_tagline = tagline;
    g_cmds = cmds;
    g_ncmds = ncmds;
    g_global_help = global_help;
}

static void print_command_help(const cli_cmd *c)
{
    size_t i;

    printf("usage: %s %s%s%s\n", g_prog, c->name,
           (c->usage != NULL && c->usage[0] != '\0') ? " " : "",
           (c->usage != NULL) ? c->usage : "");
    printf("\n%s\n", c->summary);

    if (c->nopts > 0) {
        printf("\noptions:\n");
        for (i = 0; i < c->nopts; i++) {
            const cli_opt *o = &c->opts[i];
            char left[48];

            if (o->kind == CLI_OPT_VALUE)
                (void)snprintf(left, sizeof left, "--%s %s", o->name,
                               (o->arg != NULL) ? o->arg : "<value>");
            else
                (void)snprintf(left, sizeof left, "--%s", o->name);
            printf("  %-28s %s%s\n", left, o->help,
                   o->repeatable ? " (repeatable)" : "");
        }
    }

    printf("\nglobal options:\n%s", g_global_help);
    if (c->detail != NULL)
        printf("\n%s", c->detail);
}

static void print_program_help(void)
{
    size_t i;

    printf("usage: %s <command> [options]\n", g_prog);
    if (g_tagline[0] != '\0')
        printf("\n%s\n", g_tagline);

    printf("\ncommands:\n");
    for (i = 0; i < g_ncmds; i++)
        printf("  %-10s %s\n", g_cmds[i]->name, g_cmds[i]->summary);

    printf("\nglobal options:\n%s", g_global_help);
    printf("\nRun '%s <command> --help' for a command's own options.\n",
           g_prog);
}

void cli_help_print(void)
{
    if (g_topic != NULL)
        print_command_help(g_topic);
    else
        print_program_help();
}

static const cli_opt *find_opt(const cli_cmd *c, const char *name, size_t len,
                               size_t *idx)
{
    size_t i;

    for (i = 0; i < c->nopts; i++) {
        if (strlen(c->opts[i].name) == len &&
            strncmp(c->opts[i].name, name, len) == 0) {
            *idx = i;
            return &c->opts[i];
        }
    }
    return NULL;
}

/* Index of an option the CALLER named, which is always a literal from the
 * same static array. A miss means the code and the spec disagree, so it is
 * reported as an internal error rather than papered over. */
static int opt_index(const cli_args *a, const char *name, size_t *idx)
{
    size_t i;

    for (i = 0; i < a->cmd->nopts; i++) {
        if (strcmp(a->cmd->opts[i].name, name) == 0) {
            *idx = i;
            return 1;
        }
    }
    log_error("internal: command '%s' has no option --%s", a->cmd->name, name);
    return 0;
}

cli_status cli_parse(const cli_cmd *cmd, const char *prog,
                     int argc, char **argv, cli_args *out)
{
    int i;

    memset(out, 0, sizeof *out);
    out->cmd = cmd;
    out->prog = prog;

    if (cmd->nopts > CLI_MAX_OPTS)
        return set_error(CLI_ERR_INTERNAL,
                         "command '%s' declares %zu options, cap is %u",
                         cmd->name, cmd->nopts, (unsigned)CLI_MAX_OPTS);

    /* Early --help short-circuit: before any validation, so asking for help
     * never has to satisfy the rules it is asking about. */
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            out->help_requested = 1;
            return CLI_OK;
        }
    }

    for (i = 0; i < argc; i++) {
        const char *arg = argv[i];
        const char *eq;
        const cli_opt *o;
        size_t idx = 0;
        size_t namelen;

        if (arg[0] != '-' || arg[1] == '\0') {
            if (out->npos >= cmd->max_pos || out->npos >= CLI_MAX_POS)
                return set_error(CLI_ERR_USAGE,
                                 "%s %s: unexpected argument '%s'",
                                 prog, cmd->name, arg);
            out->pos[out->npos++] = arg;
            continue;
        }
        if (arg[1] != '-')
            return set_error(CLI_ERR_USAGE,
                             "%s %s: unknown option '%s' "
                             "(only long --options are accepted)",
                             prog, cmd->name, arg);

        eq = strchr(arg + 2, '=');
        namelen = (eq != NULL) ? (size_t)(eq - (arg + 2)) : strlen(arg + 2);
        o = find_opt(cmd, arg + 2, namelen, &idx);
        if (o == NULL)
            return set_error(CLI_ERR_USAGE, "%s %s: unknown option '%s'",
                             prog, cmd->name, arg);

        if (out->seen[idx] > 0 && !o->repeatable)
            return set_error(CLI_ERR_USAGE,
                             "%s %s: --%s given more than once",
                             prog, cmd->name, o->name);

        if (o->kind == CLI_OPT_FLAG) {
            if (eq != NULL)
                return set_error(CLI_ERR_USAGE,
                                 "%s %s: --%s takes no value",
                                 prog, cmd->name, o->name);
            out->seen[idx]++;
            continue;
        }

        if (eq != NULL) {
            if (eq[1] == '\0')
                return set_error(CLI_ERR_USAGE,
                                 "%s %s: --%s needs a value",
                                 prog, cmd->name, o->name);
            out->val[idx][out->nval[idx]] = eq + 1;
        } else {
            if (i + 1 >= argc)
                return set_error(CLI_ERR_USAGE,
                                 "%s %s: --%s needs a value",
                                 prog, cmd->name, o->name);
            i++;
            out->val[idx][out->nval[idx]] = argv[i];
        }
        if (out->nval[idx] + 1 >= CLI_MAX_VALUES)
            return set_error(CLI_ERR_USAGE,
                             "%s %s: --%s repeated more than %u times",
                             prog, cmd->name, o->name,
                             (unsigned)CLI_MAX_VALUES - 1u);
        out->nval[idx]++;
        out->seen[idx]++;
    }

    if (out->npos < cmd->min_pos)
        return set_error(CLI_ERR_USAGE, "%s %s: missing %s",
                         prog, cmd->name,
                         (cmd->usage != NULL) ? cmd->usage : "argument");

    return CLI_OK;
}

int cli_flag(const cli_args *a, const char *name)
{
    size_t idx;

    if (!opt_index(a, name, &idx))
        return 0;
    return a->seen[idx] > 0;
}

const char *cli_value(const cli_args *a, const char *name, const char *dflt)
{
    size_t idx;

    if (!opt_index(a, name, &idx))
        return dflt;
    if (a->nval[idx] == 0)
        return dflt;
    return a->val[idx][a->nval[idx] - 1];
}

size_t cli_count(const cli_args *a, const char *name)
{
    size_t idx;

    if (!opt_index(a, name, &idx))
        return 0;
    return a->nval[idx];
}

const char *cli_at(const cli_args *a, const char *name, size_t i)
{
    size_t idx;

    if (!opt_index(a, name, &idx))
        return NULL;
    if (i >= a->nval[idx])
        return NULL;
    return a->val[idx][i];
}

/* Global flags are removed from argv in place before the command's whitelist
 * runs. They are deliberately not part of any command's option array: every
 * command accepts them, and duplicating them into each spec would be five
 * chances to forget one. */
static int strip_globals(int argc, char **argv, int *help)
{
    int out = 0;
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            cli_log_set_level(CLI_LOG_DEBUG);
            continue;
        }
        if (strcmp(argv[i], "--quiet") == 0) {
            cli_log_set_level(CLI_LOG_WARN);
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            *help = 1;
            continue;
        }
        argv[out++] = argv[i];
    }
    return out;
}

cli_status cli_dispatch(const cli_cmd *const *cmds, size_t ncmds,
                        const char *prog, int argc, char **argv)
{
    const cli_cmd *cmd = NULL;
    cli_args args;
    cli_status rc;
    int help = 0;
    size_t i;

    argc = strip_globals(argc, argv, &help);

    if (argc < 1) {
        cli_help_set_topic(NULL);
        if (help) {
            cli_help_print();
            return CLI_OK;
        }
        return set_error(CLI_ERR_USAGE, "%s: no command given", prog);
    }

    for (i = 0; i < ncmds; i++) {
        if (strcmp(cmds[i]->name, argv[0]) == 0) {
            cmd = cmds[i];
            break;
        }
    }
    if (cmd == NULL) {
        cli_help_set_topic(NULL);
        return set_error(CLI_ERR_USAGE, "%s: unknown command '%s'",
                         prog, argv[0]);
    }

    /* Set before the parse and before the run, which is what makes any
     * failure below print THIS command's help. */
    cli_help_set_topic(cmd);

    if (help) {
        cli_help_print();
        return CLI_OK;
    }

    rc = cli_parse(cmd, prog, argc - 1, argv + 1, &args);
    if (rc != CLI_OK)
        return rc;
    if (args.help_requested) {
        cli_help_print();
        return CLI_OK;
    }

    return cmd->run(&args);
}
