#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Test-only fault injection: stall actual inbox writes without slowing display,
 * sockets, logs, desktop-service configuration or the user's filesystem. */
ssize_t write(int fd, const void *bytes, size_t size) {
    static ssize_t (*real_write)(int, const void *, size_t);
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    const char *root = getenv("WAYBRIDGE_SLOW_TRANSFER_ROOT");
    const char *entered = getenv("WAYBRIDGE_SLOW_TRANSFER_ENTERED");
    const char *release = getenv("WAYBRIDGE_SLOW_TRANSFER_RELEASE");
    if (root && entered && release && size) {
        char link[64], path[PATH_MAX];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t length = readlink(link, path, sizeof(path) - 1);
        if (length > 0) {
            path[length] = 0;
            size_t prefix = strlen(root);
            if (!strncmp(path, root, prefix) && path[prefix] == '/') {
                int marker = open(entered, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
                if (marker >= 0) close(marker);
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
                while (access(release, F_OK)) nanosleep(&pause, NULL);
            }
        }
    }
    return real_write(fd, bytes, size);
}
