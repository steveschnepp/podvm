/*
 * rf.c - remove a tree that may contain subuid-owned files
 *
 * e2e tests that build images leave a store/state tree under their $WORK
 * dir whose layer diffs are subuid-owned (ADR-028 ingest). The shell's
 * `rm -rf` cannot remove those: unlink needs write on the parent, and a
 * subuid-owned 0755 directory grants the invoking user none. That is why
 * podvm's own rmi/prune/rm go through subuid_rm_rf -- and why a plain trap
 * `rm -rf` leaves litter behind.
 *
 * This is that same product helper, exposed as a binary for the traps:
 * a plain fast path for user-owned files, escalating to a subuid-mapped
 * userns for the rest. Links the real src/subuid.c.
 *
 * Build: gcc -o rf tests/rf.c src/subuid.c -Isrc
 * Usage: tests/rf <path>   [exit 0 = gone (or was absent), 1 = still there]
 */
#include <stdio.h>
#include <sys/stat.h>

#include "subuid.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: rf <path>\n");
        return 2;
    }
    if (subuid_rm_rf(argv[1]) == 0) return 0;
    /* absent is success; anything still present is a failure to report */
    struct stat st;
    return (lstat(argv[1], &st) < 0) ? 0 : 1;
}
