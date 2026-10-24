/* The task switch. task.h has the contract; this is its architecture half.
 *
 * task_switch(from, to) saves the callee-saved registers on the current
 * stack, stores the stack pointer in *from, loads it from to, restores the
 * other side's registers and returns on its stack. A task that has not run
 * yet has a frame built by task_start that returns into task_boot with the
 * task in a callee-saved register.
 *
 * x86-64, MS ABI. The frame, from the saved stack pointer up: xmm6-xmm15
 * (160 bytes, 16-byte aligned), 8 bytes of padding, r15, r14, r13, r12,
 * rsi, rdi, rbx, rbp, the return address. At the call into task_switch rsp
 * is 8 below a multiple of 16, so after eight pushes and 168 bytes it is on
 * one, which movdqa needs. The task rides in r12.
 *
 * AArch64, AAPCS64. The frame is 160 bytes: x19-x28, x29 and x30 (the
 * return address) in pairs, then d8-d15, the low halves of v8-v15, which
 * are all of those the convention makes the callee's. sp stays a multiple
 * of 16 throughout, as the architecture checks. The task rides in x19.
 */

#include <task/task.h>

#include <lib/string.h>

void task_switch(void **from, void *to);
void task_boot(void);
void task_entry(task *t);

#if defined(__x86_64__)

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

#define TASK_FRAME_BYTES (168u + 9u * 8u)

/* The frame task_switch pops: 168 bytes of xmm and padding, eight
 * registers, the return address. */
static void task_frame(uint64_t *f, task *t)
{
    f[24] = (uint64_t)(uintptr_t)t;             /* r12: the task */
    f[29] = (uint64_t)(uintptr_t)task_boot;     /* the return address */
}

#elif defined(__aarch64__)

__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl task_switch\n"
    "task_switch:\n"
    "    sub sp, sp, #160\n"
    "    stp x19, x20, [sp, #0]\n"
    "    stp x21, x22, [sp, #16]\n"
    "    stp x23, x24, [sp, #32]\n"
    "    stp x25, x26, [sp, #48]\n"
    "    stp x27, x28, [sp, #64]\n"
    "    stp x29, x30, [sp, #80]\n"
    "    stp d8, d9, [sp, #96]\n"
    "    stp d10, d11, [sp, #112]\n"
    "    stp d12, d13, [sp, #128]\n"
    "    stp d14, d15, [sp, #144]\n"
    "    mov x9, sp\n"
    "    str x9, [x0]\n"
    "    mov sp, x1\n"
    "    ldp x19, x20, [sp, #0]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x23, x24, [sp, #32]\n"
    "    ldp x25, x26, [sp, #48]\n"
    "    ldp x27, x28, [sp, #64]\n"
    "    ldp x29, x30, [sp, #80]\n"
    "    ldp d8, d9, [sp, #96]\n"
    "    ldp d10, d11, [sp, #112]\n"
    "    ldp d12, d13, [sp, #128]\n"
    "    ldp d14, d15, [sp, #144]\n"
    "    add sp, sp, #160\n"
    "    ret\n"
    /* A new task's first return lands here with the task in x19 and sp on
     * the top of its frame, a multiple of 16; task_entry never returns. */
    ".p2align 2\n"
    ".globl task_boot\n"
    "task_boot:\n"
    "    mov x0, x19\n"
    "    bl task_entry\n"
    "    brk #0\n");

#define TASK_FRAME_BYTES 160u

/* x19 is the first word of the frame, x30 the twelfth. */
static void task_frame(uint64_t *f, task *t)
{
    f[0] = (uint64_t)(uintptr_t)t;              /* x19: the task */
    f[11] = (uint64_t)(uintptr_t)task_boot;     /* x30: the return address */
}

#else
#error "task.c: no task switch for this architecture"
#endif

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

    /* The frame's base is 16-aligned, and so is every part of it the
     * switch needs aligned. */
    top = ((uint64_t)(uintptr_t)(stack + stack_len)) & ~(uint64_t)15u;
    top -= 128u;                            /* a little headroom */
    f = (uint64_t *)(uintptr_t)(top - TASK_FRAME_BYTES);
    memset(f, 0, TASK_FRAME_BYTES);
    task_frame(f, t);
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
