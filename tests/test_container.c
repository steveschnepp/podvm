/*
 * test_container.c - named-container state tests
 *
 * Links the real src/container.c (no mocked copy), like test_imgcfg /
 * test_userdb.
 *
 * Build: gcc -o test_container tests/test_container.c src/container.c -Isrc
 * Run: ./test_container
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "container.h"

/* A container name becomes a directory name, so anything that could
 * escape the state root or be read as an option must be rejected. */
static void test_name_valid(void) {
    static const struct {
        const char *name;
        int valid;
    } cases[] = {
        {"web",        1},
        {"a.b",        1},
        {"a..b",       1},   /* ".." only matters as the whole name */
        {"a.",         1},
        {"a b",        1},   /* spaces are legal in a dir name */
        {"0",          1},
        {"",           0},   /* would collide with the containers root */
        {NULL,         0},
        {".",          0},   /* would BE the containers root */
        {"..",         0},   /* would be its parent */
        {"/",          0},
        {"/abs",       0},
        {"a/b",        0},
        {"../escape",  0},
        {"-",          0},
        {"-x",         0},   /* `podvm rm -x` must not read it as a flag */
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);

    for (size_t i = 0; i < n; i++)
        assert(container_name_valid(cases[i].name) == cases[i].valid);

    printf("PASS: name validation (%zu cases)\n", n);
}

static void test_state_dir(void) {
    char dir[CONTAINER_PATH_MAX];

    /* XDG_STATE_HOME wins when set */
    setenv("XDG_STATE_HOME", "/xdg", 1);
    setenv("HOME", "/home/u", 1);
    assert(container_state_dir("web", dir, sizeof(dir)) == 0);
    assert(strcmp(dir, "/xdg/podvm/containers/web") == 0);

    /* XDG_STATE_HOME unset -> HOME fallback */
    unsetenv("XDG_STATE_HOME");
    assert(container_state_dir("web", dir, sizeof(dir)) == 0);
    assert(strcmp(dir, "/home/u/.local/state/podvm/containers/web") == 0);

    /* unnamed container: no persistent state, but not an error */
    assert(container_state_dir("", dir, sizeof(dir)) == 0);
    assert(dir[0] == '\0');
    assert(container_state_dir(NULL, dir, sizeof(dir)) == 0);
    assert(dir[0] == '\0');

    /* neither XDG_STATE_HOME nor HOME: report failure, empty path */
    unsetenv("HOME");
    assert(container_state_dir("web", dir, sizeof(dir)) == -1);
    assert(dir[0] == '\0');

    setenv("HOME", "/home/u", 1);
    printf("PASS: state dir construction\n");
}

static void test_rm_rf(void) {
    char tmpl[] = "/tmp/podvm-test-container.XXXXXX";
    char *base = mkdtemp(tmpl);
    assert(base != NULL);

    char p[CONTAINER_PATH_MAX];

    /* A tree shaped like real state: u/ with nested files, plus w/ */
    snprintf(p, sizeof(p), "%s/u", base);           assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/u/nested", base);    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/u/nested/file", base);
    FILE *f = fopen(p, "w");
    assert(f != NULL);
    fputs("x", f);
    fclose(f);
    snprintf(p, sizeof(p), "%s/w", base);           assert(mkdir(p, 0755) == 0);

    /* A symlink pointing outside the tree: must be removed, not
     * followed -- otherwise rm -rf could escape the state dir. */
    snprintf(p, sizeof(p), "%s/u/escape", base);
    assert(symlink("/etc", p) == 0);

    /* The overlayfs work dir, mode 000 (that is how the kernel leaves
     * it). nftw cannot opendir it, so without a chmod pre-pass this
     * aborted the delete with EISDIR -- found by e2e_rm.sh first. */
    snprintf(p, sizeof(p), "%s/w/work", base);   assert(mkdir(p, 000) == 0);

    assert(container_rm_rf(base) == 0);
    assert(access(base, F_OK) != 0);       /* the tree is gone */
    assert(access("/etc", F_OK) == 0);     /* the symlink target is not */

    /* Deleting what does not exist fails with ENOENT */
    assert(container_rm_rf(base) == -1);
    assert(errno == ENOENT);

    printf("PASS: rm -rf (nested tree, mode-000 dir, symlink not followed, ENOENT)\n");
}

int main(void) {
    printf("Running container state tests...\n\n");

    test_name_valid();
    test_state_dir();
    test_rm_rf();

    printf("\nAll tests passed!\n");
    return 0;
}
