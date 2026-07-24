#include <assert.h>
#include <pthread.h>
#include <signal.h>

#include "kernel/init.h"
#include "util/sync.h"

static void assert_internal_signal_unblocked(void) {
    sigset_t mask;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0);
    assert(sigismember(&mask, SIGUSR1) == 0);
}

static void *unblock_in_guest_thread(void *unused) {
    (void)unused;
    sigset_t inherited;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &inherited) == 0);
    assert(sigismember(&inherited, SIGUSR1) == 1);
    assert(unblock_internal_signal() == 0);
    assert_internal_signal_unblocked();
    return NULL;
}

int main(void) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    assert(pthread_sigmask(SIG_BLOCK, &blocked, NULL) == 0);

    sigset_t inherited;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &inherited) == 0);
    assert(sigismember(&inherited, SIGUSR1) == 1);

    establish_signal_handlers();
    assert_internal_signal_unblocked();

    assert(pthread_sigmask(SIG_BLOCK, &blocked, NULL) == 0);
    pthread_t guest_thread;
    assert(pthread_create(&guest_thread, NULL, unblock_in_guest_thread, NULL)
           == 0);
    assert(pthread_join(guest_thread, NULL) == 0);
    return 0;
}
