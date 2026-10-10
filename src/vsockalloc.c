/* vsockalloc.c - flock-based allocation of a unique guest CID and
 * exit-report port. See vsockalloc.h for the rationale. */
#define _GNU_SOURCE
#include "vsockalloc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define VSOCK_LOCKDIR_DEFAULT "/tmp/podvm/vsock-locks"
#define CID_BASE  3        /* guest CID: 0 (hypervisor) + 2 (host) reserved */
#define PORT_BASE 5001     /* exit-report port, previously hardcoded */
#define ALLOC_MAX 256      /* how many consecutive numbers to try */

const char *vsock_lockdir(void) {
    const char *d = getenv("PODVM_VSOCK_LOCKDIR");
    return (d && *d) ? d : VSOCK_LOCKDIR_DEFAULT;
}

static int ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* First free number in [base, base+ALLOC_MAX): flock each per-number
 * file non-blocking; the first that locks wins. Returns the number, or
 * -1 when the range is exhausted or the lock dir is unusable. */
static int alloc_num(const char *kind, int base) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", vsock_lockdir());
    if (ensure_dir(dir) < 0)
        return -1;
    for (int n = base; n < base + ALLOC_MAX; n++) {
        char path[600];
        snprintf(path, sizeof(path), "%s/%s-%d", dir, kind, n);
        int fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0)
            continue;
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            return n;   /* fd deliberately NOT closed: the reservation
                         * lives with the process (and a forked QEMU). */
        close(fd);      /* held by another VM: try the next number */
    }
    return -1;
}

int vsock_alloc_cid(void)  { return alloc_num("cid", CID_BASE); }
int vsock_alloc_port(void) { return alloc_num("port", PORT_BASE); }
