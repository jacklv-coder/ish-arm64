#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "emu/tlb.h"

__thread struct task *current;

static struct pid pids[MAX_PID + 1] = {};
lock_t pids_lock = LOCK_INITIALIZER;

static bool pid_empty(struct pid *pid) {
    return pid->task == NULL && list_empty(&pid->session) && list_empty(&pid->pgroup);
}

struct pid *pid_get(dword_t id) {
    if (id >= sizeof(pids)/sizeof(pids[0]))
        return NULL;
    struct pid *pid = &pids[id];
    if (pid_empty(pid))
        return NULL;
    return pid;
}

struct task *pid_get_task_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct task *task = pid->task;
    return task;
}

struct task *pid_get_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && task->zombie)
        return NULL;
    return task;
}

struct task *task_create_(struct task *parent) {
    lock(&pids_lock);
    static int cur_pid = 0;
    do {
        cur_pid++;
        if (cur_pid > MAX_PID) cur_pid = 1;
    } while (!pid_empty(&pids[cur_pid]));
    struct pid *pid = &pids[cur_pid];
    pid->id = cur_pid;
    list_init(&pid->session);
    list_init(&pid->pgroup);

    struct task *task = malloc(sizeof(struct task));
    if (task == NULL) {
        unlock(&pids_lock);
        return NULL;
    }
    *task = (struct task) {};
    if (parent != NULL)
        *task = *parent;
    task->pid = pid->id;

    // procfs obtains task pointers under pids_lock and may immediately take
    // task-local locks. Initialize copied synchronization state before
    // publishing the task through either the PID table or its parent's list.
    lock_init(&task->general_lock);
    lock_init(&task->native_lock);
    lock_init(&task->ptrace.lock);
    cond_init(&task->ptrace.cond);
    atomic_init(&task->force_detached, false);
    atomic_init(&task->exit_state, TASK_EXIT_RUNNING);
    atomic_init(&task->thread_started, false);
    atomic_init(&task->cancellable_host_io, false);
    task->thread = zero_init(pthread_t);
    task->native_pid = 0;
    task->is_native_proxy = false;
    task->native_stdout_thread = zero_init(pthread_t);
    task->native_stderr_thread = zero_init(pthread_t);
    task->force_detached_files = NULL;
#ifdef GUEST_ARM64
    // Invalidate exclusive monitor after copying parent state.
    // Child must not inherit parent's LDXR reservation, as any context
    // switch or interrupt (including fork/clone) invalidates exclusive state.
    task->cpu.excl_addr = UINT64_MAX;
#endif

    // Initialize blocking state for deadlock detection.
    task->blocking = false;
    {
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        task->last_unblocked_ns = (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec;
    }
    list_init(&task->children);
    list_init(&task->siblings);
    list_init(&task->group_links);
    if (parent != NULL) {
        task->parent = parent;
        list_add(&parent->children, &task->siblings);
    }

    task->pending = 0;
    list_init(&task->queue);
    task->clear_tid = 0;
    task->robust_list = 0;
    task->futex_pipe[0] = -1;
    task->futex_pipe[1] = -1;
    task->did_exec = false;

    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);

    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    lock_init(&task->waiting_cond_lock);
    cond_init(&task->pause);

    // Publish only after every field that shutdown, signal delivery and procfs
    // may inspect has been initialized. Callers still finish clone ownership
    // before task_start(); embedded halt treats an unstarted task as in-flight
    // construction and waits for the caller to either start or destroy it.
    pid->task = task;
    unlock(&pids_lock);

    return task;
}

// Deferred-free list for task structs.
// When a task is destroyed, its struct is not immediately freed — instead it's
// placed on this list. The NEXT call to task_destroy will free previously
// deferred structs. This gives leaked/exiting pthreads time to finish accessing
// `current` before the memory is recycled by malloc, preventing use-after-free
// heap corruption.
#define DEFERRED_FREE_MAX 64
static struct task *deferred_free_list[DEFERRED_FREE_MAX];
static int deferred_free_count = 0;
// Must be called with pids_lock held (task_destroy already requires this).
static void flush_deferred_frees(void) {
    for (int i = 0; i < deferred_free_count; i++) {
        free(deferred_free_list[i]);
        deferred_free_list[i] = NULL;
    }
    deferred_free_count = 0;
}

void task_unpublish_locked(struct task *task) {
    list_remove_safe(&task->siblings);
    struct pid *pid = pid_get(task->pid);
    if (pid != NULL && pid->task == task)
        pid->task = NULL;
}

void task_dispose_locked(struct task *task) {
    // pids_lock prevents a new ptrace lookup while this waits for an operation
    // that already pinned the task. Once the lock handoff completes, no ptrace
    // user can still reference the mutex or any task storage below.
    lock(&task->ptrace.lock);
    unlock(&task->ptrace.lock);

    // Flush old deferred frees first — they've had time to quiesce.
    flush_deferred_frees();

    // Zero the struct to poison stale `current` references, then defer the
    // actual free. This way if a leaked pthread is still running, it will
    // hit zeroed fields (NULL group, NULL mem) and crash cleanly rather than
    // silently corrupting a newly-allocated task at the same address.
    memset(task, 0, sizeof(struct task));

    if (deferred_free_count < DEFERRED_FREE_MAX) {
        deferred_free_list[deferred_free_count++] = task;
    } else {
        // Overflow — free immediately (rare, only with 64+ concurrent exits)
        free(task);
    }
}

void task_destroy(struct task *task) {
    task_unpublish_locked(task);
    task_dispose_locked(task);
}

static void task_run_tlb_cleanup(void *arg) {
    tlb_free((struct tlb *)arg);
}

void task_cancellable_host_io_begin(int *previous_state) {
    *previous_state = PTHREAD_CANCEL_DISABLE;
    if (current != NULL) {
        atomic_store(&current->cancellable_host_io, true);
        // Pair the publication above with the force-detach decision. If exit
        // won the race before cancellation was enabled, stop at this safe
        // boundary instead of entering a host call that nobody will cancel.
        if (atomic_load(&current->exit_state) == TASK_EXIT_FORCE_DETACHED) {
            atomic_store(&current->cancellable_host_io, false);
            task_finish_force_detached_exit();
        }
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, previous_state);
    }
}

void task_cancellable_host_io_end(int previous_state) {
    if (current != NULL) {
        pthread_setcancelstate(previous_state, NULL);
        atomic_store(&current->cancellable_host_io, false);
    }
}

void task_cancelled_exit_cleanup(void *arg) {
    struct task *task = arg;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    // A normal force-detached exit clears current before disposing the task.
    // pthread_exit still runs this registered handler, so do not dereference
    // the captured pointer unless this thread still owns that task.
    if (current != task)
        return;
    atomic_store(&task->cancellable_host_io, false);
    if (atomic_load(&task->exit_state) == TASK_EXIT_FORCE_DETACHED)
        task_cleanup_force_detached_exit();
}

void task_run_current() {
    struct task *task = current;
    // Directly adopted threads (the CLI PID 1) need the same cancellation
    // cleanup boundary as pthreads created by task_start().
    pthread_cleanup_push(task_cancelled_exit_cleanup, task);
    struct cpu_state *cpu = &current->cpu;
    struct tlb *tlb = calloc(1, sizeof(struct tlb));
    if (!tlb) die("could not allocate TLB");

    // Register cleanup so the TLB (and its fiber_frame) is freed even when
    // the thread exits via pthread_exit() from deep in handle_interrupt()
    // (e.g. do_exit() after a native-offloaded execve, or SIGKILL path).
    // Without this, every guest process leaks ~304KB of TLB + ~48KB of
    // fiber_frame, dominating app memory after repeated ffmpeg invocations.
    pthread_cleanup_push(task_run_tlb_cleanup, tlb);

    while (true) {
        // Check for group exit before entering JIT — this catches threads
        // returning from blocking host syscalls (futex, nanosleep, etc.)
        // that were interrupted by SIGUSR1 from do_exit_group.
        // Also bail if our task struct was destroyed (current zeroed or NULLed).
        struct task *self = current;
        if (self == NULL || self->group == NULL) {
            // Task struct was destroyed under us (leaked thread).
            // Exit the host thread silently; cleanup handler frees tlb.
            pthread_exit(NULL);
        }
        if (self->force_detached)
            task_finish_force_detached_exit();
        if (self->group->doing_group_exit) {
            do_exit(self->group->group_exit_code);
        }
        if (self->mem == NULL) {
            pthread_exit(NULL);
        }
        read_wrlock(&self->mem->lock);
        tlb_refresh(tlb, &self->mem->mmu);
        int interrupt = cpu_run_to_interrupt(cpu, tlb);
        read_wrunlock(&self->mem->lock);
        handle_interrupt(interrupt);
    }

    // Never reached in practice (loop only exits via pthread_exit/do_exit),
    // but the pop is required for pthread_cleanup_push/pop balance.
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(0);
}

static void *task_thread(void *vtask) {
    // Cancellation is an emergency wakeup for selected host I/O cancellation
    // points, never a general asynchronous task-kill mechanism. Keeping it
    // disabled everywhere else prevents cancellation while malloc or an iSH
    // lock is active.
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    current = vtask;
    // pthread_create may schedule this entry point before it has stored the
    // new pthread_t into the caller-provided task->thread slot. Wait until the
    // creator publishes that handle before any lifecycle path is allowed to
    // target it with pthread_kill/pthread_cancel.
    while (!atomic_load(&current->thread_started))
        sched_yield();
    // A group exit can claim a child after it is published but before
    // pthread_create schedules this entry point. Complete that claim before
    // touching any copied guest state.
    if (atomic_load(&current->exit_state) == TASK_EXIT_FORCE_DETACHED)
        task_finish_force_detached_exit();
    if (unblock_internal_signal() != 0)
        die("could not unblock internal signal for guest task");
    update_thread_name();
    task_run_current();
    die("task_thread returned"); // above function call should never return
}

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr() {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
}

void task_start(struct task *task) {
    if (pthread_create(&task->thread, &task_thread_attr, task_thread, task) != 0)
        die("could not create thread");
    atomic_store(&task->thread_started, true);
}

int task_start_joinable(struct task *task) {
    int err = pthread_create(&task->thread, NULL, task_thread, task);
    if (err == 0)
        atomic_store(&task->thread_started, true);
    return err;
}

void task_adopt_current_thread(struct task *task) {
    assert(current == task);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    task->thread = pthread_self();
    atomic_store(&task->thread_started, true);
}

int_t sys_sched_yield() {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name() {
    char name[16]; // As long as Linux will let us make this
    snprintf(name, sizeof(name), "-%d", current->pid);
    size_t pid_width = strlen(name);
    size_t name_width = snprintf(name, sizeof(name), "%s", current->comm);
    sprintf(name + (name_width < sizeof(name) - 1 - pid_width ? name_width : sizeof(name) - 1 - pid_width), "-%d", current->pid);
#if __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}
