#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include "kernel/errno.h"
#include "util/timer.h"
#include "misc.h"

// Weak so the host lifetime test can deterministically exercise pthread
// resource exhaustion without changing production behavior.
__attribute__((weak)) int timer_thread_create(pthread_t *thread,
        void *(*start_routine)(void *), void *argument) {
    return pthread_create(thread, NULL, start_routine, argument);
}

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data) {
//    assert(clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME);
    struct timer *timer = malloc(sizeof(struct timer));
    timer->clockid = clockid;
    timer->callback = callback;
    timer->data = data;
    timer->active = false;
    timer->thread_running = false;
    lock_init(&timer->lock);
    cond_init(&timer->finished);
    timer->dead = false;
    return timer;
}

void timer_free(struct timer *timer) {
    lock(&timer->lock);
    timer->active = false;
    if (timer->thread_running) {
        timer->dead = true;
        pthread_kill(timer->thread, SIGUSR1);
        unlock(&timer->lock);
    } else {
        unlock(&timer->lock);
        cond_destroy(&timer->finished);
        free(timer);
    }
}

void timer_free_sync(struct timer *timer) {
    lock(&timer->lock);
    timer->active = false;
    if (timer->thread_running) {
        pthread_kill(timer->thread, SIGUSR1);
        // The callback executes with timer->lock held. Acquiring the lock above
        // therefore waits for an in-flight callback, and this condition waits
        // for the detached timer pthread to stop touching timer storage.
        while (timer->thread_running)
            wait_for_ignore_signals(&timer->finished, &timer->lock, NULL);
    }
    unlock(&timer->lock);
    cond_destroy(&timer->finished);
    free(timer);
}

static void *timer_thread(void *param) {
    struct timer *timer = param;
    lock(&timer->lock);
    while (true) {
        struct timespec remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        while (timer->active && timespec_positive(remaining)) {
            unlock(&timer->lock);
            nanosleep(&remaining, NULL);
            lock(&timer->lock);
            remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        }
        if (timer->active)
            timer->callback(timer->data);
        if (timer->active && timespec_positive(timer->interval)) {
            timer->start = timer->end;
            timer->end = timespec_add(timer->start, timer->interval);
        } else {
            break;
        }
    }
    timer->thread_running = false;
    notify(&timer->finished);
    if (timer->dead) {
        unlock(&timer->lock);
        cond_destroy(&timer->finished);
        free(timer);
    } else {
        unlock(&timer->lock);
    }
    return NULL;
}

int timer_set(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec) {
    lock(&timer->lock);
    struct timespec now = timespec_now(timer->clockid);
    if (oldspec != NULL) {
        oldspec->value = timespec_subtract(timer->end, now);
        oldspec->interval = timer->interval;
    }

    timer->start = now;
    timer->end = timespec_add(timer->start, spec.value);
    timer->interval = spec.interval;
    timer->active = !timespec_is_zero(spec.value);
    if (timer->thread_running) {
        pthread_kill(timer->thread, SIGUSR1);
    } else if (timer->active) {
        timer->thread_running = true;
        int create_error = timer_thread_create(&timer->thread, timer_thread,
                timer);
        if (create_error != 0) {
            timer->thread_running = false;
            timer->active = false;
            unlock(&timer->lock);
            errno = create_error;
            return errno_map();
        }
        pthread_detach(timer->thread);
    }
    unlock(&timer->lock);
    return 0;
}
