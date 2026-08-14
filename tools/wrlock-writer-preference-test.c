#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>

#include "asbestos/asbestos.h"

enum {
    writer_count = 16,
};

struct fixture {
    struct asbestos asbestos;
    atomic_uint ready;
    atomic_uint acquired;
    atomic_uint active;
    atomic_uint completed;
    atomic_bool reader_acquired;
    atomic_bool release_writer;
};

static void wait_for_uint(atomic_uint *value, unsigned expected) {
    const struct timespec pause = {
        .tv_nsec = 1000000,
    };
    for (unsigned attempt = 0; attempt < 5000; attempt++) {
        if (atomic_load_explicit(value, memory_order_acquire) == expected)
            return;
        nanosleep(&pause, NULL);
    }
    assert(!"timed out waiting for writer state");
}

#ifdef __APPLE__
static void wait_for_queued_writers(wrlock_t *lock, unsigned expected) {
    const struct timespec pause = {
        .tv_nsec = 1000000,
    };
    for (unsigned attempt = 0; attempt < 5000; attempt++) {
        assert(pthread_mutex_lock(&lock->state_lock) == 0);
        unsigned waiting_writers = lock->waiting_writers;
        assert(pthread_mutex_unlock(&lock->state_lock) == 0);
        if (waiting_writers == expected)
            return;
        nanosleep(&pause, NULL);
    }
    assert(!"timed out waiting for queued writers");
}
#endif

static void *writer_main(void *opaque) {
    struct fixture *fixture = opaque;
    atomic_fetch_add_explicit(&fixture->ready, 1, memory_order_release);

    write_wrlock(&fixture->asbestos.dirty_coherence_lock);
    unsigned previous = atomic_fetch_add_explicit(&fixture->active, 1,
            memory_order_acq_rel);
    assert(previous == 0);
    atomic_fetch_add_explicit(&fixture->acquired, 1, memory_order_release);

    while (!atomic_load_explicit(&fixture->release_writer,
            memory_order_acquire)) {
        const struct timespec pause = {
            .tv_nsec = 1000000,
        };
        nanosleep(&pause, NULL);
    }

    previous = atomic_fetch_sub_explicit(&fixture->active, 1,
            memory_order_acq_rel);
    assert(previous == 1);
    atomic_fetch_add_explicit(&fixture->completed, 1, memory_order_release);
    write_wrunlock(&fixture->asbestos.dirty_coherence_lock);
    return NULL;
}

#ifdef __APPLE__
static void *reader_main(void *opaque) {
    struct fixture *fixture = opaque;
    read_wrlock(&fixture->asbestos.dirty_coherence_lock);
    // A reader arriving after queued writers must not barge between them.
    assert(atomic_load_explicit(&fixture->completed, memory_order_acquire) ==
            writer_count);
    atomic_store_explicit(&fixture->reader_acquired, true,
            memory_order_release);
    read_wrunlock(&fixture->asbestos.dirty_coherence_lock);
    return NULL;
}
#endif

int main(void) {
    struct fixture fixture = {0};
    wrlock_init(&fixture.asbestos.dirty_coherence_lock);
    atomic_init(&fixture.ready, 0);
    atomic_init(&fixture.acquired, 0);
    atomic_init(&fixture.active, 0);
    atomic_init(&fixture.completed, 0);
    atomic_init(&fixture.reader_acquired, false);
    atomic_init(&fixture.release_writer, false);

    // Hold a reader while all writers arrive. This is the multi-writer wait
    // pattern that has wedged Darwin's psynch rwlock.
    read_wrlock(&fixture.asbestos.dirty_coherence_lock);
    assert(!write_wrtrylock(&fixture.asbestos.dirty_coherence_lock));
    pthread_t writers[writer_count];
    for (unsigned i = 0; i < writer_count; i++)
        assert(pthread_create(&writers[i], NULL, writer_main, &fixture) == 0);

    wait_for_uint(&fixture.ready, writer_count);
    assert(atomic_load_explicit(&fixture.acquired, memory_order_acquire) == 0);
#ifdef __APPLE__
    wait_for_queued_writers(&fixture.asbestos.dirty_coherence_lock,
            writer_count);
#endif
    read_wrunlock(&fixture.asbestos.dirty_coherence_lock);

    // Exactly one writer may own the coherence write lock. The remaining
    // writers stay queued while new readers are held back.
    wait_for_uint(&fixture.acquired, 1);
#ifdef __APPLE__
    pthread_t reader;
    assert(pthread_create(&reader, NULL, reader_main, &fixture) == 0);
#endif
    atomic_store_explicit(&fixture.release_writer, true, memory_order_release);

    for (unsigned i = 0; i < writer_count; i++)
        assert(pthread_join(writers[i], NULL) == 0);
#ifdef __APPLE__
    assert(pthread_join(reader, NULL) == 0);
#endif
    assert(atomic_load_explicit(&fixture.acquired, memory_order_acquire) ==
            writer_count);
    assert(atomic_load_explicit(&fixture.active, memory_order_acquire) == 0);
#ifdef __APPLE__
    assert(atomic_load_explicit(&fixture.reader_acquired,
            memory_order_acquire));
#endif
    assert(write_wrtrylock(&fixture.asbestos.dirty_coherence_lock));
    write_wrunlock(&fixture.asbestos.dirty_coherence_lock);

    wrlock_destroy(&fixture.asbestos.dirty_coherence_lock);
    return 0;
}
