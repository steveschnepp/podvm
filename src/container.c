/*
 * container.c - named-container state (write-back layer, ADR-026)
 *
 * Host-side state for named containers: where the persistent overlay
 * upper lives, and how it is deleted (`podvm rm`, `--rm`).
 */
#define _GNU_SOURCE   /* nftw */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <ftw.h>

#include "container.h"
#include "subuid.h"   /* subuid_rm_rf: state uppers are subuid-owned */

/* mkdir -p (container.c keeps its own: main.c's is static, and these
 * are three lines of loop). */
static int mkdir_p(const char *path, mode_t mode) {
    char tmp[CONTAINER_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Shared state root: $XDG_STATE_HOME/podvm or ~/.local/state/podvm.
 * containers/ and locks/ are siblings under it. */
static int state_base(char *out, size_t n) {
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, n, "%s/podvm", xdg);
        return 0;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;
    snprintf(out, n, "%s/.local/state/podvm", home);
    return 0;
}

int container_name_valid(const char *name) {
    if (!name || !name[0]) return 0;
    if (name[0] == '-') return 0;              /* would read as an option */
    if (strchr(name, '/')) return 0;           /* would escape the state root */
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    return 1;
}

int container_state_dir(const char *name, char *out, size_t n) {
    out[0] = '\0';
    if (!name || !name[0]) return 0;           /* unnamed: no persistent state */

    char base[CONTAINER_PATH_MAX];
    if (state_base(base, sizeof(base)) < 0) return -1;
    snprintf(out, n, "%s/containers/%s", base, name);
    return 0;
}

/* The recursive-delete logic (chmod pre-pass + depth-first unlink) now
 * lives in subuid.c as subuid_rm_rf, because container state uppers hold
 * subuid-owned files that only a subuid-mapped userns can delete. The
 * two nftw callbacks that used to live here moved with it. */

int container_rm_rf(const char *path) {
    /* Container state dirs hold the writable upper, whose files are
     * subuid-owned once a guest chowns them (ADR-028). An unprivileged
     * nftw cannot unlink those -- it needs write on the parent, which is
     * itself subuid-owned. subuid_rm_rf handles both: a plain fast path
     * for user-owned trees, escalating to a subuid-mapped userns for the
     * rest. It also keeps the chmod pre-pass (overlay work dirs are mode
     * 000). */
    return subuid_rm_rf(path);
}

/* Build <base>/locks/<name>; no existence checks. */
static int lock_path(const char *name, char *out, size_t n) {
    char base[CONTAINER_PATH_MAX];
    if (!container_name_valid(name)) {
        errno = EINVAL;
        return -1;
    }
    if (state_base(base, sizeof(base)) < 0) return -1;
    int w = snprintf(out, n, "%s/locks/%s", base, name);
    if (w < 0 || (size_t)w >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int container_lock(const char *name) {
    char path[CONTAINER_PATH_MAX], dir[CONTAINER_PATH_MAX];
    char base[CONTAINER_PATH_MAX];

    if (lock_path(name, path, sizeof(path)) < 0) return -1;
    if (state_base(base, sizeof(base)) < 0) return -1;
    int dw = snprintf(dir, sizeof(dir), "%s/locks", base);
    if (dw < 0 || (size_t)dw >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir_p(dir, 0755) < 0) return -1;

    /* Deliberately NOT O_CLOEXEC: the QEMU child inherits the fd, so
     * the lock's lifetime is the VM's, not the launcher's. */
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int e = errno;
        close(fd);
        errno = e;                        /* EWOULDBLOCK when held */
        return -1;
    }
    return fd;
}

int container_locked(const char *name) {
    char path[CONTAINER_PATH_MAX];
    if (lock_path(name, path, sizeof(path)) < 0) return -1;

    int fd = open(path, O_RDWR);          /* no O_CREAT: probing only */
    if (fd < 0) {
        if (errno == ENOENT) return 0;    /* never ran (or pre-guard state) */
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int e = errno;
        close(fd);
        if (e == EWOULDBLOCK) return 1;   /* live */
        errno = e;
        return -1;
    }
    close(fd);                            /* was free; we just release it */
    return 0;
}

void container_lock_unlink(const char *name) {
    char path[CONTAINER_PATH_MAX];
    if (lock_path(name, path, sizeof(path)) < 0) return;
    unlink(path);                         /* best-effort */
}

int container_build_dir(char *out, size_t n) {
    char base[CONTAINER_PATH_MAX];
    if (state_base(base, sizeof(base)) < 0) return -1;
    snprintf(out, n, "%s/build", base);
    return mkdir_p(out, 0755);
}
