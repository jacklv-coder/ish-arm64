#include <time.h>
#include "debug.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "kernel/calls.h"
#include "fs/tty.h"
#include "kernel/mm.h"
#include "kernel/ptrace.h"

// Architecture-specific register access for fork/clone
#if defined(GUEST_ARM64)
#define CPU_SP(cpu) ((cpu).sp)
#define CPU_RETVAL(cpu) ((cpu).regs[0])
#else
#define CPU_SP(cpu) ((cpu).esp)
#define CPU_RETVAL(cpu) ((cpu).eax)
#endif

#define CSIGNAL_ 0x000000ff
#define CLONE_VM_ 0x00000100
#define CLONE_FS_ 0x00000200
#define CLONE_FILES_ 0x00000400
#define CLONE_SIGHAND_ 0x00000800
#define CLONE_PTRACE_ 0x00002000
#define CLONE_VFORK_ 0x00004000
#define CLONE_PARENT_ 0x00008000
#define CLONE_THREAD_ 0x00010000
#define CLONE_NEWNS_ 0x00020000
#define CLONE_SYSVSEM_ 0x00040000
#define CLONE_SETTLS_ 0x00080000
#define CLONE_PARENT_SETTID_ 0x00100000
#define CLONE_CHILD_CLEARTID_ 0x00200000
#define CLONE_DETACHED_ 0x00400000
#define CLONE_UNTRACED_ 0x00800000
#define CLONE_CHILD_SETTID_ 0x01000000
#define CLONE_NEWCGROUP_ 0x02000000
#define CLONE_NEWUTS_ 0x04000000
#define CLONE_NEWIPC_ 0x08000000
#define CLONE_NEWUSER_ 0x10000000
#define CLONE_NEWPID_ 0x20000000
#define CLONE_NEWNET_ 0x40000000
#define CLONE_IO_ 0x80000000
#define IMPLEMENTED_FLAGS (CLONE_VM_|CLONE_FILES_|CLONE_FS_|CLONE_SIGHAND_|CLONE_SYSVSEM_|CLONE_VFORK_|CLONE_THREAD_|\
        CLONE_SETTLS_|CLONE_CHILD_SETTID_|CLONE_PARENT_SETTID_|CLONE_CHILD_CLEARTID_|CLONE_DETACHED_)

void tgroup_reset_exit_state_after_copy(struct tgroup *group) {
    group->doing_group_exit = false;
    group->force_detached_count = 0;
    group->reap_deferred = false;
}

static struct tgroup *tgroup_copy(struct tgroup *old_group) {
    struct tgroup *group = malloc(sizeof(struct tgroup));
    *group = *old_group;
    list_init(&group->threads);
    list_add(&old_group->pgroup, &group->pgroup);
    list_add(&old_group->session, &group->session);
    if (group->tty) {
        lock(&group->tty->lock);
        group->tty->refcount++;
        unlock(&group->tty->lock);
    }
    group->itimer = NULL;
    tgroup_reset_exit_state_after_copy(group);
    group->children_rusage = (struct rusage_) {};
    {
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        atomic_store_explicit(&group->last_progress_ns,
            (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec,
            memory_order_relaxed);
    }
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    lock_init(&group->lock);
    return group;
}

static int copy_task(struct task *task, dword_t flags, addr_t stack, addr_t ptid_addr, addr_t tls_addr, addr_t ctid_addr) {
    task->vfork = NULL;
    if (stack != 0)
        CPU_SP(task->cpu) = stack;

    int err;
    struct mm *mm = task->mm;
    if (flags & CLONE_VM_) {
        mm_retain(mm);
    } else {
        task_set_mm(task, mm_copy(mm));
    }

    if (flags & CLONE_FILES_) {
        task->files->refcount++;
    } else {
        task->files = fdtable_copy(task->files);
        if (IS_ERR(task->files)) {
            err = PTR_ERR(task->files);
            goto fail_free_mem;
        }
    }

    err = _ENOMEM;
    if (flags & CLONE_FS_) {
        task->fs->refcount++;
    } else {
        task->fs = fs_info_copy(task->fs);
        if (task->fs == NULL)
            goto fail_free_files;
    }

    if (flags & CLONE_SIGHAND_) {
        task->sighand->refcount++;
    } else {
        task->sighand = sighand_copy(task->sighand);
        if (task->sighand == NULL)
            goto fail_free_fs;
    }

    struct tgroup *old_group = task->group;
    lock(&pids_lock);
    lock(&old_group->lock);
    // Do not publish a child into a group whose teardown snapshot has already
    // started. In particular, a force-detached parent may have entered clone
    // before the snapshot and reached this point afterward.
    if (old_group->doing_group_exit || current->force_detached) {
        unlock(&old_group->lock);
        unlock(&pids_lock);
        err = _EINTR;
        goto fail_free_sighand;
    }
    if (!(flags & CLONE_THREAD_)) {
        task->group = tgroup_copy(old_group);
        task->group->leader = task;
        task->tgid = task->pid;
    }
    list_add(&task->group->threads, &task->group_links);
    unlock(&old_group->lock);
    unlock(&pids_lock);

    if (flags & CLONE_SETTLS_) {
#if defined(GUEST_ARM64)
        // On ARM64, the TLS argument is the actual TLS pointer value (for TPIDR_EL0),
        // not a pointer to a descriptor structure like x86.
        task->cpu.tls_ptr = tls_addr;
#else
        err = task_set_thread_area(task, tls_addr);
        if (err < 0)
            goto fail_free_sighand;
#endif
    }

    err = _EFAULT;
    if (flags & CLONE_CHILD_SETTID_)
        if (user_put_task(task, ctid_addr, task->pid))
            goto fail_free_sighand;
    if (flags & CLONE_PARENT_SETTID_)
        if (user_put(ptid_addr, task->pid))
            goto fail_free_sighand;
    if (flags & CLONE_CHILD_CLEARTID_)
        task->clear_tid = ctid_addr;
    task->exit_signal = flags & CSIGNAL_;

    // remember to do CLONE_SYSVSEM
    return 0;

fail_free_sighand:
    sighand_release(task->sighand);
fail_free_fs:
    fs_info_release(task->fs);
fail_free_files:
    fdtable_release(task->files);
fail_free_mem:
    mm_release(task->mm);
    return err;
}

dword_t sys_clone(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid) {
    STRACE("clone(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", flags, stack, ptid, tls, ctid);
    if (flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS) {
        FIXME("unimplemented clone flags 0x%x", flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS);
        return _EINVAL;
    }
    if (flags & CLONE_SIGHAND_ && !(flags & CLONE_VM_))
        return _EINVAL;
    if (flags & CLONE_THREAD_ && !(flags & CLONE_SIGHAND_))
        return _EINVAL;

    struct task *task = task_create_(current);
    if (task == NULL)
        return _ENOMEM;
    int err = copy_task(task, flags, stack, ptid, tls, ctid);
    if (err < 0) {
        // FIXME: there is a window between task_create_ and task_destroy where
        // some other thread could get a pointer to the task.
        // FIXME: task_destroy doesn't free all aspects of the task, which
        // could cause leaks
        lock(&pids_lock);
        task_destroy(task);
        unlock(&pids_lock);
        return err;
    }
    CPU_RETVAL(task->cpu) = 0;

    struct vfork_info vfork;
    if (flags & CLONE_VFORK_) {
        lock_init(&vfork.lock);
        cond_init(&vfork.cond);
        vfork.done = false;
        task->vfork = &vfork;
    }

    // task might be destroyed by the time we finish, so save the pid
    pid_t pid = task->pid;

    if (current->ptrace.traced) {
        current->ptrace.trap_event = PTRACE_EVENT_FORK_;
        send_signal(current, SIGTRAP_, SIGINFO_NIL);
    }

    task_start(task);

    if (flags & CLONE_VFORK_) {
        lock(&vfork.lock);
        while (!vfork.done) {
            // FIXME this should stop waiting if a fatal signal is received
            int wait_err = wait_for_ignore_signals(&vfork.cond,
                    &vfork.lock, NULL);
            if (wait_err == _EINTR &&
                    atomic_load(&current->exit_state) ==
                            TASK_EXIT_FORCE_DETACHED) {
                // The child normally clears task->vfork before waking us.
                // A forced parent exit must perform the inverse handoff so
                // the child cannot later dereference this stack object.
                unlock(&vfork.lock);
                vfork_abandon(pid, &vfork);
                cond_destroy(&vfork.cond);
                task_finish_force_detached_exit();
            }
        }
        unlock(&vfork.lock);
        cond_destroy(&vfork.cond);
    }

    return pid;
}

dword_t sys_fork() {
    return sys_clone(SIGCHLD_, 0, 0, 0, 0);
}

dword_t sys_vfork() {
    return sys_clone(CLONE_VFORK_ | CLONE_VM_ | SIGCHLD_, 0, 0, 0, 0);
}

void vfork_notify(struct task *task) {
    lock(&task->general_lock);
    struct vfork_info *vfork = task->vfork;
    if (vfork != NULL) {
        lock(&vfork->lock);
        // Complete the task-side handoff before waking the parent. The parent
        // owns `vfork` on its stack and must not touch `task` after it wakes:
        // force-detached cleanup may dispose the task immediately afterward.
        task->vfork = NULL;
        vfork->done = true;
        notify(&vfork->cond);
        unlock(&vfork->lock);
    }
    unlock(&task->general_lock);
}

void vfork_abandon(pid_t child_pid, struct vfork_info *vfork) {
    // Resolve the child again under pids_lock: task_start() transfers its
    // lifetime to the child pthread, so the pointer originally returned by
    // task_create_ is no longer safe for the parent to retain.
    lock(&pids_lock);
    struct task *child = pid_get_task_zombie(child_pid);
    if (child != NULL) {
        lock(&child->general_lock);
        if (child->vfork == vfork)
            child->vfork = NULL;
        unlock(&child->general_lock);
    }
    unlock(&pids_lock);
}
