#ifndef UTIL_SYNC_H
#define UTIL_SYNC_H

#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <errno.h>
#include "misc.h"
#include "debug.h"

// locks, implemented using pthread

#define LOCK_DEBUG 0

typedef struct {
    pthread_mutex_t m;
    pthread_t owner;
#if LOCK_DEBUG
    struct lock_debug {
        const char *file; // doubles as locked
        int line;
        int pid;
        bool initialized;
    } debug;
#endif
} lock_t;

static inline void lock_init(lock_t *lock) {
    pthread_mutex_init(&lock->m, NULL);
#if LOCK_DEBUG
    lock->debug = (struct lock_debug) {
        .initialized = true,
    };
#endif
}

#if LOCK_DEBUG
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0, { .initialized = true }}
#else
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0}
#endif
static inline void __lock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    pthread_mutex_lock(&lock->m);
    lock->owner = pthread_self();
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(!lock->debug.file && "Attempting to recursively lock");
    lock->debug.file = file;
    lock->debug.line = line;
    extern int current_pid(void);
    lock->debug.pid = current_pid();
#endif
}
#define lock(lock) __lock(lock, __FILE__, __LINE__)
static inline void unlock(lock_t *lock) {
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(lock->debug.file && "Attempting to unlock an unlocked lock");
    lock->debug = (struct lock_debug) { .initialized = true };
#endif
    lock->owner = zero_init(pthread_t);
    pthread_mutex_unlock(&lock->m);
}

static inline int trylock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    int status = pthread_mutex_trylock(&lock->m);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(void);
        lock->debug.pid = current_pid();
    }
#endif
    return status;
}
#define trylock(lock) trylock(lock, __FILE__, __LINE__)

// conditions, implemented using pthread conditions but hacked so you can also
// be woken by a signal

typedef struct {
    pthread_cond_t cond;
} cond_t;
#define COND_INITIALIZER ((cond_t) {PTHREAD_COND_INITIALIZER})

// Must call before using the condition
void cond_init(cond_t *cond);
// Must call when finished with the condition (currently doesn't do much but might do something important eventually I guess)
void cond_destroy(cond_t *cond);
// Releases the lock, waits for the condition, and reacquires the lock.
// Returns _EINTR if waiting stopped because the thread received a signal,
// _ETIMEDOUT if waiting stopped because the timout expired, 0 otherwise.
// Will never return _ETIMEDOUT if timeout is NULL.
int must_check wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Same as wait_for, except guest signals do not interrupt the wait. Lifecycle
// force-detach still returns _EINTR so the owning host thread can clean up.
int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Wake up all waiters.
void notify(cond_t *cond);
// Wake up one waiter.
void notify_once(cond_t *cond);

// This is a read-write lock that prefers writers, i.e. if there are any
// writers waiting a read lock will block.
//
// Darwin's pthread rwlock can leave both the queued writer and later readers
// asleep under the read-to-write upgrade pattern used by mem_ptr. Implement
// the small amount of state we need with a mutex and condition variable on
// Apple platforms. Linux keeps its native writer-preferring rwlock.
typedef struct {
#ifdef __APPLE__
    pthread_mutex_t state_lock;
    pthread_cond_t state_changed;
    unsigned waiting_writers;
#else
    pthread_rwlock_t l;
#endif
    // 0: unlocked
    // -1: write-locked
    // >0: read-locked with this many readers
    atomic_int val;
    const char *file;
    int line;
    int pid;
} wrlock_t;
static inline void wrlock_init(wrlock_t *lock) {
#ifdef __APPLE__
    if (pthread_mutex_init(&lock->state_lock, NULL)) __builtin_trap();
    if (pthread_cond_init(&lock->state_changed, NULL)) __builtin_trap();
    lock->waiting_writers = 0;
#else
    pthread_rwlockattr_t *pattr = NULL;
#if defined(__GLIBC__)
    pthread_rwlockattr_t attr;
    pattr = &attr;
    pthread_rwlockattr_init(pattr);
    pthread_rwlockattr_setkind_np(pattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    if (pthread_rwlock_init(&lock->l, pattr)) __builtin_trap();
#endif
    lock->val = lock->line = lock->pid = 0;
    lock->file = NULL;
}

extern int current_pid(void);
static inline void wrlock_destroy(wrlock_t *lock) {
#ifdef __APPLE__
    if (pthread_cond_destroy(&lock->state_changed) != 0) __builtin_trap();
    if (pthread_mutex_destroy(&lock->state_lock) != 0) __builtin_trap();
#else
    if (pthread_rwlock_destroy(&lock->l) != 0) __builtin_trap();
#endif
}
static inline void read_wrlock(wrlock_t *lock) {
#ifdef __APPLE__
    if (pthread_mutex_lock(&lock->state_lock) != 0) __builtin_trap();
    while (lock->val < 0 || lock->waiting_writers != 0) {
        if (pthread_cond_wait(&lock->state_changed, &lock->state_lock) != 0)
            __builtin_trap();
    }
    lock->val++;
    if (pthread_mutex_unlock(&lock->state_lock) != 0) __builtin_trap();
#else
    if (pthread_rwlock_rdlock(&lock->l) != 0) __builtin_trap();
    assert(lock->val >= 0);
    lock->val++;
#endif
}
static inline void read_wrunlock(wrlock_t *lock) {
#ifdef __APPLE__
    if (pthread_mutex_lock(&lock->state_lock) != 0) __builtin_trap();
    assert(lock->val > 0);
    lock->val--;
    if (lock->val == 0 && lock->waiting_writers != 0) {
        if (pthread_cond_broadcast(&lock->state_changed) != 0)
            __builtin_trap();
    }
    if (pthread_mutex_unlock(&lock->state_lock) != 0) __builtin_trap();
#else
    assert(lock->val > 0);
    lock->val--;
    if (pthread_rwlock_unlock(&lock->l) != 0) __builtin_trap();
#endif
}
static inline void __write_wrlock(wrlock_t *lock, const char *file, int line) {
#ifdef __APPLE__
    if (pthread_mutex_lock(&lock->state_lock) != 0) __builtin_trap();
    lock->waiting_writers++;
    while (lock->val != 0) {
        if (pthread_cond_wait(&lock->state_changed, &lock->state_lock) != 0)
            __builtin_trap();
    }
    lock->waiting_writers--;
#else
    if (pthread_rwlock_wrlock(&lock->l) != 0) __builtin_trap();
#endif
    assert(lock->val == 0);
    lock->val = -1;
    lock->file = file;
    lock->line = line;
    lock->pid = current_pid();
#ifdef __APPLE__
    if (pthread_mutex_unlock(&lock->state_lock) != 0) __builtin_trap();
#endif
}
#define write_wrlock(lock) __write_wrlock(lock, __FILE__, __LINE__)
static inline bool write_wrtrylock(wrlock_t *lock) {
#ifdef __APPLE__
    int err = pthread_mutex_trylock(&lock->state_lock);
    if (err == EBUSY) return false;
    if (err != 0) __builtin_trap();
    if (lock->val != 0 || lock->waiting_writers != 0) {
        if (pthread_mutex_unlock(&lock->state_lock) != 0) __builtin_trap();
        return false;
    }
#else
    int err = pthread_rwlock_trywrlock(&lock->l);
    if (err == EBUSY) return false;
    if (err != 0) __builtin_trap();
#endif
    assert(lock->val == 0);
    lock->val = -1;
#ifdef __APPLE__
    if (pthread_mutex_unlock(&lock->state_lock) != 0) __builtin_trap();
#endif
    return true;
}
static inline void write_wrunlock(wrlock_t *lock) {
#ifdef __APPLE__
    if (pthread_mutex_lock(&lock->state_lock) != 0) __builtin_trap();
#endif
    assert(lock->val == -1);
    lock->val = lock->line = lock->pid = 0;
    lock->file = NULL;
#ifdef __APPLE__
    if (pthread_cond_broadcast(&lock->state_changed) != 0) __builtin_trap();
    if (pthread_mutex_unlock(&lock->state_lock) != 0) __builtin_trap();
#else
    if (pthread_rwlock_unlock(&lock->l) != 0) __builtin_trap();
#endif
}

extern __thread sigjmp_buf unwind_buf;
extern __thread bool should_unwind;
int unblock_internal_signal(void);
static inline int sigunwind_start(void) {
    if (sigsetjmp(unwind_buf, 1)) {
        should_unwind = false;
        return 1;
    } else {
        should_unwind = true;
        return 0;
    }
}
static inline void sigunwind_end(void) {
    should_unwind = false;
}

#endif
