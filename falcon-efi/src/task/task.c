/* The task switch. task.h has the contract; this is its x86-64 MS ABI half.
 *
 * task_switch(from, to) pushes the callee-saved registers onto the current
 * stack, stores the stack pointer in *from, loads it from to, pops the other
 * side's registers and returns on its stack. A task that has not run yet has
 * a frame built by task_start that pops into task_boot with the task in r12.
 *
 * The frame, from the saved stack pointer up: xmm6-xmm15 (160 bytes, 16-byte
 * aligned), 8 bytes of padding, r15, r14, r13, r12, rsi, rdi, rbx, rbp, the
 * return address. At the call into task_switch rsp is 8 below a multiple of
 * 16, so after eight pushes and 168 bytes it is on one, which movdqa needs.
 */

#include <task/task.h>

#include <lib/string.h>

void task_switch(void **from, void *to);
void task_boot(void);
void task_entry(task *t);

__asm__(
    ".text\n"
    ".globl task_switch\n"
    "task_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $168, %rsp\n"
    "    movdqa %xmm6, 0(%rsp)\n"
    "    movdqa %xmm7, 16(%rsp)\n"
    "    movdqa %xmm8, 32(%rsp)\n"
    "    movdqa %xmm9, 48(%rsp)\n"
    "    movdqa %xmm10, 64(%rsp)\n"
    "    movdqa %xmm11, 80(%rsp)\n"
    "    movdqa %xmm12, 96(%rsp)\n"
    "    movdqa %xmm13, 112(%rsp)\n"
    "    movdqa %xmm14, 128(%rsp)\n"
    "    movdqa %xmm15, 144(%rsp)\n"
    "    movq %rsp, (%rcx)\n"
    "    movq %rdx, %rsp\n"
    "    movdqa 0(%rsp), %xmm6\n"
    "    movdqa 16(%rsp), %xmm7\n"
    "    movdqa 32(%rsp), %xmm8\n"
    "    movdqa 48(%rsp), %xmm9\n"
    "    movdqa 64(%rsp), %xmm10\n"
    "    movdqa 80(%rsp), %xmm11\n"
    "    movdqa 96(%rsp), %xmm12\n"
    "    movdqa 112(%rsp), %xmm13\n"
    "    movdqa 128(%rsp), %xmm14\n"
    "    movdqa 144(%rsp), %xmm15\n"
    "    addq $168, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rsi\n"
    "    popq %rdi\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    /* A new task's first return lands here with the task in r12. The call
     * wants rsp on a multiple of 16 with 32 bytes of shadow space above
     * the return address; task_entry never returns. */
    ".globl task_boot\n"
    "task_boot:\n"
    "    andq $-16, %rsp\n"
    "    subq $32, %rsp\n"
    "    movq %r12, %rcx\n"
    "    call task_entry\n"
    "    ud2\n");

/* The task running now, for task_yield; NULL in the main flow. */
static task *g_cur;

void task_entry(task *t)
{
    t->fn(t->arg);
    /* Finished: back to the main flow for the last time. The stack is not
     * returned to, so nothing after the switch runs. */
    t->state = TASK_IDLE;
    g_cur = (task *)0;
    task_switch(&t->sp, t->main_sp);
}

void task_start(task *t, uint8_t *stack, size_t stack_len, task_fn fn,
                void *arg)
{
    uint64_t top;
    uint64_t *f;

    if (t == (task *)0 || stack == (uint8_t *)0 || stack_len < 4096u ||
        fn == (task_fn)0)
        return;
    t->stack = stack;
    t->stack_len = stack_len;
    t->fn = fn;
    t->arg = arg;

    /* The frame task_switch pops: 168 bytes of xmm and padding, eight
     * registers, the return address. Its base is 16-aligned, so the xmm
     * area is. */
    top = ((uint64_t)(uintptr_t)(stack + stack_len)) & ~(uint64_t)15u;
    top -= 128u;                            /* a little headroom */
    f = (uint64_t *)(uintptr_t)(top - (168u + 9u * 8u));
    memset(f, 0, 168u + 9u * 8u);
    f[21] = 0;                                  /* r15 */
    f[22] = 0;                                  /* r14 */
    f[23] = 0;                                  /* r13 */
    f[24] = (uint64_t)(uintptr_t)t;             /* r12: the task */
    f[25] = 0;                                  /* rsi */
    f[26] = 0;                                  /* rdi */
    f[27] = 0;                                  /* rbx */
    f[28] = 0;                                  /* rbp */
    f[29] = (uint64_t)(uintptr_t)task_boot;     /* the return address */
    t->sp = f;
    t->state = TASK_READY;
}

int task_resume(task *t)
{
    if (t == (task *)0 || t->state != TASK_READY || g_cur != (task *)0)
        return 0;
    t->state = TASK_RUNNING;
    g_cur = t;
    task_switch(&t->main_sp, t->sp);
    return t->state == TASK_READY;
}

void task_yield(void)
{
    task *t = g_cur;

    if (t == (task *)0)
        return;
    t->state = TASK_READY;
    g_cur = (task *)0;
    task_switch(&t->sp, t->main_sp);
    /* Resumed: task_resume set RUNNING and g_cur again. */
}

int task_in(void)
{
    return g_cur != (task *)0;
}

void task_abandon(task *t)
{
    if (t == (task *)0 || t->state != TASK_READY)
        return;
    t->state = TASK_IDLE;
    t->sp = (void *)0;
}
