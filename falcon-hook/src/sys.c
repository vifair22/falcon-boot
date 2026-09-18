/* falcon-hook: startup and system calls, x86-64 Linux. See sys.h. */

#include "sys.h"

#include <asm/unistd.h>

#define AT_FDCWD (-100)
#define SIGCHLD  17

static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SC(n, a, b, c, d, e, f) \
    sc6((long)(n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), \
        (long)(f))

long sys_read(int fd, void *buf, size_t n)
{
    return SC(__NR_read, fd, buf, n, 0, 0, 0);
}

long sys_write(int fd, const void *buf, size_t n)
{
    return SC(__NR_write, fd, buf, n, 0, 0, 0);
}

long sys_pread(int fd, void *buf, size_t n, uint64_t off)
{
    return SC(__NR_pread64, fd, buf, n, off, 0, 0);
}

long sys_open(const char *path, int flags, int mode)
{
    return SC(__NR_openat, AT_FDCWD, path, flags, mode, 0, 0);
}

long sys_close(int fd)
{
    return SC(__NR_close, fd, 0, 0, 0, 0, 0);
}

long sys_ioctl(int fd, unsigned long req, void *arg)
{
    return SC(__NR_ioctl, fd, req, arg, 0, 0, 0);
}

long sys_mount(const char *src, const char *dst, const char *type,
               unsigned long flags, const void *data)
{
    return SC(__NR_mount, src, dst, type, flags, data, 0);
}

long sys_umount(const char *dst)
{
    return SC(__NR_umount2, dst, 0, 0, 0, 0, 0);
}

long sys_mkdir(const char *path, int mode)
{
    return SC(__NR_mkdirat, AT_FDCWD, path, mode, 0, 0, 0);
}

long sys_mknod(const char *path, int mode, unsigned long dev)
{
    return SC(__NR_mknodat, AT_FDCWD, path, mode, dev, 0, 0);
}

long sys_symlink(const char *target, const char *path)
{
    return SC(__NR_symlinkat, target, AT_FDCWD, path, 0, 0, 0);
}

long sys_exists(const char *path)
{
    return SC(__NR_faccessat, AT_FDCWD, path, 0, 0, 0, 0);
}

long sys_getdents64(int fd, void *buf, size_t n)
{
    return SC(__NR_getdents64, fd, buf, n, 0, 0, 0);
}

long sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return SC(__NR_execve, path, argv, envp, 0, 0, 0);
}

/* fork is clone with nothing shared and SIGCHLD on exit. */
long sys_fork(void)
{
    return SC(__NR_clone, SIGCHLD, 0, 0, 0, 0, 0);
}

long sys_wait(long pid, int *status)
{
    return SC(__NR_wait4, pid, status, 0, 0, 0, 0);
}

long sys_sleep_ms(unsigned ms)
{
    long ts[2];

    ts[0] = (long)(ms / 1000u);
    ts[1] = (long)(ms % 1000u) * 1000000L;
    return SC(__NR_nanosleep, ts, 0, 0, 0, 0, 0);
}

void sys_exit(int code)
{
    for (;;)
        (void)SC(__NR_exit_group, code, 0, 0, 0, 0, 0);
}

/* The process entry point: the kernel starts PID 1 here with argc, argv and
 * envp on the stack. The stack is realigned for the C call, and nothing
 * returns from it. */
void hook_start(long *sp) __attribute__((used, noreturn));

void hook_start(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(void *)(sp + 1);
    char **envp = argv + argc + 1;

    sys_exit(hook_main(argc, argv, envp));
}

__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %rbp, %rbp\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call hook_start\n"
        "    ud2\n");
