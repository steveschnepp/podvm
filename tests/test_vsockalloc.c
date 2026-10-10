/*
 * test_vsockalloc.c - unique guest-CID / exit-port allocation
 *
 * Links the real src/vsockalloc.c (no mocked copy). Proves the flock
 * reservation: a number another holder has locked is skipped, and each
 * call takes the next free number. Isolated via PODVM_VSOCK_LOCKDIR.
 *
 * Build: gcc -o test_vsockalloc tests/test_vsockalloc.c src/vsockalloc.c -Isrc
 * Run: ./test_vsockalloc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

#include "vsockalloc.h"

/* Hold a number the way a second live VM would: open + flock its file. */
static int hold(const char *kind, int n) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s-%d", vsock_lockdir(), kind, n);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    assert(fd >= 0);
    assert(flock(fd, LOCK_EX | LOCK_NB) == 0);
    return fd;
}

static void test_base_values(void) {
    /* Nothing held yet: the first allocation takes each base number. */
    assert(vsock_alloc_cid() == 3);
    assert(vsock_alloc_port() == 5001);
    printf("PASS: base values (cid 3, port 5001)\n");
}

static void test_each_call_takes_next(void) {
    /* The base numbers are still reserved by the calls above, so the
     * next allocations must move to the following free numbers. */
    assert(vsock_alloc_cid() == 4);
    assert(vsock_alloc_port() == 5002);
    printf("PASS: consecutive calls take consecutive numbers\n");
}

static void test_skips_held_number(void) {
    /* Simulate a third live VM holding cid-6 and port-5005, then prove
     * allocation walks past them. Cids 3,4 are held; 5 is free. */
    int c = hold("cid", 6);
    int p = hold("port", 5005);
    assert(vsock_alloc_cid() == 5);        /* 3,4 taken -> 5 */
    assert(vsock_alloc_cid() == 7);        /* 6 held by the test -> 7 */
    assert(vsock_alloc_port() == 5003);    /* 5001,5002 taken -> 5003 */
    assert(vsock_alloc_port() == 5004);    /* 5003 now taken -> 5004 */
    assert(vsock_alloc_port() == 5006);    /* 5005 held -> skip to 5006 */
    close(c);
    close(p);
    printf("PASS: held numbers are skipped\n");
}

int main(void) {
    char tmpl[] = "/tmp/podvm-vsockalloc-test-XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);
    setenv("PODVM_VSOCK_LOCKDIR", dir, 1);

    printf("Running vsock allocation tests...\n\n");
    test_base_values();
    test_each_call_takes_next();
    test_skips_held_number();
    printf("\nAll tests passed!\n");
    return 0;
}
