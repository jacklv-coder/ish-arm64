#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "fs/sock.h"

static void assert_layout(uint16_t fake_family, int real_family, socklen_t length) {
    uint8_t storage[sizeof(struct sockaddr_max_)];
    memset(storage, 0, sizeof(storage));
    ((struct sockaddr_ *) storage)->family = fake_family;

    assert(sock_prepare_real_address(storage, length) == real_family);
    assert(((struct sockaddr *) storage)->sa_family == real_family);
#ifdef __APPLE__
    assert(((struct sockaddr *) storage)->sa_len == length);
#endif
}

int main(void) {
    assert_layout(AF_INET_, AF_INET, sizeof(struct sockaddr_in));
    assert_layout(AF_INET6_, AF_INET6, sizeof(struct sockaddr_in6));
    return 0;
}
