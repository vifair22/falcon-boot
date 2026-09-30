/* falcon-efi: one task beside the main flow, on a stack of its own
 * (DESIGN Section 7.8, the background probe).
 *
 * A coroutine, not a thread. Nothing runs at the same time as anything
 * else: the main flow resumes the task, the task runs until it yields, and
 * the main flow carries on from the resume. What it buys is that the task's
 * state lives on its own stack, so it outlives whatever the main flow
 * returns from in between: a menu call ends for a folder change, the next
 * one resumes the same probe where it stopped.
 *
 * Why not a thread: an application processor may not call boot services,
 * Block I/O among them, and a timer event's notify function runs at raised
 * TPL, where the main flow cannot run until it returns.
 *
 * A switch saves what the MS x64 convention makes the callee's (rbx, rbp,
 * rdi, rsi, r12-r15, xmm6-xmm15) and the stack pointer, and nothing else.
 */

#ifndef FALCON_EFI_TASK_H
#define FALCON_EFI_TASK_H

#include <stdint.h>
#include <stddef.h>

typedef void (*task_fn)(void *arg);

typedef enum {
    TASK_IDLE = 0,   /* never started, finished, or abandoned */
    TASK_READY,      /* started and suspended at a yield; resume runs it */
    TASK_RUNNING     /* inside task_resume */
} task_state;

typedef struct {
    void      *sp;          /* the task's stack pointer while suspended */
    void      *main_sp;     /* the main flow's, while the task runs */
    uint8_t   *stack;       /* caller's memory, stack_len bytes */
    size_t     stack_len;
    task_fn    fn;
    void      *arg;
    task_state state;
} task;

/* Prepares t to run fn(arg) on stack (stack_len bytes, the caller's) at
 * the next task_resume. A task that is READY is abandoned first. */
void task_start(task *t, uint8_t *stack, size_t stack_len, task_fn fn,
                void *arg);

/* Runs t until it yields or fn returns. Nothing when t is IDLE. Returns
 * nonzero while t is READY (it will want resuming again). */
int task_resume(task *t);

/* From inside the running task: back to the main flow, which returns from
 * task_resume. Nothing when no task is running, so code shared with the
 * main flow may call it unconditionally. */
void task_yield(void);

/* Nonzero from inside a running task. */
int task_in(void);

/* Drops a READY task where it is suspended: its stack is forgotten and
 * nothing on it runs again. Whatever it had half done stays half done, so
 * the caller undoes what matters (the probe's entry is simply left
 * unprobed). */
void task_abandon(task *t);

#endif /* FALCON_EFI_TASK_H */
