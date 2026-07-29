set -eu

cat > rename_noreplace.c <<'EOF'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3)
        return 64;
    if (syscall(SYS_renameat2, AT_FDCWD, argv[1],
                AT_FDCWD, argv[2], 1) == 0) {
        puts("0");
        return 0;
    }
    printf("%d\n", errno);
    return 0;
}
EOF

cc -Wall -Werror rename_noreplace.c -o rename_noreplace
printf source > source
printf destination > destination

./rename_noreplace source created
./rename_noreplace created destination
cat created
echo
cat destination
echo
test ! -e source

./rename_noreplace created renamed
cat renamed
echo
test ! -e created
