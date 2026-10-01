/* The task switch. task.h has the contract; this is its i386 half, the
 * mirror of falcon-efi/src/task/task.c.
 *
 * task_switch(from, to) pushes ebp, ebx, esi and edi onto the current stack,
 * stores the stack pointer in *from, loads it from to, pops the other side's
 * and returns on its stack. A task that has not run yet has a frame built by
 * task_start that pops into task_boot with the task in ebx.
 *
 * The frame, from the saved stack pointer up: edi, esi, ebx, ebp, the return
 * address. The processor floor is the Pentium (DESIGN Section 14.8), so the
 * unreachable end of task_boot is a jump to itself rather than ud2, which
 * came with the Pentium Pro.
 */

#include <task/task.h>

void task_switch(void **from, void *to);
void task_boot(void);
void task_entry(task *t);

__asm__(
    ".text\n"
    ".globl task_switch\n"
    "task_switch:\n"
    "    movl 4(%esp), %eax\n"
    "    movl 8(%esp), %edx\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    movl %esp, (%eax)\n"
    "    movl %edx, %esp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    popl %ebp\n"
    "    ret\n"
    /* A new task's first return lands here with the task in ebx. */
    ".globl task_boot\n"
    "task_boot:\n"
    "    andl $-16, %esp\n"
    "    subl $12, %esp\n"
    "    pushl %ebx\n"
    "    call task_entry\n"
    "1:  jmp 1b\n");

/* The task running now, for task_yield; NULL in the main flow. */
static task *g_cur;

void task_entry(task *t)
{
    t->fn(t->arg);
    /* Finished: back to the main flow for the last time. */
    t->state = TASK_IDLE;
    g_cur = (task *)0;
    task_switch(&t->sp, t->main_sp);
}

void task_start(task *t, uint8_t *stack, size_t stack_len, task_fn fn,
                void *arg)
{
    uint32_t top;
    uint32_t *f;
    unsigned i;

    if (t == (task *)0 || stack == (uint8_t *)0 || stack_len < 4096u ||
        fn == (task_fn)0)
        return;
    t->stack = stack;
    t->stack_len = stack_len;
    t->fn = fn;
    t->arg = arg;

    top = ((uint32_t)(uintptr_t)(stack + stack_len)) & ~(uint32_t)15u;
    top -= 64u;                                 /* a little headroom */
    f = (uint32_t *)(uintptr_t)(top - 5u * 4u);
    for (i = 0; i < 5u; i++)
        f[i] = 0;
    f[2] = (uint32_t)(uintptr_t)t;              /* ebx: the task */
    f[4] = (uint32_t)(uintptr_t)task_boot;      /* the return address */
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
