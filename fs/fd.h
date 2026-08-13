#ifndef FD_H
#define FD_H
#include <dirent.h>
#include <stdatomic.h>
#include "kernel/memory.h"
#include "util/list.h"
#include "util/sync.h"
#include "util/bits.h"
#include "fs/stat.h"
#include "fs/proc.h"
#include "fs/sockrestart.h"

// FIXME almost everything that uses the structs in this file does so without any kind of sane locking

struct fd {
    atomic_uint refcount;
    // References owned by descriptor tables whose tasks were all forcibly
    // detached. When this reaches refcount, no live external owner remains.
    unsigned force_shutdown_refs;
    lock_t refcount_lock;
    unsigned flags;
    mode_t_ type; // just the S_IFMT part, it can't change
    const struct fd_ops *ops;
    struct list poll_fds;
    lock_t poll_lock;
    unsigned long offset;

    // fd data
    union {
        // tty
        struct {
            struct tty *tty;
            // links together fds pointing to the same tty
            // locked by the tty
            struct list tty_other_fds;
        };
        struct {
            struct poll *poll;
        } epollfd;
        struct {
            uint64_t val;
        } eventfd;
        struct {
            struct timer *timer;
            uint64_t expirations;
        } timerfd;
        struct {
            int domain;
            int type;
            int protocol;

            // These are only used as strong references, to keep the inode
            // alive while there is a listener.
            struct inode_data *unix_name_inode;
            struct unix_abstract *unix_name_abstract;
            uint8_t unix_name_len;
            char unix_name[108];
            struct fd *unix_peer; // locked by peer_lock, for simplicity
            cond_t unix_got_peer;
            // Queue of struct scm for sending file descriptors
            // locked by fd->lock
            struct list unix_scm;
            struct ucred_ {
                pid_t_ pid;
                uid_t_ uid;
                uid_t_ gid;
            } unix_cred;
        } socket;

        // See app/Pasteboard.m
        struct {
            // UIPasteboard.changeCount
            uint64_t generation;
            // Buffer for written data
            void* buffer;
            // its capacity
            size_t buffer_cap;
            // length of actual data stored in the buffer
            size_t buffer_len;
        } clipboard;

        // can fit anything in here
        void *data;
    };
    // fs data
    union {
        struct {
            struct proc_entry entry;
            unsigned dir_index;
            struct proc_data data;
        } proc;
        struct {
            int num;
        } devpts;
        struct {
            struct tmp_dirent *dirent;
            struct tmp_dirent *dir_pos;
        } tmpfs;
        void *fs_data;
    };

    // fs/inode data
    struct mount *mount;
    atomic_int real_fd; // seeks on this fd require the lock TODO think about making a special lock just for that
    DIR *dir;
    struct inode_data *inode;
    ino_t fake_inode;
    struct statbuf stat; // for adhoc fs
    struct fd_sockrestart sockrestart; // argh

    // these are used for a variety of things related to the fd
    lock_t lock;
    cond_t cond;
};

typedef sdword_t fd_t;
#define AT_FDCWD_ -100

struct fd *fd_create(const struct fd_ops *ops);
struct fd *fd_retain(struct fd *fd);
int fd_close(struct fd *fd);

int fd_getflags(struct fd *fd);
int fd_setflags(struct fd *fd, int flags);

#define NAME_MAX 255
struct dir_entry {
    qword_t inode;
    char name[NAME_MAX + 1];
};

#define LSEEK_SET 0
#define LSEEK_CUR 1
#define LSEEK_END 2

struct fd_ops {
    // required for files
    // TODO make optional for non-files
    ssize_t (*read)(struct fd *fd, void *buf, size_t bufsize);
    ssize_t (*write)(struct fd *fd, const void *buf, size_t bufsize);
    ssize_t (*pread)(struct fd *fd, void *buf, size_t bufsize, off_t off);
    ssize_t (*pwrite)(struct fd *fd, const void *buf, size_t bufsize, off_t off);
    off_t_ (*lseek)(struct fd *fd, off_t_ off, int whence);

    // Reads a directory entry from the stream
    // required for directories
    int (*readdir)(struct fd *fd, struct dir_entry *entry);
    // Return an opaque value representing the current point in the directory stream
    // optional, fd->offset will be used instead
    unsigned long (*telldir)(struct fd *fd);
    // Seek to the location represented by a pointer returned from telldir
    // optional, fd->offset will be used instead
    void (*seekdir)(struct fd *fd, unsigned long ptr);

    // map the file
    int (*mmap)(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags);

    // returns a bitmask of operations that won't block
    int (*poll)(struct fd *fd);

    // returns the size needed for the output of ioctl, 0 if the arg is not a
    // pointer, -1 for invalid command
    ssize_t (*ioctl_size)(int cmd);
    // if ioctl_size returns non-zero, arg must point to ioctl_size valid bytes
    int (*ioctl)(struct fd *fd, int cmd, void *arg);

    int (*fsync)(struct fd *fd);
    int (*close)(struct fd *fd);

    // handle F_GETFL, i.e. return open flags for this fd
    int (*getflags)(struct fd *fd);
    // handle F_SETFL, i.e. set O_NONBLOCK
    int (*setflags)(struct fd *fd, dword_t arg);
};

struct fdtable {
    atomic_uint refcount;
    // References owned by force-detached tasks. Protected by lock. When every
    // remaining owner is deferred, host handles can be closed to wake them.
    unsigned force_detached_refs;
    // Force-detached owners that could not allocate a per-descriptor snapshot.
    // While nonzero, installs/copies are frozen and closed slots remain as
    // hidden tombstones until the table itself is destroyed. The table's own
    // slot reference pins any in-flight borrow held by a shared owner.
    // Protected by lock.
    unsigned unsnapshotted_force_detached_refs;
    // Prevent repeated descriptor scans as deferred owners finish. Protected
    // by lock and set only after a complete shutdown pass.
    bool force_shutdown_complete;
    unsigned size;
    struct fd **files;
    bits_t *cloexec;
    // Slots whose fd references were registered by the forced-shutdown scan.
    // A syscall may still install new descriptors after that snapshot.
    bits_t *force_shutdown;
    bits_t *force_detach_closed;
    lock_t lock;
};

// Descriptor references borrowed by a force-detached task while its host
// pthread finishes an in-flight syscall. The snapshot is separate from the
// shared fd table because another CLONE_FILES owner may close or replace a slot
// before the detached pthread reaches its cleanup boundary.
struct fdtable_force_detach {
    unsigned count;
    unsigned capacity;
    struct fd *files[];
};

struct fdtable *fdtable_new(int size);
void fdtable_release(struct fdtable *table);
// The caller holds table->lock. NULL is a supported low-memory result: commit
// switches the table into fail-closed mode until that detached task returns.
struct fdtable_force_detach *fdtable_prepare_force_detach_locked(
        struct fdtable *table);
void fdtable_commit_force_detach_locked(struct fdtable *table,
        struct fdtable_force_detach *snapshot);
void fdtable_release_force_detached(struct fdtable *table,
        struct fdtable_force_detach *snapshot);
struct fdtable *fdtable_copy(struct fdtable *table);
void fdtable_free(struct fdtable *table);
void fdtable_do_cloexec(struct fdtable *table);
struct fd *fdtable_get(struct fdtable *table, fd_t f);

struct fd *f_get(fd_t f);
// steals a reference to the fd, gives it to the table on success and destroys it on error
// flags is checked for O_CLOEXEC and O_NONBLOCK
fd_t f_install(struct fd *fd, int flags);
int f_close(fd_t f);

#endif
