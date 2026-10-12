/*
 * subuid.c - subuid-range id translation (ADR-028)
 *
 * Pure translation; no fork dance, no newuidmap. Rootless podman's
 * on-disk convention is: container root = the invoking user, container
 * uids 1..N = the subuid allocation. podvm's xattrs speak guest ids
 * directly, so this mapping only matters when reading an on-disk uid
 * that has no xattr (podman diff dirs served directly).
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#include "subuid.h"

int subuid_parse_file(const char *path, const char *name,
                      unsigned long *base, unsigned long *count) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t nlen = strlen(name);
    char line[512];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        /* name:base:count -- name may not contain ':' */
        char *colon = strchr(line, ':');
        if (!colon) continue;
        if ((size_t)(colon - line) != nlen || strncmp(line, name, nlen))
            continue;
        unsigned long b, c;
        if (sscanf(colon + 1, "%lu:%lu", &b, &c) == 2 && c > 0) {
            *base = b;
            *count = c;
            rc = 0;
            break;
        }
    }
    fclose(f);
    return rc;
}

int subuid_self(unsigned long *base, unsigned long *count) {
    /* cached: may be called per-file when packing a layer */
    static int cached = 0;
    static unsigned long cbase, ccount;
    static int cok = -1;
    if (!cached) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_name)
            cok = subuid_parse_file("/etc/subuid", pw->pw_name, &cbase, &ccount);
        cached = 1;
    }
    if (cok != 0) return -1;
    *base = cbase;
    *count = ccount;
    return 0;
}

unsigned subuid_to_guest(unsigned long host_id) {
    if (host_id == (unsigned long)getuid()) return 0;
    unsigned long base = 0, count = 0;
    if (subuid_self(&base, &count) == 0 &&
        host_id >= base && host_id < base + count)
        return (unsigned)(host_id - base + 1);
    return 0;   /* unknown owner collapses to root */
}

unsigned long subuid_from_guest(unsigned guest_id) {
    if (guest_id == 0) return (unsigned long)getuid();
    unsigned long base = 0, count = 0;
    if (subuid_self(&base, &count) == 0 && guest_id <= count)
        return base + guest_id - 1;
    return (unsigned long)getuid();
}
