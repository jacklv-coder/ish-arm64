#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/real.h"
#include "fs/sock.h"

// Weak so the lifetime test can exercise the low-memory safety-valve path.
__attribute__((weak)) void *fdtable_force_detach_snapshot_alloc(size_t size) {
    return malloc(size);
}

struct fd *fd_create(const struct fd_ops *ops) {
    struct fd *fd = malloc(sizeof(struct fd));
    if (fd == NULL)
        return NULL;
    *fd = (struct fd) {};
    fd->ops = ops;
    fd->refcount = 1;
    fd->force_shutdown_refs = 0;
    lock_init(&fd->refcount_lock);
    fd->flags = 0;
    fd->mount = NULL;
    fd->offset = 0;
    list_init(&fd->poll_fds);
    lock_init(&fd->poll_lock);
    lock_init(&fd->lock);
    cond_init(&fd->cond);
    return fd;
}

struct fd *fd_retain(struct fd *fd) {
    lock(&fd->refcount_lock);
    assert(atomic_load(&fd->refcount) > 0);
    fd->refcount++;
    unlock(&fd->refcount_lock);
    return fd;
}

static bool fd_supports_force_shutdown(struct fd *fd) {
    return fd->ops != NULL &&
        (fd->ops->close == realfs_close || fd->ops == &socket_fdops);
}

static void fd_mark_force_shutdown(struct fd *fd, unsigned references) {
    lock(&fd->refcount_lock);
    fd->force_shutdown_refs += references;
    assert(fd->force_shutdown_refs <= atomic_load(&fd->refcount));
    bool should_shutdown = fd_supports_force_shutdown(fd) &&
        fd->force_shutdown_refs == atomic_load(&fd->refcount);
    int real_fd = should_shutdown ? atomic_exchange(&fd->real_fd, -1) : -1;
    unlock(&fd->refcount_lock);
    if (real_fd >= 0)
        close(real_fd);
}

static int fd_close_internal(struct fd *fd, bool force_shutdown_reference) {
    int err = 0;
    bool deferred = force_shutdown_reference &&
        fd_supports_force_shutdown(fd);
    lock(&fd->refcount_lock);
    if (deferred) {
        assert(fd->force_shutdown_refs > 0);
        fd->force_shutdown_refs--;
    }
    unsigned references = atomic_fetch_sub(&fd->refcount, 1) - 1;
    assert(references != 0 || fd->force_shutdown_refs == 0);
    bool should_shutdown = references != 0 &&
        fd->force_shutdown_refs == references &&
        fd_supports_force_shutdown(fd);
    int real_fd = should_shutdown ? atomic_exchange(&fd->real_fd, -1) : -1;
    unlock(&fd->refcount_lock);

    if (real_fd >= 0)
        close(real_fd);
    if (references == 0) {
        poll_cleanup_fd(fd);
        if (fd->ops->close)
            err = fd->ops->close(fd);
        // see comment in close in kernel/fs.h
        if (fd->mount && fd->mount->fs->close && fd->mount->fs->close != fd->ops->close) {
            int new_err = fd->mount->fs->close(fd);
            if (new_err < 0)
                err = new_err;
        }

        if (fd->inode)
            inode_release(fd->inode);
        if (fd->mount)
            mount_release(fd->mount);
        free(fd);
    }
    return err;
}

int fd_close(struct fd *fd) {
    return fd_close_internal(fd, false);
}

static int fdtable_resize(struct fdtable *table, unsigned size);

struct fdtable *fdtable_new(int size) {
    struct fdtable *fdt = malloc(sizeof(struct fdtable));
    if (fdt == NULL)
        return ERR_PTR(_ENOMEM);
    fdt->refcount = 1;
    fdt->force_detached_refs = 0;
    fdt->unsnapshotted_force_detached_refs = 0;
    fdt->force_shutdown_complete = false;
    fdt->size = 0;
    fdt->files = NULL;
    fdt->cloexec = NULL;
    fdt->force_shutdown = NULL;
    lock_init(&fdt->lock);
    int err = fdtable_resize(fdt, size);
    if (err < 0) {
        free(fdt);
        return ERR_PTR(err);
    }
    return fdt;
}

static int fdtable_close(struct fdtable *table, fd_t f);

static int fd_pointer_compare(const void *left, const void *right) {
    uintptr_t left_value = (uintptr_t) *(struct fd *const *) left;
    uintptr_t right_value = (uintptr_t) *(struct fd *const *) right;
    return (left_value > right_value) - (left_value < right_value);
}

static void fdtable_shutdown_exclusive_without_storage_locked(
        struct fdtable *table) {
    for (fd_t f = 0; (unsigned) f < table->size; f++) {
        struct fd *fd = table->files[f];
        if (fd == NULL || !fd_supports_force_shutdown(fd))
            continue;

        bool already_counted = false;
        for (fd_t earlier = 0; earlier < f; earlier++) {
            if (table->files[earlier] == fd) {
                already_counted = true;
                break;
            }
        }
        if (already_counted)
            continue;

        unsigned table_refs = 1;
        for (fd_t later = f + 1; (unsigned) later < table->size; later++) {
            if (table->files[later] == fd)
                table_refs++;
        }
        fd_mark_force_shutdown(fd, table_refs);
        for (fd_t slot = f; (unsigned) slot < table->size; slot++) {
            if (table->files[slot] == fd)
                bit_set(slot, table->force_shutdown);
        }
    }
}

static void fdtable_shutdown_exclusive_locked(struct fdtable *table) {
    if (table->force_shutdown_complete)
        return;

    struct fd **owned = malloc(sizeof(*owned) * table->size);
    if (owned == NULL && table->size != 0) {
        // Emergency teardown must still wake blocked host calls under memory
        // pressure. This allocation-free fallback is slower, but it is used
        // only when the linear-storage fast path cannot be allocated.
        fdtable_shutdown_exclusive_without_storage_locked(table);
        table->force_shutdown_complete = true;
        return;
    }

    unsigned owned_count = 0;
    for (fd_t f = 0; (unsigned) f < table->size; f++) {
        struct fd *fd = table->files[f];
        if (fd == NULL || !fd_supports_force_shutdown(fd))
            continue;
        owned[owned_count++] = fd;
        bit_set(f, table->force_shutdown);
    }

    // Count aliases in one sorted pass. A table can contain thousands of
    // duplicate descriptors, so rescanning the table per entry would make
    // emergency teardown quadratic while holding its lock.
    qsort(owned, owned_count, sizeof(*owned), fd_pointer_compare);
    for (unsigned first = 0; first < owned_count;) {
        struct fd *fd = owned[first];
        unsigned after = first + 1;
        while (after < owned_count && owned[after] == fd)
            after++;

        // External refs (mmap, poll, or a copied descriptor table) keep the
        // host handle open. Their final release closes it when only deferred
        // aliases remain.
        fd_mark_force_shutdown(fd, after - first);
        first = after;
    }
    free(owned);
    table->force_shutdown_complete = true;
}

static void fdtable_release_force_detach_snapshot(
        struct fdtable_force_detach *snapshot) {
    if (snapshot == NULL)
        return;
    for (unsigned index = 0; index < snapshot->count; index++)
        fd_close_internal(snapshot->files[index], true);
    free(snapshot);
}

struct fdtable_force_detach *fdtable_prepare_force_detach_locked(
        struct fdtable *table) {
    size_t files_size;
    size_t allocation_size;
    if (__builtin_mul_overflow((size_t) table->size,
            sizeof(struct fd *), &files_size) ||
            __builtin_add_overflow(sizeof(struct fdtable_force_detach),
                files_size, &allocation_size)) {
        return NULL;
    }
    struct fdtable_force_detach *snapshot =
        fdtable_force_detach_snapshot_alloc(allocation_size);
    if (snapshot == NULL)
        return NULL;
    snapshot->count = 0;
    snapshot->capacity = table->size;
    return snapshot;
}

void fdtable_commit_force_detach_locked(struct fdtable *table,
        struct fdtable_force_detach *snapshot) {
    if (snapshot == NULL) {
        // Allocation failure must not disable group-exit's safety valve. Keep
        // the table slots stable until this host pthread leaves its in-flight
        // syscall; close/dup replacement returns EBUSY in the meantime.
        table->unsnapshotted_force_detached_refs++;
    } else {
        assert(table->size <= snapshot->capacity);
        for (fd_t slot = 0; (unsigned) slot < table->size; slot++) {
            struct fd *fd = table->files[slot];
            if (fd == NULL)
                continue;
            snapshot->files[snapshot->count++] = fd_retain(fd);
            // The retained reference belongs only to a task that teardown is
            // trying to wake. If runnable/copy-table owners later close their
            // references, the host handle closes while this reference keeps
            // the struct fd alive until the syscall returns.
            if (fd_supports_force_shutdown(fd))
                fd_mark_force_shutdown(fd, 1);
        }
    }
    assert(table->force_detached_refs < atomic_load(&table->refcount));
    table->force_detached_refs++;
    if (table->force_detached_refs == atomic_load(&table->refcount))
        fdtable_shutdown_exclusive_locked(table);
}

// FIXME this looks like it has the classic refcount UAF
void fdtable_release(struct fdtable *table) {
    lock(&table->lock);
    if (--table->refcount == 0) {
        assert(table->force_detached_refs == 0);
        for (fd_t f = 0; (unsigned) f < table->size; f++)
            fdtable_close(table, f);
        free(table->files);
        free(table->cloexec);
        free(table->force_shutdown);
        unlock(&table->lock);
        free(table);
    } else {
        // The last runnable owner just exited. Closing host handles now wakes
        // every force-detached owner still blocked in a host syscall.
        if (table->force_detached_refs == atomic_load(&table->refcount))
            fdtable_shutdown_exclusive_locked(table);
        unlock(&table->lock);
    }
}

void fdtable_release_force_detached(struct fdtable *table,
        struct fdtable_force_detach *snapshot) {
    // The host pthread has returned from every fd operation. Drop its borrowed
    // descriptors before releasing its ownership of the table itself.
    fdtable_release_force_detach_snapshot(snapshot);
    lock(&table->lock);
    if (snapshot == NULL) {
        assert(table->unsnapshotted_force_detached_refs > 0);
        table->unsnapshotted_force_detached_refs--;
    }
    assert(table->force_detached_refs > 0);
    table->force_detached_refs--;
    if (--table->refcount == 0) {
        assert(table->force_detached_refs == 0);
        for (fd_t f = 0; (unsigned) f < table->size; f++)
            fdtable_close(table, f);
        free(table->files);
        free(table->cloexec);
        free(table->force_shutdown);
        unlock(&table->lock);
        free(table);
    } else {
        if (table->force_detached_refs == atomic_load(&table->refcount))
            fdtable_shutdown_exclusive_locked(table);
        unlock(&table->lock);
    }
}

static int fdtable_resize(struct fdtable *table, unsigned size) {
    // currently the only legitimate use of this is to expand the table
    assert(size > table->size);

    struct fd **files = malloc(sizeof(struct fd *) * size);
    if (files == NULL)
        return _ENOMEM;
    memset(files, 0, sizeof(struct fd *) * size);
    if (table->files)
        memcpy(files, table->files, sizeof(struct fd *) * table->size);

    bits_t *cloexec = malloc(BITS_SIZE(size));
    if (cloexec == NULL) {
        free(files);
        return _ENOMEM;
    }
    memset(cloexec, 0, BITS_SIZE(size));
    if (table->cloexec)
        memcpy(cloexec, table->cloexec, BITS_SIZE(table->size));

    bits_t *force_shutdown = malloc(BITS_SIZE(size));
    if (force_shutdown == NULL) {
        free(files);
        free(cloexec);
        return _ENOMEM;
    }
    memset(force_shutdown, 0, BITS_SIZE(size));
    if (table->force_shutdown)
        memcpy(force_shutdown, table->force_shutdown,
            BITS_SIZE(table->size));

    free(table->files);
    table->files = files;
    free(table->cloexec);
    table->cloexec = cloexec;
    free(table->force_shutdown);
    table->force_shutdown = force_shutdown;
    table->size = size;
    return 0;
}

struct fdtable *fdtable_copy(struct fdtable *table) {
    lock(&table->lock);
    int size = table->size;
    struct fdtable *new_table = fdtable_new(size);
    if (IS_ERR(new_table)) {
        unlock(&table->lock);
        return new_table;
    }
    memcpy(new_table->files, table->files, sizeof(struct fd *) * size);
    for (fd_t f = 0; f < size; f++)
        if (new_table->files[f])
            fd_retain(new_table->files[f]);
    memcpy(new_table->cloexec, table->cloexec, BITS_SIZE(size));
    unlock(&table->lock);
    return new_table;
}

static int fdtable_expand(struct fdtable *table, fd_t max,
        rlim_t_ nofile_limit) {
    unsigned size = max + 1;
    if (size > nofile_limit)
        return _EMFILE;
    if (table->size >= size)
        return 0;
    return fdtable_resize(table, max + 1);
}

struct fd *fdtable_get(struct fdtable *table, fd_t f) {
    if (f < 0 || (unsigned) f >= current->files->size)
        return NULL;
    return table->files[f];
}

struct fd *f_get(fd_t f) {
    lock(&current->files->lock);
    struct fd *fd = fdtable_get(current->files, f);
    unlock(&current->files->lock);
    return fd;
}

static fd_t f_install_start(struct fd *fd, fd_t start,
        rlim_t_ nofile_limit) {
    assert(start >= 0);
    struct fdtable *table = current->files;
    rlim_t_ size = nofile_limit;
    if (size > table->size)
        size = table->size;

    fd_t f;
    for (f = start; (unsigned) f < size; f++)
        if (table->files[f] == NULL)
            break;
    if ((unsigned) f >= size) {
        int err = fdtable_expand(table, f, nofile_limit);
        if (err < 0)
            f = err;
    }

    if (f >= 0) {
        table->files[f] = fd;
        bit_clear(f, table->cloexec);
        bit_clear(f, table->force_shutdown);
    } else {
        fd_close(fd);
    }
    return f;
}

fd_t f_install(struct fd *fd, int flags) {
    rlim_t_ nofile_limit = rlimit(RLIMIT_NOFILE_);
    lock(&current->files->lock);
    fd_t f = f_install_start(fd, 0, nofile_limit);
    if (f >= 0) {
        if (flags & O_CLOEXEC_)
            bit_set(f, current->files->cloexec);
        if (flags & O_NONBLOCK_)
            fd_setflags(fd, O_NONBLOCK_);
    }
    unlock(&current->files->lock);
    return f;
}

static int fdtable_close(struct fdtable *table, fd_t f) {
    if (f < 0 || (unsigned) f >= table->size)
        return _EBADF;
    struct fd *fd = table->files[f];
    if (fd == NULL)
        return _EBADF;
    if (table->unsnapshotted_force_detached_refs != 0)
        return _EBUSY;
    if (fd->inode != NULL) // temporary hack for files like sockets that right now don't have inodes but will eventually
        file_lock_remove_owned_by(fd, table);
    int err = fd_close_internal(fd, bit_test(f, table->force_shutdown));
    table->files[f] = NULL;
    bit_clear(f, table->cloexec);
    bit_clear(f, table->force_shutdown);
    return err;
}

int f_close(fd_t f) {
    lock(&current->files->lock);
    int err = fdtable_close(current->files, f);
    unlock(&current->files->lock);
    return err;
}

dword_t sys_close(fd_t f) {
    STRACE("close(%d)", f);
    return f_close(f);
}

// Linux 5.9+ syscall: close every fd in [first, last] (inclusive).
//
//   flags & CLOSE_RANGE_UNSHARE  (1 << 1): caller wants a private
//       copy of the fd table before closing. iSH's f_close already
//       only touches current's table, so unshare is a no-op for us.
//   flags & CLOSE_RANGE_CLOEXEC (1 << 2): set FD_CLOEXEC on the
//       fds in the range instead of closing them.
//
// Many tools (Bun, modern Go, glibc 2.34+, anything that probes
// the syscall and falls back to /proc/self/fd otherwise) try this
// at startup; returning ENOSYS makes them either spin a SIGTRAP
// debug-handler or fall back to a per-fd close loop. Implementing
// it natively avoids both.
#define CLOSE_RANGE_UNSHARE_ (1u << 1)
#define CLOSE_RANGE_CLOEXEC_ (1u << 2)
dword_t sys_close_range(dword_t first, dword_t last, dword_t flags) {
    STRACE("close_range(%u, %u, %#x)", first, last, flags);
    if (last < first) return _EINVAL;
    if (flags & ~(CLOSE_RANGE_UNSHARE_ | CLOSE_RANGE_CLOEXEC_))
        return _EINVAL;

    struct fdtable *table = current->files;
    lock(&table->lock);
    unsigned hi = last;
    if (hi >= table->size) hi = table->size > 0 ? table->size - 1 : 0;
    for (unsigned f = first; f <= hi; f++) {
        if (table->files[f] == NULL) continue;
        if (flags & CLOSE_RANGE_CLOEXEC_) {
            bit_set(f, table->cloexec);
        } else {
            fdtable_close(table, f);
        }
    }
    unlock(&table->lock);
    return 0;
}

void fdtable_do_cloexec(struct fdtable *table) {
    lock(&table->lock);
    for (fd_t f = 0; (unsigned) f < table->size; f++)
        if (bit_test(f, table->cloexec))
            fdtable_close(table, f);
    unlock(&table->lock);
}

#define F_DUPFD_ 0
#define F_GETFD_ 1
#define F_SETFD_ 2
#define F_GETFL_ 3
#define F_SETFL_ 4

#define F_GETLK_ 5
#define F_SETLK_ 6
#define F_SETLKW_ 7
#define F_GETOWN_ 9
#define F_SETOWN_ 8
#define F_GETLK64_ 12
#define F_SETLK64_ 13
#define F_SETLKW64_ 14

#define F_DUPFD_CLOEXEC_ 1030
#define F_SETPIPE_SZ_ 1031
#define F_GETPIPE_SZ_ 1032
#define F_OFD_GETLK_ 36
#define F_OFD_SETLK_ 37
#define F_OFD_SETLKW_ 38

dword_t sys_dup(fd_t f) {
    STRACE("dup(%d)", f);
    struct fdtable *table = current->files;
    rlim_t_ nofile_limit = rlimit(RLIMIT_NOFILE_);
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL) {
        unlock(&table->lock);
        return _EBADF;
    }
    fd_retain(fd);
    fd_t new_f = f_install_start(fd, 0, nofile_limit);
    unlock(&table->lock);
    return new_f;
}

static dword_t duplicate_to(fd_t f, fd_t new_f, int_t flags,
        bool allow_same_descriptor) {
    if (new_f < 0)
        return _EBADF;
    if (f == new_f && !allow_same_descriptor)
        return _EINVAL;

    struct fdtable *table = current->files;
    rlim_t_ nofile_limit = rlimit(RLIMIT_NOFILE_);
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL)
        goto bad_fd;
    if (f == new_f) {
        unlock(&table->lock);
        return new_f;
    }
    int err = fdtable_expand(table, new_f, nofile_limit);
    if (err < 0) {
        unlock(&table->lock);
        return err;
    }
    if (table->files[new_f] != NULL) {
        int close_error = fdtable_close(table, new_f);
        if (close_error < 0) {
            unlock(&table->lock);
            return close_error;
        }
    }
    fd_retain(fd);
    table->files[new_f] = fd;
    bit_clear(new_f, table->force_shutdown);
    if (flags & O_CLOEXEC_)
        bit_set(new_f, table->cloexec);
    unlock(&table->lock);
    return new_f;

bad_fd:
    unlock(&table->lock);
    return _EBADF;
}

dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags) {
    STRACE("dup3(%d, %d, %d)", f, new_f, flags);
    if (flags & ~O_CLOEXEC_)
        return _EINVAL;
    return duplicate_to(f, new_f, flags, false);
}

dword_t sys_dup2(fd_t f, fd_t new_f) {
    STRACE("dup2(%d, %d)", f, new_f);
    return duplicate_to(f, new_f, 0, true);
}

int fd_getflags(struct fd *fd) {
    if (fd->ops->getflags)
        return fd->ops->getflags(fd);
    return fd->flags;
}

#define FD_ALLOWED_FLAGS (O_APPEND_ | O_NONBLOCK_)
int fd_setflags(struct fd *fd, int flags) {
    if (fd->ops->setflags)
        return fd->ops->setflags(fd, flags);
    fd->flags = (fd->flags & ~FD_ALLOWED_FLAGS) | (flags & FD_ALLOWED_FLAGS);
    return 0;
}

dword_t sys_fcntl(fd_t f, dword_t cmd, addr_t arg) {
    struct fdtable *table = current->files;
    if (cmd == F_DUPFD_ || cmd == F_DUPFD_CLOEXEC_) {
        STRACE("fcntl(%d, %s, %d)", f,
            cmd == F_DUPFD_ ? "F_DUPFD" : "F_DUPFD_CLOEXEC", arg);
        rlim_t_ nofile_limit = rlimit(RLIMIT_NOFILE_);
        lock(&table->lock);
        struct fd *duplicate = fdtable_get(table, f);
        if (duplicate == NULL) {
            unlock(&table->lock);
            return _EBADF;
        }
        fd_retain(duplicate);
        fd_t installed = f_install_start(duplicate, arg, nofile_limit);
        if (installed >= 0 && cmd == F_DUPFD_CLOEXEC_)
            bit_set(installed, table->cloexec);
        unlock(&table->lock);
        return installed;
    }

    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    struct flock32_ flock32;
    struct flock_ flock;
    int err;
    switch (cmd) {
        case F_GETFD_:
            STRACE("fcntl(%d, F_GETFD)", f);
            return bit_test(f, table->cloexec);
        case F_SETFD_:
            STRACE("fcntl(%d, F_SETFD, 0x%x)", f, arg);
            if (arg & 1)
                bit_set(f, table->cloexec);
            else
                bit_clear(f, table->cloexec);
            return 0;

        case F_GETFL_:
            STRACE("fcntl(%d, F_GETFL)", f);
            return fd_getflags(fd);
        case F_SETFL_:
            STRACE("fcntl(%d, F_SETFL, %#x)", f, arg);
            return fd_setflags(fd, arg);

        case F_SETOWN_:
            STRACE("fcntl(%d, F_SETOWN, %d)", f, arg);
            return 0;
        case F_GETOWN_:
            STRACE("fcntl(%d, F_GETOWN)", f);
            return 0;

        case F_SETPIPE_SZ_:
            STRACE("fcntl(%d, F_SETPIPE_SZ, %d)", f, arg);
            return arg;
        case F_GETPIPE_SZ_:
            STRACE("fcntl(%d, F_GETPIPE_SZ)", f);
            return 65536;

        case F_GETLK_:
            STRACE("fcntl(%d, F_GETLK, %#x)", f, arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0) {
                flock32.type = flock.type;
                flock32.whence = flock.whence;
                flock32.start = flock.start;
                flock32.len = flock.len;
                flock32.pid = flock.pid;
                if (user_write(arg, &flock32, sizeof(flock32)))
                    return _EFAULT;
            }
            return err;

        case F_GETLK64_:
            STRACE("fcntl(%d, F_GETLK64, %#x)", f, arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0)
                if (user_write(arg, &flock, sizeof(flock)))
                    return _EFAULT;
            return err;

        case F_SETLK_:
        case F_SETLKW_:
            STRACE("fcntl(%d, F_SETLK%*s, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW64_);

        case F_SETLK64_:
        case F_SETLKW64_:
        case F_OFD_SETLK_:
        case F_OFD_SETLKW_:
            STRACE("fcntl(%d, F_SETLK%*s64, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW_);

        case F_OFD_GETLK_:
            STRACE("fcntl(%d, F_OFD_GETLK, %#x)", f, arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0)
                if (user_write(arg, &flock, sizeof(flock)))
                    return _EFAULT;
            return err;

        default:
            STRACE("fcntl(%d, %d)", f, cmd);
            return _EINVAL;
    }
}

dword_t sys_fcntl32(fd_t fd, dword_t cmd, addr_t arg) {
    switch (cmd) {
        case F_GETLK64_:
        case F_SETLK64_:
        case F_SETLKW64_:
            return _EINVAL;
    }
    return sys_fcntl(fd, cmd, arg);
}

// NOTE: openminis/master ships a simpler sys_close_range definition here;
// we keep the fuller implementation above (handles CLOSE_RANGE_UNSHARE,
// rejects invalid flag bits) and drop the duplicate. Behaviour is a
// superset of what the upstream sibling provided.
#if 0
dword_t sys_close_range_openminis_duplicate(uint32_t first, uint32_t last, uint32_t flags) {
    STRACE("close_range(%u, %u, 0x%x)", first, last, flags);
    if (first > last)
        return _EINVAL;
    lock(&current->files->lock);
    fd_t lim = current->files->size;
    if ((fd_t)last >= lim) last = lim - 1;
    int err = 0;
    for (fd_t f = (fd_t)first; f <= (fd_t)last; f++) {
        if (current->files->files[f] == NULL) continue;
        if (flags & CLOSE_RANGE_CLOEXEC_) {
            bit_set(f, current->files->cloexec);
            continue;
        }
        int e = fdtable_close(current->files, f);
        if (e != 0 && err == 0) err = e;
    }
    unlock(&current->files->lock);
    return 0;
}
#endif
