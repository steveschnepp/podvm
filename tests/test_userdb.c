/*
 * test_userdb.c - unit tests for podvm.user resolution.
 *
 * Drives userdb_resolve() against small fixture passwd/group files in a
 * temp dir, covering the docker-style spec matrix: numeric, named,
 * uid:gid, name:group, and the not-found paths.
 *
 * Build: gcc -Wall -Wextra -O2 -g -o test_userdb test_userdb.c ../src/userdb.c -I../src
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/userdb.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static char dir[64];
static char passwd_path[128];
static char group_path[128];

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(content, f);
    fclose(f);
}

/* Resolve and assert uid/gid. expect_rc: 0 = found, -1 = not found. */
static void expect(const char *spec, int want_rc, int want_uid, int want_gid) {
    int uid = -99, gid = -99;
    int rc = userdb_resolve(passwd_path, group_path, spec, &uid, &gid);
    if (rc != want_rc) {
        printf("FAIL spec '%s': rc=%d want %d\n", spec, rc, want_rc);
        failures++;
        return;
    }
    if (rc == 0 && (uid != want_uid || gid != want_gid)) {
        printf("FAIL spec '%s': uid=%d gid=%d want uid=%d gid=%d\n",
               spec, uid, gid, want_uid, want_gid);
        failures++;
    }
}

int main(void) {
    snprintf(dir, sizeof(dir), "/tmp/podvm_userdb_XXXXXX");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    snprintf(passwd_path, sizeof(passwd_path), "%s/passwd", dir);
    snprintf(group_path, sizeof(group_path), "%s/group", dir);

    /* name:passwd:uid:gid:gecos:home:shell */
    write_file(passwd_path,
        "root:x:0:0:root:/root:/bin/sh\n"
        "app:x:1000:1000:app:/app:/bin/sh\n"
        "svc:x:2000:3000:svc:/srv:/bin/false\n"
        "nogid:x:500:500:x:/x:/bin/sh\n");
    /* name:passwd:gid:members */
    write_file(group_path,
        "root:x:0:\n"
        "app:x:1000:\n"
        "staff:x:50:\n"
        "users:x:100:app,svc\n");

    /* Numeric uid:gid. */
    expect("0:0", 0, 0, 0);
    expect("1000:1000", 0, 1000, 1000);
    expect("123:456", 0, 123, 456);

    /* Bare numeric uid -> gid 0 (docker does not look up primary group). */
    expect("1000", 0, 1000, 0);
    expect("42", 0, 42, 0);

    /* Named user -> uid + primary gid from passwd. */
    expect("root", 0, 0, 0);
    expect("app", 0, 1000, 1000);
    expect("svc", 0, 2000, 3000);   /* passwd gid, not group file */

    /* name:gid (numeric group overrides). */
    expect("app:0", 0, 1000, 0);
    expect("svc:50", 0, 2000, 50);

    /* name:group (named group resolved from group file). */
    expect("app:staff", 0, 1000, 50);
    expect("svc:users", 0, 2000, 100);

    /* uid:group (numeric uid + named group). */
    expect("1000:staff", 0, 1000, 50);

    /* uid:gid with named group already covered; empty group half. */
    expect("app:", 0, 1000, 1000);  /* empty group -> ignore, keep primary */

    /* Not-found paths. */
    expect("nosuchuser", -1, 0, 0);
    expect("app:nosuchgroup", -1, 0, 0);
    expect("", -1, 0, 0);

    /* NULL / degenerate args must not crash. */
    {
        int uid, gid;
        CHECK(userdb_resolve(passwd_path, group_path, NULL, &uid, &gid) == -1);
        CHECK(userdb_resolve(NULL, NULL, "app", &uid, &gid) == -1);
        CHECK(userdb_resolve(passwd_path, group_path, "app", NULL, &gid) == -1);
    }

    /* Cleanup fixture files. */
    unlink(passwd_path);
    unlink(group_path);
    rmdir(dir);

    if (failures == 0) {
        printf("userdb tests: all passed\n");
        return 0;
    }
    printf("userdb tests: %d failure(s)\n", failures);
    return 1;
}
