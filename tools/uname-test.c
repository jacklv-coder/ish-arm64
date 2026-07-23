#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "kernel/calls.h"

extern const char *uname_hostname_override;
extern const char *uname_version;

struct guarded_uname {
    uint64_t prefix;
    struct uname value;
    uint64_t suffix;
};

static void fill_string(char *buffer, size_t size, char byte) {
    assert(size > 0);
    memset(buffer, byte, size - 1);
    buffer[size - 1] = '\0';
}

static void assert_zero_tail(const char field[UNAME_LENGTH]) {
    size_t length = strnlen(field, UNAME_LENGTH);
    assert(length < UNAME_LENGTH);
    for (size_t index = length + 1; index < UNAME_LENGTH; index++)
        assert(field[index] == '\0');
}

int main(void) {
    char long_hostname[UNAME_LENGTH * 2];
    char long_version[UNAME_LENGTH * 2];
    fill_string(long_hostname, sizeof(long_hostname), 'h');
    fill_string(long_version, sizeof(long_version), 'v');

    const char *saved_hostname = uname_hostname_override;
    const char *saved_version = uname_version;
    uname_hostname_override = long_hostname;
    uname_version = long_version;

    struct guarded_uname guarded = {
        .prefix = UINT64_C(0x1122334455667788),
        .suffix = UINT64_C(0x8877665544332211),
    };
    do_uname(&guarded.value);

    assert(guarded.prefix == UINT64_C(0x1122334455667788));
    assert(guarded.suffix == UINT64_C(0x8877665544332211));
    assert(strcmp(guarded.value.system, "Linux") == 0);
    assert_zero_tail(guarded.value.system);
    assert(strlen(guarded.value.hostname) == UNAME_LENGTH - 1);
    assert(strncmp(guarded.value.hostname, long_hostname,
            UNAME_LENGTH - 1) == 0);
    assert(guarded.value.hostname[UNAME_LENGTH - 1] == '\0');
    assert(strcmp(guarded.value.release, "4.20.69-ish") == 0);
    assert_zero_tail(guarded.value.release);
    assert(guarded.value.version[UNAME_LENGTH - 1] == '\0');
#if defined(GUEST_ARM64)
    assert(strcmp(guarded.value.arch, "aarch64") == 0);
#else
    assert(strcmp(guarded.value.arch, "i686") == 0);
#endif
    assert_zero_tail(guarded.value.arch);
    assert(strcmp(guarded.value.domain, "(none)") == 0);
    assert_zero_tail(guarded.value.domain);

    uname_hostname_override = saved_hostname;
    uname_version = saved_version;
    return 0;
}
