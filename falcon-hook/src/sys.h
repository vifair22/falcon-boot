/* falcon-hook: the handful of Linux system calls the hook makes, with no libc.
 *
 * The hook runs as PID 1 inside somebody else's initramfs, before that
 * initramfs's own init. It cannot assume any libc is in there (Alpine's is
 * musl, most are glibc, some are busybox-only) and it is appended to every
 * direct boot, so it is built freestanding: these wrappers, falcon-core's
 * mem* routines, and nothing else. The result is a few tens of kilobytes
 * instead of the most part of a megabyte a static glibc costs.
 *
 * x86-64 only, like the loader that carries it. Each wrapper returns the raw
 * kernel result: a non-negative value, or -errno.
 */

#ifndef FALCON_HOOK_SYS_H
#define FALCON_HOOK_SYS_H

#include <stddef.h>
#include <stdint.h>

long sys_read(int fd, void *buf, size_t n);
long sys_write(int fd, const void *buf, size_t n);
long sys_pread(int fd, void *buf, size_t n, uint64_t off);
long sys_open(const char *path, int flags, int mode);
long sys_close(int fd);
long sys_ioctl(int fd, unsigned long req, void *arg);
long sys_mount(const char *src, const char *dst, const char *type,
               unsigned long flags, const void *data);
long sys_umount(const char *dst);
long sys_mkdir(const char *path, int mode);
long sys_mknod(const char *path, int mode, unsigned long dev);
long sys_symlink(const char *target, const char *path);
long sys_unlink(const char *path);
/* struct utsname: six fields of 65 bytes; the release is the third. */
long sys_uname(void *buf);
#define HOOK_UTS_FIELD 65u
/* 0 when path exists (faccessat F_OK), -errno otherwise. */
long sys_exists(const char *path);
long sys_getdents64(int fd, void *buf, size_t n);
long sys_execve(const char *path, char *const argv[], char *const envp[]);
long sys_fork(void);
long sys_wait(long pid, int *status);
long sys_sleep_ms(unsigned ms);
void sys_exit(int code) __attribute__((noreturn));

/* The flags and modes used, from the kernel's ABI rather than a libc. */
#define HOOK_O_RDONLY   0
#define HOOK_O_WRONLY   1
#define HOOK_O_RDWR     2
#define HOOK_O_CREAT    0100
#define HOOK_O_TRUNC    01000
#define HOOK_O_CLOEXEC  02000000
/* O_DIRECTORY is one of the few that differ: on arm64 0200000 is O_DIRECT. */
#if defined(__aarch64__)
#define HOOK_O_DIRECTORY 040000
#else
#define HOOK_O_DIRECTORY 0200000
#endif
#define HOOK_S_IFBLK    0060000
#define HOOK_S_IFCHR    0020000
#define HOOK_EEXIST     17
#define HOOK_EBUSY      16

/* The entry point the startup code calls, with the kernel's argv and envp. */
int hook_main(int argc, char **argv, char **envp);

#endif /* FALCON_HOOK_SYS_H */
