/* falcon-boot mkstick: the unwind stack for kernel-visible resources.
 *
 * Building a stick leaves three kinds of thing attached to the running
 * kernel: loop devices, device-mapper targets from `cryptsetup open`, and
 * mounts. Every one of them outlives this process if it is not released, and
 * a half-built stick that leaves /dev/mapper/falcon-p3 behind is a stick that
 * cannot be rebuilt without the user knowing what cryptsetup is.
 *
 * So they go on a stack and come off in reverse order, on every exit path:
 * success, error return, and a signal. Registration happens immediately after
 * the resource exists, never before and never batched at the end.
 *
 * The signal handler deliberately does work that is not async-signal-safe
 * (it forks umount and cryptsetup). That is a considered trade: the
 * alternative, a flag the main loop polls, does nothing at all when the
 * process is blocked in waitpid on a cryptsetup that will not return, which
 * is exactly when a user reaches for Ctrl-C. Leaving a mapper and a loop
 * device attached is the worse failure, so the handler unwinds and exits.
 */

#ifndef FALCON_MKSTICK_CLEANUP_H
#define FALCON_MKSTICK_CLEANUP_H

#include <common/status.h>

/* Installs the atexit and signal handlers. Call once, early. */
void cleanup_init(void);

cli_status cleanup_push_mount(const char *mountpoint);
cli_status cleanup_push_crypt(const char *mapper_name);
cli_status cleanup_push_loop(const char *loop_dev);
cli_status cleanup_push_rmdir(const char *dir);

/* A directory that held key material: every regular file in it is overwritten
 * and unlinked before the directory goes. Registered the moment the directory
 * is created, so that a Ctrl-C between `openssl req` and the copy onto p3
 * cannot leave the private key sitting in /tmp. */
cli_status cleanup_push_shreddir(const char *dir);

/* Release the most recently registered resource, reporting failure. This is
 * the NORMAL path: the flow closes what it opened as it finishes with it, and
 * a umount that fails is an error worth stopping for, not a shrug. */
cli_status cleanup_pop(void);

/* Release everything, best effort, ignoring failures. The panic path. */
void cleanup_unwind_all(void);

/* How many resources are still held; the flow asserts this is zero when it
 * believes it has finished. */
unsigned cleanup_depth(void);

#endif /* FALCON_MKSTICK_CLEANUP_H */
