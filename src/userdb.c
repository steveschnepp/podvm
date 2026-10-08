/*
 * userdb.c - resolve a podvm.user spec to uid:gid from passwd/group files
 *
 * See userdb.h for the accepted forms. Pure parsing, no NSS, no
 * dependency -- the guest links statically and getpwnam under static
 * linking is fragile across libc implementations.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "userdb.h"

/* Is every character a digit (and is the string non-empty)? */
static int all_digits(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* Extract colon-separated field `idx` (0-based) from a line into out.
 * Returns 1 if the field exists, 0 otherwise. Trailing newline is
 * stripped from the extracted field. */
static int field(const char *line, int idx, char *out, size_t outsz) {
    const char *p = line;
    for (int i = 0; i < idx; i++) {
        p = strchr(p, ':');
        if (!p) return 0;
        p++;
    }
    const char *end = strchr(p, ':');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    /* strip trailing newline/CR */
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/*
 * Look up `name` in a passwd-style file. On a match, writes the uid
 * (field 2) to *uid and the gid (field 3) to *gid. Returns 0 on match,
 * -1 if not found or the file is unreadable.
 */
static int passwd_lookup(const char *path, const char *name,
                         long *uid, long *gid) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512], nm[256], uidbuf[64], gidbuf[64];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        if (!field(line, 0, nm, sizeof(nm))) continue;
        if (strcmp(nm, name) != 0) continue;
        if (field(line, 2, uidbuf, sizeof(uidbuf)) &&
            field(line, 3, gidbuf, sizeof(gidbuf))) {
            *uid = strtol(uidbuf, NULL, 10);
            *gid = strtol(gidbuf, NULL, 10);
            rc = 0;
        }
        break;
    }
    fclose(f);
    return rc;
}

/*
 * Look up a group name in a group-style file. On a match, writes the gid
 * (field 2) to *gid. Returns 0 on match, -1 otherwise.
 */
static int group_lookup(const char *path, const char *name, long *gid) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512], nm[256], gidbuf[64];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        if (!field(line, 0, nm, sizeof(nm))) continue;
        if (strcmp(nm, name) != 0) continue;
        if (field(line, 2, gidbuf, sizeof(gidbuf))) {
            *gid = strtol(gidbuf, NULL, 10);
            rc = 0;
        }
        break;
    }
    fclose(f);
    return rc;
}

int userdb_resolve(const char *passwd_path, const char *group_path,
                   const char *spec, int *uid, int *gid) {
    if (!spec || !*spec || !uid || !gid) return -1;

    char user[256], grp[256];
    const char *colon = strchr(spec, ':');

    if (colon) {
        size_t ulen = (size_t)(colon - spec);
        if (ulen >= sizeof(user)) ulen = sizeof(user) - 1;
        memcpy(user, spec, ulen);
        user[ulen] = '\0';
        snprintf(grp, sizeof(grp), "%s", colon + 1);
    } else {
        snprintf(user, sizeof(user), "%s", spec);
        grp[0] = '\0';
    }

    long u = 0, g = 0;

    /* Resolve the user half. */
    if (all_digits(user)) {
        u = strtol(user, NULL, 10);
        /* A bare numeric uid with no group: keep gid 0 (docker does not
         * look up the primary group for a numeric uid). */
        g = 0;
    } else {
        if (passwd_lookup(passwd_path, user, &u, &g) != 0)
            return -1;   /* unknown user */
    }

    /* Resolve the group half, if given. */
    if (grp[0]) {
        if (all_digits(grp)) {
            g = strtol(grp, NULL, 10);
        } else {
            if (group_lookup(group_path, grp, &g) != 0)
                return -1;   /* unknown group name */
        }
    }

    *uid = (int)u;
    *gid = (int)g;
    return 0;
}
