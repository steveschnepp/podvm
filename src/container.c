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
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ftw.h>

#include "container.h"

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

    const char *xdg = getenv("XDG_STATE_HOME");
    char base[CONTAINER_PATH_MAX];
    if (xdg && xdg[0]) {
        snprintf(base, sizeof(base), "%s/podvm/containers", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return -1;
        snprintf(base, sizeof(base), "%s/.local/state/podvm/containers", home);
    }
    snprintf(out, n, "%s/%s", base, name);
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
