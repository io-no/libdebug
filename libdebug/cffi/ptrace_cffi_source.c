#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>

struct ptrace_hit_bp {
    int pid;
    uint64_t addr;
    uint64_t bp_instruction;
    uint64_t prev_instruction;
};

struct software_breakpoint {
    uint64_t addr;
    uint64_t instruction;
    uint64_t patched_instruction;
    char enabled;
    struct software_breakpoint *next;
};

struct thread {
    int tid;
    struct user_regs_struct regs;
    struct thread *next;
};

struct thread_status {
    int tid;
    int status;
    struct thread_status *next;
};

struct thread *t_HEAD = NULL;

struct software_breakpoint *b_HEAD = NULL;

struct user_regs_struct *register_thread(int tid)
{
    // Verify if the thread is already registered
    struct thread *t = t_HEAD;
    while (t != NULL) {
        if (t->tid == tid) return &t->regs;
        t = t->next;
    }

    t = malloc(sizeof(struct thread));
    t->tid = tid;

    ptrace(PTRACE_GETREGS, tid, NULL, &t->regs);

    t->next = t_HEAD;
    t_HEAD = t;

    return &t->regs;
}

void unregister_thread(int tid)
{
    struct thread *t = t_HEAD;
    struct thread *prev = NULL;

    while (t != NULL) {
        if (t->tid == tid) {
            if (prev == NULL) {
                t_HEAD = t->next;
            } else {
                prev->next = t->next;
            }
            free(t);
            return;
        }
        prev = t;
        t = t->next;
    }
}

void free_thread_list()
{
    struct thread *t = t_HEAD;
    struct thread *next;

    while (t != NULL) {
        next = t->next;
        free(t);
        t = next;
    }

    t_HEAD = NULL;
}

int ptrace_trace_me(void)
{
    return ptrace(PTRACE_TRACEME, 0, NULL, NULL);
}

int ptrace_attach(int pid)
{
    return ptrace(PTRACE_ATTACH, pid, NULL, NULL);
}

int ptrace_detach(int pid)
{
    return ptrace(PTRACE_DETACH, pid, NULL, NULL);
}

void ptrace_set_options(int pid)
{
    int options = PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                  PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT;
    // int options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC |
    // PTRACE_O_TRACEEXIT;

    ptrace(PTRACE_SETOPTIONS, pid, NULL, options);
}

uint64_t ptrace_peekdata(int pid, uint64_t addr)
{
    // Since the value returned by a successful PTRACE_PEEK*
    // request may be -1, the caller must clear errno before the call,
    errno = 0;

    return ptrace(PTRACE_PEEKDATA, pid, (void *)addr, NULL);
}

uint64_t ptrace_pokedata(int pid, uint64_t addr, uint64_t data)
{
    return ptrace(PTRACE_POKEDATA, pid, (void *)addr, data);
}

uint64_t ptrace_peekuser(int pid, uint64_t addr)
{
    // Since the value returned by a successful PTRACE_PEEK*
    // request may be -1, the caller must clear errno before the call,
    errno = 0;

    return ptrace(PTRACE_PEEKUSER, pid, addr, NULL);
}

uint64_t ptrace_pokeuser(int pid, uint64_t addr, uint64_t data)
{
    return ptrace(PTRACE_POKEUSER, pid, addr, data);
}

uint64_t ptrace_geteventmsg(int pid)
{
    uint64_t data = 0;

    ptrace(PTRACE_GETEVENTMSG, pid, NULL, &data);

    return data;
}

int singlestep(int tid)
{
    // flush any register changes
    struct thread *t = t_HEAD;
    while (t != NULL) {
        if (ptrace(PTRACE_SETREGS, t->tid, NULL, &t->regs))
            perror("ptrace_setregs");
        t = t->next;
    }

    return ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
}

int step_until(int tid, uint64_t addr, int max_steps)
{
    // flush any register changes
    struct thread *t = t_HEAD, *stepping_thread = NULL;
    while (t != NULL) {
        if (ptrace(PTRACE_SETREGS, t->tid, NULL, &t->regs))
            perror("ptrace_setregs");

        if (t->tid == tid) stepping_thread = t;

        t = t->next;
    }

    int count = 0, status = 0;
    uint64_t previous_ip;

    if (!stepping_thread) {
        perror("Thread not found");
        return -1;
    }

    while (max_steps == -1 || count < max_steps) {
        if (ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL)) return -1;

        // wait for the child
        waitpid(tid, &status, 0);

        previous_ip = INSTRUCTION_POINTER(stepping_thread->regs);

        // update the registers
        ptrace(PTRACE_GETREGS, tid, NULL, &stepping_thread->regs);

        if (INSTRUCTION_POINTER(stepping_thread->regs) == addr) break;

        // if the instruction pointer didn't change, we have to step again
        // because we hit a hardware breakpoint
        if (INSTRUCTION_POINTER(stepping_thread->regs) == previous_ip) continue;

        count++;
    }

    return 0;
}

int cont_all_and_set_bps(int pid)
{
    int status = 0;

    // flush any register changes
    struct thread *t = t_HEAD;
    while (t != NULL) {
        if (ptrace(PTRACE_SETREGS, t->tid, NULL, &t->regs))
            fprintf(stderr, "ptrace_setregs failed for thread %d: %s\\n",
                    t->tid, strerror(errno));
        t = t->next;
    }

    // iterate over all the threads and check if any of them has hit a software
    // breakpoint
    t = t_HEAD;
    struct software_breakpoint *b;
    int t_hit;

    while (t != NULL) {
        t_hit = 0;
        uint64_t ip = INSTRUCTION_POINTER(t->regs);

        b = b_HEAD;
        while (b != NULL && !t_hit) {
            if (b->addr == ip)
                // we hit a software breakpoint on this thread
                t_hit = 1;

            b = b->next;
        }

        if (t_hit) {
            // step over the breakpoint
            if (ptrace(PTRACE_SINGLESTEP, t->tid, NULL, NULL)) return -1;

            // wait for the child
            waitpid(t->tid, &status, 0);

            // status == 4991 ==> (WIFSTOPPED(status) && WSTOPSIG(status) ==
            // SIGSTOP) this should happen only if threads are involved
            if (status == 4991) {
                ptrace(PTRACE_SINGLESTEP, t->tid, NULL, NULL);
                waitpid(t->tid, &status, 0);
            }
        }

        t = t->next;
    }

    // Reset any software breakpoint
    b = b_HEAD;
    while (b != NULL) {
        if (b->enabled) {
            ptrace(PTRACE_POKEDATA, pid, (void *)b->addr,
                   b->patched_instruction);
        }
        b = b->next;
    }

    // continue the execution of all the threads
    t = t_HEAD;
    while (t != NULL) {
        if (ptrace(PTRACE_CONT, t->tid, NULL, NULL))
            fprintf(stderr, "ptrace_cont failed for thread %d: %s\\n", t->tid,
                    strerror(errno));
        t = t->next;
    }

    return status;
}

struct thread_status *wait_all_and_update_regs(int pid)
{
    // Allocate the head of the list
    struct thread_status *head;
    head = malloc(sizeof(struct thread_status));

    // The first element is the first status we get from polling with waitpid
    head->tid = waitpid(-1, &head->status, __WALL);

    head->next = NULL;

    if (head->tid == -1) {
        free(head);
        perror("waitpid");
        return NULL;
    }

    // We must interrupt all the other threads with a SIGSTOP
    struct thread *t = t_HEAD;
    int temp_tid, temp_status;
    while (t != NULL) {
        if (t->tid != head->tid) {
            // If GETREGS succeeds, the thread is already stopped, so we must
            // not "stop" it again
            if (ptrace(PTRACE_GETREGS, t->tid, NULL, &t->regs) == -1) {
                // Stop the thread with a SIGSTOP
                tgkill(pid, t->tid, SIGSTOP);
                // Wait for the thread to stop
                temp_tid = waitpid(t->tid, &temp_status, NULL);

                // Register the status of the thread, as it might contain useful
                // information
                struct thread_status *ts = malloc(sizeof(struct thread_status));
                ts->tid = temp_tid;
                ts->status = temp_status;
                ts->next = head;
                head = ts;
            }
        }
        t = t->next;
    }

    // We keep polling but don't block, we want to get all the statuses we can
    while ((temp_tid = waitpid(-1, &temp_status, WNOHANG | __WALL)) > 0) {
        struct thread_status *ts = malloc(sizeof(struct thread_status));
        ts->tid = temp_tid;
        ts->status = temp_status;
        ts->next = head;
        head = ts;
    }

    // Update the registers of all the threads
    t = t_HEAD;
    while (t) {
        ptrace(PTRACE_GETREGS, t->tid, NULL, &t->regs);
        t = t->next;
    }

    // Restore any software breakpoint
    struct software_breakpoint *b = b_HEAD;

    while (b != NULL) {
        if (b->enabled) {
            ptrace(PTRACE_POKEDATA, pid, (void *)b->addr, b->instruction);
        }
        b = b->next;
    }

    return head;
}

void free_thread_status_list(struct thread_status *head)
{
    struct thread_status *next;

    while (head) {
        next = head->next;
        free(head);
        head = next;
    }
}

void register_breakpoint(int pid, uint64_t address)
{
    uint64_t instruction, patched_instruction;

    instruction = ptrace(PTRACE_PEEKDATA, pid, (void *)address, NULL);

    patched_instruction = INSTALL_BREAKPOINT(instruction);

    ptrace(PTRACE_POKEDATA, pid, (void *)address, patched_instruction);

    struct software_breakpoint *b = b_HEAD;

    while (b != NULL) {
        if (b->addr == address) {
            b->enabled = 1;
            return;
        }
        b = b->next;
    }

    b = malloc(sizeof(struct software_breakpoint));
    b->addr = address;
    b->instruction = instruction;
    b->patched_instruction = patched_instruction;
    b->enabled = 1;

    b->next = b_HEAD;
    b_HEAD = b;
}

void unregister_breakpoint(uint64_t address)
{
    struct software_breakpoint *b = b_HEAD;
    struct software_breakpoint *prev = NULL;

    while (b != NULL) {
        if (b->addr == address) {
            if (prev == NULL) {
                b_HEAD = b->next;
            } else {
                prev->next = b->next;
            }
            free(b);
            return;
        }
        prev = b;
        b = b->next;
    }
}

void disable_breakpoint(int pid, uint64_t address)
{
    struct software_breakpoint *b = b_HEAD;

    while (b != NULL) {
        if (b->addr == address) {
            b->enabled = 0;
            ptrace(PTRACE_POKEDATA, pid, b->addr, b->patched_instruction);
            return;
        }
        b = b->next;
    }
}

void free_breakpoints()
{
    struct software_breakpoint *b = b_HEAD;
    struct software_breakpoint *next;

    while (b != NULL) {
        next = b->next;
        free(b);
        b = next;
    }

    b_HEAD = NULL;
}
