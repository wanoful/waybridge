#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* One-shot failure on a disposable compositor's named socket. Other sockets,
 * the private systemd manager and application filesystem calls are untouched. */
int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
    static int (*real_accept4)(int, struct sockaddr *, socklen_t *, int);
    if (!real_accept4) real_accept4 = dlsym(RTLD_NEXT, "accept4");
    const char *path = getenv("WAYBRIDGE_TEST_ACCEPT_PATH");
    const char *trigger = getenv("WAYBRIDGE_TEST_ACCEPT_TRIGGER");
    struct sockaddr_un local = {0};
    socklen_t size = sizeof(local);
    if (path && trigger && !getsockname(fd, (struct sockaddr *)&local, &size) &&
        local.sun_family == AF_UNIX && !strcmp(local.sun_path, path) && !unlink(trigger)) {
        errno = EIO;
        return -1;
    }
    return real_accept4(fd, address, length, flags);
}
