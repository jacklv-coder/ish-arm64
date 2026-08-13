#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/task.h"
#include "fs/real.h"
#include "fs/sock.h"

struct blocked_write {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool ready;
    bool released;
    atomic_int result;
    struct task *task;
};

struct async_signal {
    struct task *task;
    atomic_bool entered;
    atomic_bool saw_live_sighand;
};

struct blocked_ptrace {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool ready;
    bool released;
    atomic_bool saw_live_task;
    struct task *task;
};

struct task_disposer {
    struct task *task;
    atomic_bool started;
    atomic_bool finished;
};

struct detached_task_finisher {
    struct task *task;
    atomic_bool started;
};

struct vfork_observer {
    struct vfork_info *vfork;
    struct task *address_space_owner;
    struct mm *mm;
    atomic_bool saw_cleared_tid;
    atomic_bool saw_released_mm;
};

static void async_signal_callback(void *opaque) {
    struct async_signal *signal = opaque;
    atomic_store(&signal->entered, true);

    // Keep the callback in flight long enough for force-detach to contend on
    // timer_free(). It must still see live signal state after this delay.
    struct timespec delay = {.tv_nsec = 50 * 1000000L};
    nanosleep(&delay, NULL);
    struct sighand *sighand = signal->task->sighand;
    atomic_store(&signal->saw_live_sighand, sighand != NULL);
    if (sighand != NULL) {
        lock(&sighand->lock);
        unlock(&sighand->lock);
    }
}

static void blocked_write_init(struct blocked_write *write, struct task *task) {
    *write = (struct blocked_write) {.task = task};
    assert(pthread_mutex_init(&write->lock, NULL) == 0);
    assert(pthread_cond_init(&write->cond, NULL) == 0);
    atomic_init(&write->result, -1);
}

static void blocked_write_destroy(struct blocked_write *write) {
    assert(pthread_cond_destroy(&write->cond) == 0);
    assert(pthread_mutex_destroy(&write->lock) == 0);
}

static void blocked_write_wait_until_ready(struct blocked_write *write) {
    pthread_mutex_lock(&write->lock);
    while (!write->ready)
        pthread_cond_wait(&write->cond, &write->lock);
    pthread_mutex_unlock(&write->lock);
}

static void blocked_write_release(struct blocked_write *write) {
    pthread_mutex_lock(&write->lock);
    write->released = true;
    pthread_cond_broadcast(&write->cond);
    pthread_mutex_unlock(&write->lock);
}

static void *blocked_guest_write(void *opaque) {
    struct blocked_write *write = opaque;
    current = write->task;

    pthread_mutex_lock(&write->lock);
    write->ready = true;
    pthread_cond_broadcast(&write->cond);
    while (!write->released)
        pthread_cond_wait(&write->cond, &write->lock);
    pthread_mutex_unlock(&write->lock);

    const char value = 'x';
    atomic_store(&write->result,
            user_write_task(write->task, PAGE_SIZE, &value, 1));
    task_finish_force_detached_exit();
}

static void *normal_guest_exit(void *opaque) {
    current = opaque;
    do_exit(0);
}

static void *finish_detached_guest_exit(void *opaque) {
    current = opaque;
    task_finish_force_detached_exit();
}

static void *finish_detached_guest_exit_observed(void *opaque) {
    struct detached_task_finisher *finisher = opaque;
    current = finisher->task;
    atomic_store(&finisher->started, true);
    task_finish_force_detached_exit();
}

static void *hold_ptrace_reference(void *opaque) {
    struct blocked_ptrace *blocked = opaque;
    lock(&blocked->task->ptrace.lock);
    pthread_mutex_lock(&blocked->lock);
    blocked->ready = true;
    pthread_cond_broadcast(&blocked->cond);
    while (!blocked->released)
        pthread_cond_wait(&blocked->cond, &blocked->lock);
    pthread_mutex_unlock(&blocked->lock);
    atomic_store(&blocked->saw_live_task,
            blocked->task->pid != 0 && blocked->task->ptrace.stopped);
    unlock(&blocked->task->ptrace.lock);
    return NULL;
}

static void *dispose_task(void *opaque) {
    struct task_disposer *disposer = opaque;
    atomic_store(&disposer->started, true);
    lock(&pids_lock);
    task_destroy(disposer->task);
    unlock(&pids_lock);
    atomic_store(&disposer->finished, true);
    return NULL;
}

static void *observe_vfork_completion(void *opaque) {
    struct vfork_observer *observer = opaque;
    lock(&observer->vfork->lock);
    while (!observer->vfork->done)
        wait_for_ignore_signals(&observer->vfork->cond,
                &observer->vfork->lock, NULL);
    unlock(&observer->vfork->lock);

    pid_t_ child_tid = -1;
    atomic_store(&observer->saw_cleared_tid,
            user_read_task(observer->address_space_owner, PAGE_SIZE,
                    &child_tid, sizeof(child_tid)) == 0 && child_tid == 0);
    atomic_store(&observer->saw_released_mm,
            atomic_load(&observer->mm->refcount) == 2);
    return NULL;
}

static struct tgroup *make_group(struct task *leader) {
    struct tgroup *group = calloc(1, sizeof(*group));
    assert(group != NULL);
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = leader;
    leader->group = group;
    leader->tgid = leader->pid;
    list_add(&group->threads, &leader->group_links);
    return group;
}

static struct mm *make_mapped_mm(void) {
    struct mm *mm = mm_new();
    assert(mm != NULL);
    write_wrlock(&mm->mem.lock);
    assert(pt_map_nothing(&mm->mem, 1, 1, P_READ | P_WRITE) == 0);
    write_wrunlock(&mm->mem.lock);
    return mm;
}

static void test_nonleader_resources_follow_host_pthread(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct mm *mm = make_mapped_mm();
    task_set_mm(leader, mm);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->sighand = sighand_new();
    assert(task->sighand != NULL);
    task->files = fdtable_new(1);
    assert(!IS_ERR(task->files));
    int shutdown_pipe[2];
    assert(pipe(shutdown_pipe) == 0);
    struct fd *shutdown_writer = fd_create(&realfs_fdops);
    assert(shutdown_writer != NULL);
    shutdown_writer->real_fd = shutdown_pipe[1];
    task->files->files[0] = shutdown_writer;
    task->fs = fs_info_new();
    assert(task->fs != NULL);
    task->fs->root = fd_create(&realfs_fdops);
    task->fs->pwd = fd_create(&realfs_fdops);
    assert(task->fs->root != NULL && task->fs->pwd != NULL);
    task->fs->root->real_fd = dup(STDIN_FILENO);
    task->fs->pwd->real_fd = dup(STDIN_FILENO);
    assert(task->fs->root->real_fd >= 0 && task->fs->pwd->real_fd >= 0);
    assert(pipe(task->futex_pipe) == 0);
    task->group = group;
    task->tgid = leader->tgid;
    mm_retain(mm);
    task_set_mm(task, mm);
    list_add(&group->threads, &task->group_links);

    // Observer ownership lets the test verify the exact cleanup boundary.
    mm_retain(mm);
    assert(atomic_load(&mm->refcount) == 3);

    pid_t_ child_tid = 42;
    assert(user_write_task(task, PAGE_SIZE, &child_tid, sizeof(child_tid)) == 0);
    task->clear_tid = PAGE_SIZE;

    struct vfork_info vfork = {};
    lock_init(&vfork.lock);
    cond_init(&vfork.cond);
    task->vfork = &vfork;

    struct vfork_observer vfork_observer = {
        .vfork = &vfork,
        .address_space_owner = leader,
        .mm = mm,
    };
    atomic_init(&vfork_observer.saw_cleared_tid, false);
    atomic_init(&vfork_observer.saw_released_mm, false);
    pthread_t vfork_waiter;
    assert(pthread_create(&vfork_waiter, NULL, observe_vfork_completion,
            &vfork_observer) == 0);

    struct sigqueue *pending = calloc(1, sizeof(*pending));
    assert(pending != NULL);
    pending->info.sig = SIGKILL_;
    list_add(&task->queue, &pending->queue);

    struct async_signal signal = {.task = task};
    atomic_init(&signal.entered, false);
    atomic_init(&signal.saw_live_sighand, false);
    group->itimer = timer_new(CLOCK_MONOTONIC, async_signal_callback, &signal);
    assert(!IS_ERR(group->itimer));
    struct timer_spec timer_spec = {
        .value = {.tv_nsec = 1000000L},
    };
    assert(timer_set(group->itimer, timer_spec, NULL) == 0);
    while (!atomic_load(&signal.entered)) {
        struct timespec delay = {.tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
    }

    struct blocked_write write;
    blocked_write_init(&write, task);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, blocked_guest_write, &write) == 0);
    blocked_write_wait_until_ready(&write);

    current = leader;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    assert(task->mem == &mm->mem);
    assert(task->mm == mm);
    // A private descriptor table can publish pipe EOF immediately without
    // freeing fd/fs object storage that the blocked host syscall may borrow.
    assert(task->files != NULL);
    assert(task->fs != NULL);
    char eof;
    assert(read(shutdown_pipe[0], &eof, sizeof(eof)) == 0);
    close(shutdown_pipe[0]);
    // The per-thread wake pipe remains live until futex_wait removes its stack
    // waiter, preventing a concurrent wake from writing to a reused host fd.
    assert(task->futex_pipe[0] != -1);
    assert(task->futex_pipe[1] != -1);
    assert(group->force_detached_count == 1);
    unlock(&group->lock);
    unlock(&pids_lock);

    blocked_write_release(&write);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_join(vfork_waiter, NULL) == 0);
    assert(atomic_load(&write.result) == 0);
    assert(atomic_load(&signal.saw_live_sighand));
    assert(group->itimer == NULL);
    assert(vfork.done);
    assert(task->vfork == NULL);
    assert(atomic_load(&vfork_observer.saw_cleared_tid));
    assert(atomic_load(&vfork_observer.saw_released_mm));
    assert(atomic_load(&mm->refcount) == 2);
    assert(group->force_detached_count == 0);
    pid_t_ cleared_tid = -1;
    assert(user_read_task(leader, PAGE_SIZE, &cleared_tid,
            sizeof(cleared_tid)) == 0);
    assert(cleared_tid == 0);
    cond_destroy(&vfork.cond);
    blocked_write_destroy(&write);

    list_remove(&leader->group_links);
    mm_release(leader->mm);
    leader->mm = NULL;
    leader->mem = NULL;
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    mm_release(mm);
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detached_leader_stays_hidden_until_zombie(void) {
    struct task *parent = task_create_(NULL);
    assert(parent != NULL);
    struct tgroup *parent_group = make_group(parent);

    struct task *leader = task_create_(parent);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);
    struct mm *mm = make_mapped_mm();
    task_set_mm(leader, mm);
    mm_retain(mm);

    struct blocked_write write;
    blocked_write_init(&write, leader);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, blocked_guest_write, &write) == 0);
    blocked_write_wait_until_ready(&write);

    current = parent;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(leader));
    unlock(&group->lock);
    unlock(&pids_lock);

    blocked_write_release(&write);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load(&write.result) == 0);
    assert(!leader->zombie);
    assert(atomic_load(&leader->force_detached));
    assert(leader->mm == NULL);
    assert(leader->mem == NULL);
    assert(pid_get_task_zombie(leader->pid) == leader);
    assert(group->force_detached_count == 0);
    blocked_write_destroy(&write);

    pid_t_ leader_pid = leader->pid;
    leader->zombie = true;
    assert((pid_t_) sys_wait4(leader_pid, 0, 0, 0) == leader_pid);
    assert(pid_get_task_zombie(leader_pid) == NULL);
    mm_release(mm);

    list_remove(&parent->group_links);
    lock(&pids_lock);
    task_destroy(parent);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&parent_group->child_exit);
    cond_destroy(&parent_group->stopped_cond);
    free(parent_group);
}

static void test_reap_waits_for_force_detached_leader(void) {
    struct task *parent = task_create_(NULL);
    assert(parent != NULL);
    struct tgroup *parent_group = make_group(parent);

    struct task *leader = task_create_(parent);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);
    struct mm *mm = make_mapped_mm();
    task_set_mm(leader, mm);

    struct task *child = task_create_(NULL);
    assert(child != NULL);
    child->parent = leader;
    list_add(&leader->children, &child->siblings);

    // Observer ownership proves that the returning host pthread released its
    // address-space reference after the parent consumed the visible zombie.
    mm_retain(mm);
    assert(atomic_load(&mm->refcount) == 2);

    struct blocked_write write;
    blocked_write_init(&write, leader);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, blocked_guest_write, &write) == 0);
    blocked_write_wait_until_ready(&write);

    current = parent;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(leader));
    leader->zombie = true;
    assert(child->parent == NULL);
    assert(list_empty(&leader->children));
    unlock(&group->lock);
    unlock(&pids_lock);

    // Model sys_clone publishing a child after the detach snapshot but before
    // the detached host thread reaches its final cleanup boundary.
    struct task *late_child = task_create_(NULL);
    assert(late_child != NULL);
    lock(&pids_lock);
    late_child->parent = leader;
    list_add(&leader->children, &late_child->siblings);
    late_child->group = group;
    late_child->tgid = leader->tgid;
    list_add(&group->threads, &late_child->group_links);
    unlock(&pids_lock);

    pid_t_ leader_pid = leader->pid;
    assert((pid_t_) sys_wait4(leader_pid, 0, 0, 0) == leader_pid);
    assert(group->reap_deferred);
    assert(group->force_detached_count == 1);
    assert(pid_get_task_zombie(leader_pid) == NULL);

    blocked_write_release(&write);
    assert(pthread_join(worker, NULL) == 0);
    assert(atomic_load(&write.result) == 0);
    assert(atomic_load(&mm->refcount) == 1);
    assert(late_child->parent == NULL);
    assert(list_empty(&leader->children));
    assert(group->reap_deferred);
    assert(group->force_detached_count == 0);
    assert(!list_empty(&group->threads));
    blocked_write_destroy(&write);
    mm_release(mm);

    // The late CLONE_THREAD child is now the final owner of the retained group.
    // Its ordinary exit must dispose both itself and the already-reaped leader.
    pid_t_ late_child_pid = late_child->pid;
    pthread_t late_worker;
    assert(pthread_create(&late_worker, NULL, normal_guest_exit, late_child) == 0);
    assert(pthread_join(late_worker, NULL) == 0);
    assert(pid_get_task_zombie(late_child_pid) == NULL);
    assert(pid_get_task_zombie(leader_pid) == NULL);

    lock(&pids_lock);
    task_destroy(child);
    unlock(&pids_lock);
    list_remove(&parent->group_links);
    lock(&pids_lock);
    task_destroy(parent);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&parent_group->child_exit);
    cond_destroy(&parent_group->stopped_cond);
    free(parent_group);
}

static void test_force_detach_during_normal_exit_handoff(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct mm *mm = make_mapped_mm();
    task_set_mm(leader, mm);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->group = group;
    task->tgid = leader->tgid;
    mm_retain(mm);
    task_set_mm(task, mm);
    list_add(&group->threads, &task->group_links);

    // Keep do_exit blocked after it claims normal cleanup ownership. The group
    // exit safety valve must not steal cleanup ownership or touch resources
    // that do_exit_claimed may already be releasing.
    current = leader;
    lock(&pids_lock);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, normal_guest_exit, task) == 0);

    while (atomic_load(&task->exit_state) != TASK_EXIT_NORMAL) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }

    lock(&group->lock);
    assert(!task_force_detach_for_group_exit_locked(task));
    assert(group->force_detached_count == 0);
    assert(!atomic_load(&task->force_detached));
    unlock(&group->lock);
    unlock(&pids_lock);

    assert(pthread_join(worker, NULL) == 0);
    assert(group->force_detached_count == 0);

    list_remove(&leader->group_links);
    mm_release(leader->mm);
    leader->mm = NULL;
    leader->mem = NULL;
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detach_preserves_externally_shared_fdtable(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->group = group;
    task->tgid = leader->tgid;
    list_add(&group->threads, &task->group_links);

    task->files = fdtable_new(1);
    assert(!IS_ERR(task->files));
    int shared_pipe[2];
    assert(pipe(shared_pipe) == 0);
    struct fd *writer = fd_create(&realfs_fdops);
    assert(writer != NULL);
    writer->real_fd = shared_pipe[1];
    task->files->files[0] = writer;

    // Model CLONE_FILES without CLONE_THREAD: another live process group owns
    // the same table and must continue using it after this group detaches.
    struct task *external = task_create_(NULL);
    assert(external != NULL);
    struct tgroup *external_group = make_group(external);
    external->files = task->files;
    external->files->refcount++;
    struct fdtable *shared_files = task->files;

    current = leader;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    unlock(&group->lock);
    unlock(&pids_lock);

    const char value = 'x';
    assert(write(atomic_load(&writer->real_fd), &value, 1) == 1);
    char received = 0;
    assert(read(shared_pipe[0], &received, 1) == 1 && received == value);
    close(shared_pipe[0]);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, finish_detached_guest_exit, task) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(group->force_detached_count == 0);

    external->files = NULL;
    fdtable_release(shared_files);
    list_remove(&external->group_links);
    lock(&pids_lock);
    task_destroy(external);
    unlock(&pids_lock);
    cond_destroy(&external_group->child_exit);
    cond_destroy(&external_group->stopped_cond);
    free(external_group);

    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detach_preserves_fd_shared_by_copied_table(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->group = group;
    task->tgid = leader->tgid;
    list_add(&group->threads, &task->group_links);
    task->files = fdtable_new(1);
    assert(!IS_ERR(task->files));

    int shared_pipe[2];
    assert(pipe(shared_pipe) == 0);
    struct fd *writer = fd_create(&realfs_fdops);
    assert(writer != NULL);
    writer->real_fd = shared_pipe[1];
    task->files->files[0] = writer;

    struct task *external = task_create_(NULL);
    assert(external != NULL);
    struct tgroup *external_group = make_group(external);
    external->files = fdtable_copy(task->files);
    assert(!IS_ERR(external->files));
    assert(atomic_load(&writer->refcount) == 2);

    current = leader;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    unlock(&group->lock);
    unlock(&pids_lock);

    const char value = 'y';
    assert(write(atomic_load(&writer->real_fd), &value, 1) == 1);
    char received = 0;
    assert(read(shared_pipe[0], &received, 1) == 1 && received == value);
    // Releasing the copied table removes the last live reference. The host
    // handle must close immediately even though the deferred task has not yet
    // returned to release its original table.
    fdtable_release(external->files);
    external->files = NULL;
    assert(atomic_load(&writer->real_fd) == -1);
    assert(read(shared_pipe[0], &received, 1) == 0);
    close(shared_pipe[0]);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, finish_detached_guest_exit, task) == 0);
    assert(pthread_join(worker, NULL) == 0);

    list_remove(&external->group_links);
    lock(&pids_lock);
    task_destroy(external);
    unlock(&pids_lock);
    cond_destroy(&external_group->child_exit);
    cond_destroy(&external_group->stopped_cond);
    free(external_group);

    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detach_shuts_down_private_socket(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->group = group;
    task->tgid = leader->tgid;
    list_add(&group->threads, &task->group_links);
    task->files = fdtable_new(1);
    assert(!IS_ERR(task->files));

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct fd *socket_fd = fd_create(&socket_fdops);
    assert(socket_fd != NULL);
    socket_fd->real_fd = sockets[0];
    socket_fd->socket.domain = AF_INET_;
    task->files->files[0] = socket_fd;

    current = leader;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    assert(atomic_load(&socket_fd->real_fd) == -1);
    unlock(&group->lock);
    unlock(&pids_lock);

    char byte;
    assert(read(sockets[1], &byte, sizeof(byte)) == 0);
    close(sockets[1]);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, finish_detached_guest_exit, task) == 0);
    assert(pthread_join(worker, NULL) == 0);

    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detach_treats_late_installed_fd_as_live(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->group = group;
    task->tgid = leader->tgid;
    list_add(&group->threads, &task->group_links);
    task->files = fdtable_new(2);
    assert(!IS_ERR(task->files));

    int first_pipe[2];
    int late_pipe[2];
    assert(pipe(first_pipe) == 0);
    assert(pipe(late_pipe) == 0);
    struct fd *first = fd_create(&realfs_fdops);
    struct fd *late = fd_create(&realfs_fdops);
    assert(first != NULL && late != NULL);
    first->real_fd = first_pipe[1];
    late->real_fd = late_pipe[1];
    task->files->files[0] = first;

    current = leader;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    assert(atomic_load(&first->real_fd) == -1);
    // Model accept/open/SCM_RIGHTS finishing after the shutdown snapshot.
    task->files->files[1] = late;
    assert(!bit_test(1, task->files->force_shutdown));
    unlock(&group->lock);
    unlock(&pids_lock);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, finish_detached_guest_exit, task) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(read(first_pipe[0], &(char) {0}, 1) == 0);
    assert(read(late_pipe[0], &(char) {0}, 1) == 0);
    close(first_pipe[0]);
    close(late_pipe[0]);

    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detach_consumes_marker_on_inflight_close(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->group = group;
    task->tgid = leader->tgid;
    list_add(&group->threads, &task->group_links);
    task->files = fdtable_new(1);
    assert(!IS_ERR(task->files));

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct fd *socket_fd = fd_create(&socket_fdops);
    assert(socket_fd != NULL);
    socket_fd->real_fd = sockets[0];
    socket_fd->socket.domain = AF_INET_;
    task->files->files[0] = socket_fd;

    current = task;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    assert(bit_test(0, task->files->force_shutdown));
    unlock(&group->lock);
    unlock(&pids_lock);

    // Model an in-flight syscall completing a guest close after detachment.
    assert(f_close(0) == 0);
    assert(task->files->files[0] == NULL);
    assert(!bit_test(0, task->files->force_shutdown));
    assert(read(sockets[1], &(char) {0}, 1) == 0);
    close(sockets[1]);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, finish_detached_guest_exit, task) == 0);
    assert(pthread_join(worker, NULL) == 0);

    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detach_shuts_down_duplicated_private_socket(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->group = group;
    task->tgid = leader->tgid;
    list_add(&group->threads, &task->group_links);
    task->files = fdtable_new(3);
    assert(!IS_ERR(task->files));

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct fd *socket_fd = fd_create(&socket_fdops);
    assert(socket_fd != NULL);
    socket_fd->real_fd = sockets[0];
    socket_fd->socket.domain = AF_INET_;
    task->files->files[0] = socket_fd;
    task->files->files[1] = fd_retain(socket_fd);
    task->files->files[2] = fd_retain(socket_fd);
    assert(atomic_load(&socket_fd->refcount) == 3);

    current = leader;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    assert(atomic_load(&socket_fd->real_fd) == -1);
    unlock(&group->lock);
    unlock(&pids_lock);

    char byte;
    assert(read(sockets[1], &byte, sizeof(byte)) == 0);
    close(sockets[1]);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, finish_detached_guest_exit, task) == 0);
    assert(pthread_join(worker, NULL) == 0);

    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detach_preserves_same_group_shared_fdtable(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);
    leader->files = fdtable_new(3);
    assert(!IS_ERR(leader->files));

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct fd *socket_fd = fd_create(&socket_fdops);
    assert(socket_fd != NULL);
    socket_fd->real_fd = sockets[0];
    socket_fd->socket.domain = AF_INET_;
    leader->files->files[0] = socket_fd;
    leader->files->files[1] = fd_retain(socket_fd);
    leader->files->files[2] = fd_retain(socket_fd);

    struct task *task = task_create_(leader);
    assert(task != NULL);
    task->group = group;
    task->tgid = leader->tgid;
    task->files = leader->files;
    task->files->refcount++;
    list_add(&group->threads, &task->group_links);

    current = leader;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    assert(atomic_load(&socket_fd->real_fd) == sockets[0]);
    unlock(&group->lock);
    unlock(&pids_lock);

    const char value = 'z';
    assert(write(atomic_load(&socket_fd->real_fd), &value, 1) == 1);
    char received = 0;
    assert(read(sockets[1], &received, 1) == 1 && received == value);

    // Once the last runnable owner releases the shared table, only the
    // deferred reference remains. That transition must close the host socket
    // so the blocked detached task can return and finish cleanup.
    fdtable_release(leader->files);
    leader->files = NULL;
    assert(atomic_load(&socket_fd->real_fd) == -1);
    char byte;
    assert(read(sockets[1], &byte, sizeof(byte)) == 0);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, finish_detached_guest_exit, task) == 0);
    assert(pthread_join(worker, NULL) == 0);

    close(sockets[1]);
    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_force_detach_closes_table_after_every_owner_detaches(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);

    struct task *first = task_create_(leader);
    struct task *second = task_create_(leader);
    assert(first != NULL && second != NULL);
    first->group = second->group = group;
    first->tgid = second->tgid = leader->tgid;
    list_add(&group->threads, &first->group_links);
    list_add(&group->threads, &second->group_links);

    first->files = fdtable_new(3);
    assert(!IS_ERR(first->files));
    second->files = first->files;
    second->files->refcount++;

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct fd *socket_fd = fd_create(&socket_fdops);
    assert(socket_fd != NULL);
    socket_fd->real_fd = sockets[0];
    socket_fd->socket.domain = AF_INET_;
    first->files->files[0] = socket_fd;
    first->files->files[1] = fd_retain(socket_fd);
    first->files->files[2] = fd_retain(socket_fd);

    current = leader;
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(first));
    assert(atomic_load(&socket_fd->real_fd) == sockets[0]);
    assert(task_force_detach_for_group_exit_locked(second));
    assert(atomic_load(&socket_fd->real_fd) == -1);
    assert(first->files->force_shutdown_complete);
    unlock(&group->lock);
    unlock(&pids_lock);

    char byte;
    assert(read(sockets[1], &byte, sizeof(byte)) == 0);
    close(sockets[1]);

    pthread_t first_worker;
    pthread_t second_worker;
    assert(pthread_create(&first_worker, NULL,
            finish_detached_guest_exit, first) == 0);
    assert(pthread_create(&second_worker, NULL,
            finish_detached_guest_exit, second) == 0);
    assert(pthread_join(first_worker, NULL) == 0);
    assert(pthread_join(second_worker, NULL) == 0);

    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_copied_group_drops_exit_only_state(void) {
    struct tgroup group = {
        .doing_group_exit = true,
        .force_detached_count = 7,
        .reap_deferred = true,
    };
    tgroup_reset_exit_state_after_copy(&group);
    assert(!group.doing_group_exit);
    assert(group.force_detached_count == 0);
    assert(!group.reap_deferred);
}

static void test_group_exit_rejects_replacement_itimer(void) {
    struct task *leader = task_create_(NULL);
    assert(leader != NULL);
    struct tgroup *group = make_group(leader);
    current = leader;

    // The force-detach path deliberately drops group->lock while waiting for
    // an old timer callback. Any thread that reaches alarm/setitimer during
    // that interval must not publish a replacement timer retaining a raw task
    // pointer.
    group->doing_group_exit = true;
    assert((int_t) sys_alarm(1) == _EINTR);
    assert(group->itimer == NULL);

    list_remove(&leader->group_links);
    lock(&pids_lock);
    task_destroy(leader);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

static void test_dispose_waits_for_pinned_ptrace_user(void) {
    struct task *task = task_create_(NULL);
    assert(task != NULL);
    task->ptrace.stopped = true;

    struct blocked_ptrace blocked = {.task = task};
    assert(pthread_mutex_init(&blocked.lock, NULL) == 0);
    assert(pthread_cond_init(&blocked.cond, NULL) == 0);
    atomic_init(&blocked.saw_live_task, false);

    pthread_t observer;
    assert(pthread_create(&observer, NULL, hold_ptrace_reference, &blocked) == 0);
    pthread_mutex_lock(&blocked.lock);
    while (!blocked.ready)
        pthread_cond_wait(&blocked.cond, &blocked.lock);
    pthread_mutex_unlock(&blocked.lock);

    struct task_disposer disposer = {.task = task};
    atomic_init(&disposer.started, false);
    atomic_init(&disposer.finished, false);
    pthread_t cleanup;
    assert(pthread_create(&cleanup, NULL, dispose_task, &disposer) == 0);
    while (!atomic_load(&disposer.started)) {
        struct timespec delay = {.tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
    }
    struct timespec delay = {.tv_nsec = 10 * 1000000L};
    nanosleep(&delay, NULL);
    assert(!atomic_load(&disposer.finished));

    pthread_mutex_lock(&blocked.lock);
    blocked.released = true;
    pthread_cond_broadcast(&blocked.cond);
    pthread_mutex_unlock(&blocked.lock);
    assert(pthread_join(observer, NULL) == 0);
    assert(pthread_join(cleanup, NULL) == 0);
    assert(atomic_load(&blocked.saw_live_task));
    assert(atomic_load(&disposer.finished));
    assert(pthread_cond_destroy(&blocked.cond) == 0);
    assert(pthread_mutex_destroy(&blocked.lock) == 0);
}

static void test_force_detached_cleanup_uses_global_lock_order(void) {
    struct task *task = task_create_(NULL);
    assert(task != NULL);
    struct tgroup *group = make_group(task);
    task_set_mm(task, make_mapped_mm());

    // Hold pids_lock while the cleanup thread starts. Correct cleanup must
    // block on this lock before touching ptrace.lock. The old inverse order
    // acquired ptrace.lock first and then waited for pids_lock, forming an
    // ABBA deadlock with wait4/ptrace/task disposal.
    lock(&pids_lock);
    lock(&group->lock);
    assert(task_force_detach_for_group_exit_locked(task));
    unlock(&group->lock);

    struct detached_task_finisher finisher = {.task = task};
    atomic_init(&finisher.started, false);
    pthread_t cleanup;
    assert(pthread_create(&cleanup, NULL,
            finish_detached_guest_exit_observed, &finisher) == 0);
    while (!atomic_load(&finisher.started)) {
        struct timespec delay = {.tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
    }
    struct timespec settle = {.tv_nsec = 10 * 1000000L};
    nanosleep(&settle, NULL);
    assert(trylock(&task->ptrace.lock) == 0);
    unlock(&task->ptrace.lock);
    unlock(&pids_lock);

    assert(pthread_join(cleanup, NULL) == 0);
    assert(group->force_detached_count == 0);
    assert(task->mm == NULL);
    assert(task->mem == NULL);

    lock(&pids_lock);
    task_destroy(task);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group->child_exit);
    cond_destroy(&group->stopped_cond);
    free(group);
}

int main(void) {
    test_nonleader_resources_follow_host_pthread();
    test_force_detached_leader_stays_hidden_until_zombie();
    test_reap_waits_for_force_detached_leader();
    test_force_detach_during_normal_exit_handoff();
    test_force_detach_preserves_externally_shared_fdtable();
    test_force_detach_preserves_fd_shared_by_copied_table();
    test_force_detach_shuts_down_private_socket();
    test_force_detach_treats_late_installed_fd_as_live();
    test_force_detach_consumes_marker_on_inflight_close();
    test_force_detach_shuts_down_duplicated_private_socket();
    test_force_detach_preserves_same_group_shared_fdtable();
    test_force_detach_closes_table_after_every_owner_detaches();
    test_copied_group_drops_exit_only_state();
    test_group_exit_rejects_replacement_itimer();
    test_dispose_waits_for_pinned_ptrace_user();
    test_force_detached_cleanup_uses_global_lock_order();
    return 0;
}
