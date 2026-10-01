/* falcon-hook: startup and system calls, x86 Linux, in either width. See
 * sys.h.
 *
 * The hook is built twice, x86-64 and i686, because it runs inside the
 * booted kernel's initramfs and has to be the kernel's width: falcon-bios
 * boots 32-bit kernels as well as 64-bit ones (DESIGN Section 6.4).
 * Everything above this file is the same source for both. What
 * differs is how a system call is made and how the process starts, and each
 * of those is one short block below; <asm/unistd.h> gives the right numbers
 * for whichever width the compiler is targeting.
 */

#include "sys.h"

#include <asm/unistd.h>

#define AT_FDCWD (-100)
#define SIGCHLD  17

#if defined(__x86_64__)

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

/* Opening a file larger than 2 GiB is ordinary on x86-64. */
#define O_LARGE 0

#elif defined(__i386__)

/* int $0x80 with the arguments in ebx, ecx, edx, esi, edi. A sixth would go
 * in ebp, which the compiler will not hand over as an operand; nothing here
 * makes a call with six, so f is required to be zero rather than carried. */
static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;

    (void)f;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                     : "memory");
    return ret;
}

/* O_LARGEFILE, which a 32-bit process has to ask for or an ISO past 2 GiB
 * fails to open with EOVERFLOW. */
#define O_LARGE 0100000

#else
#error "falcon-hook is built for x86-64 and i686 only"
#endif

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
#if defined(__i386__)
    /* The offset in two registers, low half first. */
    return SC(__NR_pread64, fd, buf, n, (uint32_t)off, (uint32_t)(off >> 32),
              0);
#else
    return SC(__NR_pread64, fd, buf, n, off, 0, 0);
#endif
}

long sys_open(const char *path, int flags, int mode)
{
    return SC(__NR_openat, AT_FDCWD, path, flags | O_LARGE, mode, 0, 0);
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

long sys_uname(void *buf)
{
    return SC(__NR_uname, buf, 0, 0, 0, 0, 0);
}

long sys_unlink(const char *path)
{
    return SC(__NR_unlinkat, AT_FDCWD, path, 0, 0, 0, 0);
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
 * returns from it. The pointer to them travels in rdi on x86-64 and on the
 * stack on i686, which is each ABI's first argument. */
void hook_start(long *sp) __attribute__((used, noreturn));

void hook_start(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(void *)(sp + 1);
    char **envp = argv + argc + 1;

    sys_exit(hook_main(argc, argv, envp));
}

#if defined(__x86_64__)
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %rbp, %rbp\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call hook_start\n"
        "    ud2\n");
#else
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    mov %esp, %eax\n"
        "    and $-16, %esp\n"
        "    sub $12, %esp\n"
        "    push %eax\n"
        "    call hook_start\n"
        "    ud2\n");
#endif
