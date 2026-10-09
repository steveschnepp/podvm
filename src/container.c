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

/* nftw callback: delete depth-first (children before their parent, so
 * directories are empty by the time we rmdir them). A directory can
 * still arrive as FTW_D/FTW_DNR (pre-order) when nftw could not descend
 * into it; rmdir then too -- it needs write on the *parent*, not on the
 * dir. Non-ENOENT errors abort the walk. */
static int rm_one(const char *path, const struct stat *st, int type,
                  struct FTW *ftwbuf) {
    (void)st; (void)ftwbuf;
    int isdir = (type == FTW_D || type == FTW_DNR || type == FTW_DP);
    int rc = isdir ? rmdir(path) : unlink(path);
    return (rc < 0 && errno != ENOENT) ? -1 : 0;
}

/* nftw pre-pass: open up every directory so the delete pass can descend.
 * overlayfs leaves its work dir at mode 000, which nftw cannot opendir --
 * glibc reports that as FTW_DNR, and without this pass the delete pass
 * would try unlink() on the dir and abort with EISDIR. The tree is ours
 * (podvm created it as the invoking user), so chmod cannot fail on
 * anything we could otherwise delete. Symlinks are left alone (FTW_PHYS
 * + the FTW_D/FTW_DNR test), so this cannot chmod out of the tree. */
static int chmod_one(const char *path, const struct stat *st, int type,
                     struct FTW *ftwbuf) {
    (void)path; (void)st; (void)ftwbuf;
    if ((type == FTW_D || type == FTW_DNR) &&
        chmod(path, 0700) < 0 && errno != ENOENT) return -1;
    return 0;
}

int container_rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return -1;       /* errno = ENOENT if absent */
    if (nftw(path, chmod_one, 16, FTW_PHYS) < 0) return -1;
    return nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
}

/* Build <base>/locks/<name>; no existence checks. */
static int lock_path(const char *name, char *out, size_t n) {
    char base[CONTAINER_PATH_MAX];
    if (!container_name_valid(name)) {
        errno = EINVAL;
        return -1;
    }
    if (state_base(base, sizeof(base)) < 0) return -1;
    snprintf(out, n, "%s/locks/%s", base, name);
    return 0;
}

int container_lock(const char *name) {
    char path[CONTAINER_PATH_MAX], dir[CONTAINER_PATH_MAX];
    char base[CONTAINER_PATH_MAX];

    if (lock_path(name, path, sizeof(path)) < 0) return -1;
    if (state_base(base, sizeof(base)) < 0) return -1;
    snprintf(dir, sizeof(dir), "%s/locks", base);
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
