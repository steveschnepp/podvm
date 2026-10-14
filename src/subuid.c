/*
 * subuid.c - read /etc/subuid+/etc/subgid and write a child's uid/gid map
 * via the setuid newuidmap/newgidmap helpers (ADR-028, path B).
 *
 * See subuid.h for why this exists: a multi-line uid_map needs CAP_SETUID
 * in the parent namespace, which only the setuid helpers have.
 */
#define _GNU_SOURCE
#include "subuid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <sched.h>      /* unshare */
#include <signal.h>     /* kill */
#include <ftw.h>        /* nftw */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

/*
 * Find `user`'s line in an /etc/sub{u,g}id-style file: "name:base:count".
 * Takes the FIRST matching line (a user normally has one allocation).
 * Returns 1 and fills base/count on success, 0 if absent/unparsable.
 */
static int read_alloc_file(const char *path, const char *user,
                           unsigned *base, unsigned *count) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t ulen = strlen(user);
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline so strchr-based parsing is exact */
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, user, ulen) != 0 || line[ulen] != ':') continue;
        char *p = line + ulen + 1;
        char *end;
        unsigned long b = strtoul(p, &end, 10);
        if (end == p || *end != ':') break;   /* no numeric base */
        char *cstart = end + 1;
        unsigned long c = strtoul(cstart, &end, 10);
        if (end == cstart || *end != '\0' || c == 0) break;
        *base = (unsigned)b;
        *count = (unsigned)c;
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

int subuid_read_alloc(SubUidAlloc *alloc) {
    const char *user = getenv("USER");
    if (!user || !*user) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) user = pw->pw_name;
    }
    if (!user) return 0;

    /* Both a uid AND a gid range are required: a half-mapped VM (uids
     * but no gids) would leave the guest with a broken group model. */
    if (!read_alloc_file("/etc/subuid", user, &alloc->uid_base, &alloc->uid_count))
        return 0;
    if (!read_alloc_file("/etc/subgid", user, &alloc->gid_base, &alloc->gid_count))
        return 0;
    return 1;
}

/* Run `argv` to completion; returns its exit status, or -1 on exec fail. */
static int run_helper(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Deny setgroups before any gid_map write. Must precede both newgidmap
 * and a direct gid_map write; harmless if /proc/pid/setgroups is absent. */
static void deny_setgroups(pid_t child) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)child);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        (void)!write(fd, "deny", 4);
        close(fd);
    }
}

int subuid_map_child(pid_t child, const SubUidAlloc *alloc) {
    deny_setgroups(child);

    /* newuidmap PID  ns-uid host-uid count [ns-uid host-uid count ...]
     * Both ranges in ONE invocation: uid_map is write-once, so a second
     * call to add the range would fail with EPERM. */
    char pid[32], u1[16], u2[16], u3[16], g1[16], g2[16], g3[16];
    snprintf(pid, sizeof(pid), "%d", (int)child);
    snprintf(u1, sizeof(u1), "0");
    snprintf(u2, sizeof(u2), "%u", (unsigned)getuid());
    snprintf(u3, sizeof(u3), "1");
    snprintf(g1, sizeof(g1), "1");
    snprintf(g2, sizeof(g2), "%u", alloc->uid_base);
    snprintf(g3, sizeof(g3), "%u", alloc->uid_count);

    char *uidargv[] = { "newuidmap", pid, u1, u2, u3, g1, g2, g3, NULL };
    if (run_helper(uidargv) != 0) {
        fprintf(stderr, "podvm: newuidmap failed (range %u+%u); "
                        "falling back to the single-uid map\n",
                alloc->uid_base, alloc->uid_count);
        return -1;               /* uid_map untouched: safe to fall back */
    }

    /* same for gids */
    char p1[16], p2[16], p3[16], q1[16], q2[16], q3[16];
    snprintf(p1, sizeof(p1), "0");
    snprintf(p2, sizeof(p2), "%u", (unsigned)getgid());
    snprintf(p3, sizeof(p3), "1");
    snprintf(q1, sizeof(q1), "1");
    snprintf(q2, sizeof(q2), "%u", alloc->gid_base);
    snprintf(q3, sizeof(q3), "%u", alloc->gid_count);
    char *gidargv[] = { "newgidmap", pid, p1, p2, p3, q1, q2, q3, NULL };
    if (run_helper(gidargv) != 0) {
        fprintf(stderr, "podvm: newgidmap failed (range %u+%u); "
                        "the uid map is already written, so this is fatal\n",
                alloc->gid_base, alloc->gid_count);
        return -2;               /* uid_map committed: no fallback possible */
    }
    return 0;
}

int subuid_map_child_single(pid_t child) {
    deny_setgroups(child);

    char path[64], buf[64];
    int n, fd, ok = 1;

    snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)child);
    n = snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)getuid());
    fd = open(path, O_WRONLY);
    if (fd < 0 || write(fd, buf, (size_t)n) != n) ok = 0;
    if (fd >= 0) close(fd);
    if (!ok) {
        fprintf(stderr, "podvm: uid_map failed: %s\n", strerror(errno));
        return -1;
    }

    snprintf(path, sizeof(path), "/proc/%d/gid_map", (int)child);
    n = snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)getgid());
    fd = open(path, O_WRONLY);
    if (fd < 0 || write(fd, buf, (size_t)n) != n) ok = 0;
    if (fd >= 0) close(fd);
    if (!ok) {
        fprintf(stderr, "podvm: gid_map failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * The container<->host ownership translation for the store's disk edges.
 *
 * A subuid-mapped userns writes files whose on-disk (host) owner encodes
 * the container id: container 0 is the invoking user, container k is
 * subuid_base + (k-1). The tar format must carry CONTAINER ids (the image
 * spec, and what podman expects on save/load) -- so pack translates
 * host->container, and anything outside the two known ranges is passed
 * through (a real host-owned file podvm did not create).
 *
 * These read the allocation fresh each call (one /etc/subuid parse; cheap,
 * and keeps them usable without threading an alloc through every caller).
 */
void subuid_host_to_container(unsigned host_uid, unsigned host_gid,
                              unsigned *out_uid, unsigned *out_gid) {
    unsigned inv_uid = (unsigned)getuid(), inv_gid = (unsigned)getgid();
    SubUidAlloc a;
    int have = subuid_read_alloc(&a);

    if (host_uid == inv_uid)      *out_uid = 0;
    else if (have && host_uid >= a.uid_base &&
             host_uid < a.uid_base + a.uid_count)
        *out_uid = (host_uid - a.uid_base) + 1;
    else                          *out_uid = host_uid;   /* pass through */

    if (host_gid == inv_gid)      *out_gid = 0;
    else if (have && host_gid >= a.gid_base &&
             host_gid < a.gid_base + a.gid_count)
        *out_gid = (host_gid - a.gid_base) + 1;
    else                          *out_gid = host_gid;
}

void subuid_container_to_host(unsigned cuid, unsigned cgid,
                              unsigned *out_uid, unsigned *out_gid) {
    unsigned inv_uid = (unsigned)getuid(), inv_gid = (unsigned)getgid();
    SubUidAlloc a;
    int have = subuid_read_alloc(&a);

    if (cuid == 0)                 *out_uid = inv_uid;
    else if (have && cuid >= 1 && cuid <= a.uid_count)
        *out_uid = a.uid_base + (cuid - 1);
    else                           *out_uid = cuid;       /* pass through */

    if (cgid == 0)                 *out_gid = inv_gid;
    else if (have && cgid >= 1 && cgid <= a.gid_count)
        *out_gid = a.gid_base + (cgid - 1);
    else                           *out_gid = cgid;
}

/* ── subuid-aware recursive delete ───────────────────────────────────
 *
 * The ownership round-trip (ADR-028 ingest) leaves store/state trees
 * owned by subuids. An unprivileged `rm -rf` cannot delete those: unlink
 * needs write on the PARENT, and a subuid-owned 0755 directory grants the
 * invoking user none. That broke rmi/prune/--rm and the e2e traps once
 * ownership started surviving.
 *
 * The fix (what rootless podman does): run the delete INSIDE a
 * subuid-mapped userns, where the process is ns-root over the range and
 * so owns every subuid-owned file. The chmod pre-pass matters too --
 * overlayfs leaves its work dir at mode 000, and chmod needs ownership.
 */
static int rm_one_cb(const char *path, const struct stat *st, int type,
                     struct FTW *ftwbuf) {
    (void)st; (void)ftwbuf;
    int isdir = (type == FTW_D || type == FTW_DNR || type == FTW_DP);
    int rc = isdir ? rmdir(path) : unlink(path);
    return (rc < 0 && errno != ENOENT) ? -1 : 0;
}

/* Pre-pass: open every dir so the delete can descend (overlay work dirs
 * are mode 000). Symlinks untouched (FTW_PHYS + the type test). */
static int chmod_one_cb(const char *path, const struct stat *st, int type,
                        struct FTW *ftwbuf) {
    (void)path; (void)st; (void)ftwbuf;
    if ((type == FTW_D || type == FTW_DNR) &&
        chmod(path, 0700) < 0 && errno != ENOENT) return -1;
    return 0;
}

int subuid_rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return -1;       /* errno = ENOENT if absent */

    /* Fast path: most trees podvm deletes are user-owned (save/load
     * staging, build temp, an empty state dir). A plain delete handles
     * those with no fork. It aborts on the first EPERM -- a subuid-owned
     * dir the chmod pre-pass cannot open -- which is exactly when we must
     * escalate to the userns below. */
    if (nftw(path, chmod_one_cb, 16, FTW_PHYS) == 0 &&
        nftw(path, rm_one_cb, 16, FTW_DEPTH | FTW_PHYS) == 0)
        return 0;

    /* Slow path: the tree holds subuid-owned files. Re-run the delete in
     * a subuid-mapped userns (ns-root over the range), the only context
     * where chmod+unlink succeed on them. The prior partial attempt is
     * harmless: rm_one_cb tolerates ENOENT, so the retry finishes it. */
    int ready[2], go[2];
    if (pipe(ready) < 0 || pipe(go) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(ready[0]); close(go[1]);
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0) _exit(1);
        char x = 'U';
        if (write(ready[1], &x, 1) != 1) _exit(1);   /* unshared */
        if (read(go[0], &x, 1) != 1) _exit(1);       /* map written */
        close(ready[1]); close(go[0]);
        /* ns-root over the subuid range: chmod + unlink now succeed on
         * every subuid-owned file podvm created. */
        int rc = nftw(path, chmod_one_cb, 16, FTW_PHYS);
        if (rc == 0) rc = nftw(path, rm_one_cb, 16, FTW_DEPTH | FTW_PHYS);
        _exit(rc == 0 ? 0 : 1);
    }
    close(ready[1]); close(go[0]);
    char x;
    if (read(ready[0], &x, 1) != 1) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }
    SubUidAlloc alloc;
    int mapped = 0, fatal = 0;
    if (subuid_read_alloc(&alloc)) {
        int rc = subuid_map_child(pid, &alloc);
        mapped = (rc == 0);
        fatal = (rc == -2);       /* uid map committed, gid failed: no fallback */
    }
    if (!mapped && !fatal && subuid_map_child_single(pid) < 0) fatal = 1;
    close(ready[0]);
    if (fatal) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }
    x = 'M';
    if (write(go[1], &x, 1) != 1) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }
    close(go[1]);
    int wst = 0;
    waitpid(pid, &wst, 0);
    return (WIFEXITED(wst) && WEXITSTATUS(wst) == 0) ? 0 : -1;
}
