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
