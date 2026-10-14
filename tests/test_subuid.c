/*
 * test_subuid.c - container<->host ownership translation (ADR-028 ingest/commit)
 *
 * The store's tar format must carry CONTAINER uids (what the image spec
 * and podman expect), while the on-disk files it packs are owned by HOST
 * ids (the invoking user for container 0, a subuid for container 1..N).
 * subuid_host_to_container / subuid_container_to_host are the two
 * translators at those edges. An off-by-one here silently corrupts every
 * image's ownership, so the round-trip is pinned.
 *
 * Links the real src/subuid.c. Needs no VM, no newuidmap -- these are
 * pure arithmetic against /etc/subuid.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "subuid.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stdout, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); \
        failures++; \
    } \
} while (0)

/* Read the invoking user's subuid base/count from /etc/subuid, so the * expected values track the host rather than hardcoding 165536. */
static int host_subuid(unsigned *base, unsigned *count) {
    const char *user = getenv("USER");
    if (!user || !*user) return 0;
    FILE *f = fopen("/etc/subuid", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    size_t ulen = strlen(user);
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, user, ulen) != 0 || line[ulen] != ':') continue;
        if (sscanf(line + ulen + 1, "%u:%u", base, count) == 2) found = 1;
        break;
    }
    fclose(f);
    return found && *count > 0;
}

int main(void) {
    unsigned inv_uid = (unsigned)getuid(), inv_gid = (unsigned)getgid();
    unsigned cu, cg, hu, hg;

    /* container 0 <-> the invoking user (root-in-guest is us on the host) */
    subuid_host_to_container(inv_uid, inv_gid, &cu, &cg);
    CHECK(cu == 0 && cg == 0, "invoking %u:%u -> container %u:%u (want 0:0)",
          inv_uid, inv_gid, cu, cg);
    subuid_container_to_host(0, 0, &hu, &hg);
    CHECK(hu == inv_uid && hg == inv_gid,
          "container 0 -> host %u:%u (want invoking %u:%u)", hu, hg, inv_uid, inv_gid);

    /* container 1 <-> subuid base (the first non-root id) */
    unsigned base, count;
    if (host_subuid(&base, &count)) {
        subuid_host_to_container(base, base, &cu, &cg);
        CHECK(cu == 1 && cg == 1, "subuid base %u -> container %u:%u (want 1:1)",
              base, cu, cg);
        subuid_container_to_host(1, 1, &hu, &hg);
        CHECK(hu == base && hg == base, "container 1 -> host %u:%u (want base %u)",
              hu, hg, base);

        /* a mid-range id (the e2e-observed container 1000 -> base+999) */
        subuid_container_to_host(1000, 1000, &hu, &hg);
        CHECK(hu == base + 999, "container 1000 -> host %u (want base+999 = %u)",
              hu, base + 999);
        subuid_host_to_container(base + 999, base + 999, &cu, &cg);
        CHECK(cu == 1000 && cg == 1000,
              "subuid base+999 -> container %u:%u (want 1000:1000)", cu, cg);

        /* ROUND-TRIP over a spread of container ids: host->container(host(c))==c */
        for (unsigned c = 0; c <= 2000; c += 137) {
            subuid_container_to_host(c, c, &hu, &hg);
            subuid_host_to_container(hu, hg, &cu, &cg);
            CHECK(cu == c && cg == c, "round-trip container %u -> host %u -> %u",
                  c, hu, cu);
        }

        /* out-of-range container id passes through unchanged (not ours) */
        if (count + 50 < 65535) {
            subuid_container_to_host(count + 50, count + 50, &hu, &hg);
            CHECK(hu == count + 50, "out-of-range container %u -> host %u (passthrough)",
                  count + 50, hu);
        }
    } else {
        printf("note: no subuid allocation for %s; range cases skipped\n",
               getenv("USER") ? getenv("USER") : "?");
    }

    /* a foreign host uid (neither invoking nor in range) passes through */
    if (inv_uid != 12345) {
        subuid_host_to_container(12345, 12345, &cu, &cg);
        CHECK(cu == 12345, "foreign host uid 12345 -> container %u (passthrough)", cu);
    }

    if (failures) {
        printf("test_subuid: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_subuid: all checks passed\n");
    return 0;
}
