#include <cli/mkstick/cleanup.h>

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cli/common/log.h>
#include <cli/common/proc.h>
#include <cli/common/util.h>

#define CLEANUP_MAX  16u
#define CLEANUP_ARG 4096u

typedef enum {
    CL_MOUNT = 0,
    CL_CRYPT,
    CL_LOOP,
    CL_RMDIR,
    CL_SHREDDIR
} cleanup_kind;

typedef struct {
    cleanup_kind kind;
    char         arg[CLEANUP_ARG];
} cleanup_ent;

static cleanup_ent g_stack[CLEANUP_MAX];
static unsigned    g_depth;
static int         g_unwinding;

static cli_status push(cleanup_kind kind, const char *arg)
{
    if (g_depth >= CLEANUP_MAX)
        return set_error(CLI_ERR_INTERNAL,
                         "cleanup stack full (%u entries)",
                         (unsigned)CLEANUP_MAX);
    if (strlen(arg) >= CLEANUP_ARG)
        return set_error(CLI_ERR_RANGE, "cleanup argument too long: %s", arg);

    g_stack[g_depth].kind = kind;
    (void)snprintf(g_stack[g_depth].arg, CLEANUP_ARG, "%s", arg);
    g_depth++;
    return CLI_OK;
}

cli_status cleanup_push_mount(const char *mountpoint)
{
    return push(CL_MOUNT, mountpoint);
}

cli_status cleanup_push_crypt(const char *mapper_name)
{
    return push(CL_CRYPT, mapper_name);
}

cli_status cleanup_push_loop(const char *loop_dev)
{
    return push(CL_LOOP, loop_dev);
}

cli_status cleanup_push_rmdir(const char *dir)
{
    return push(CL_RMDIR, dir);
}

cli_status cleanup_push_shreddir(const char *dir)
{
    return push(CL_SHREDDIR, dir);
}

static cli_status shred_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    cli_status rc = CLI_OK;

    if (d == NULL) {
        if (errno == ENOENT)
            return CLI_OK;
        return set_error_errno(CLI_ERR_IO, "opening %s to wipe it", dir);
    }
    while ((e = readdir(d)) != NULL) {
        char path[CLEANUP_ARG];
        struct stat st;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (util_join(path, sizeof path, dir, e->d_name) != CLI_OK)
            continue;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (util_shred(path) != CLI_OK)
            rc = CLI_ERR_IO;
    }
    (void)closedir(d);

    if (rmdir(dir) != 0 && errno != ENOENT)
        return set_error_errno(CLI_ERR_IO, "rmdir %s", dir);
    return rc;
}

static cli_status release(const cleanup_ent *e)
{
    switch (e->kind) {
    case CL_MOUNT:
        /* sync first: umount does flush, but a failed umount that leaves the
         * writes in page cache is the one case where the image on disk would
         * differ from what we just staged. */
        sync();
        return proc_runl_quiet("umount", e->arg, PROC_END);
    case CL_CRYPT:
        return proc_runl_quiet("cryptsetup", "close", e->arg, PROC_END);
    case CL_LOOP:
        return proc_runl_quiet("losetup", "-d", e->arg, PROC_END);
    case CL_RMDIR:
        if (rmdir(e->arg) != 0)
            return set_error_errno(CLI_ERR_IO, "rmdir %s", e->arg);
        return CLI_OK;
    case CL_SHREDDIR:
        return shred_dir(e->arg);
    }
    return set_error(CLI_ERR_INTERNAL, "unknown cleanup kind %d",
                     (int)e->kind);
}

cli_status cleanup_pop(void)
{
    cli_status rc;

    if (g_depth == 0)
        return set_error(CLI_ERR_INTERNAL, "cleanup_pop with nothing held");

    g_depth--;
    rc = release(&g_stack[g_depth]);
    if (rc != CLI_OK)
        return rc;
    return CLI_OK;
}

void cleanup_unwind_all(void)
{
    if (g_unwinding)
        return;
    g_unwinding = 1;

    while (g_depth > 0) {
        g_depth--;
        /* Best effort: a failure here is reported and stepped over, because
         * the entries below it still have to be released. */
        if (release(&g_stack[g_depth]) != CLI_OK)
            log_warn("%s", cli_error_msg());
    }
    g_unwinding = 0;
}

static void on_signal(int sig)
{
    /* Not async-signal-safe, on purpose; see the header. */
    log_warn("interrupted (signal %d); releasing loop devices and mounts",
             sig);
    cleanup_unwind_all();
    _exit(1);
}

void cleanup_init(void)
{
    struct sigaction sa;

    if (atexit(cleanup_unwind_all) != 0)
        log_warn("could not install the exit-time cleanup handler");

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGHUP, &sa, NULL);
}

unsigned cleanup_depth(void)
{
    return g_depth;
}
