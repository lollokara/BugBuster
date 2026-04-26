#ifndef SHIM_POLL_H
#define SHIM_POLL_H

// Minimal poll.h shim for MicroPython's vfs_posix.c
// ESP-IDF's newlib doesn't provide poll.h, but vfs_posix.c only needs
// the struct pollfd and some constants to compile (even if not fully used).

#include <sys/types.h>

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

#endif // SHIM_POLL_H
