/*
 * test_store_native.c - podvm-native layered store
 *
 * Links the real src/store_native.c (+ tar.c, sha256.c, json.c, image.c for
 * image_parse/config). Proves: content-addressed commit, dedup (same diff
 * -> same digest), manifest write, chain ORDERING (top-first, the reverse
 * of the manifest's bottom-first list), and config load.
 *
 * Build: gcc -o test_store_native tests/test_store_native.c \
 *          src/store_native.c src/tar.c src/sha256.c src/json.c \
 *          src/image.c src/strparse.c -Isrc -lcurl -lz -lzstd
 * Run: PODVM_STORE_DIR=<tmp> ./test_store_native
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "store_native.h"
#include "image.h"

static void mk(const char *p) { assert(mkdir(p, 0755) == 0); }
static void wf(const char *p, const char *content) {
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

int main(void) {
    char tmpl[] = "/tmp/podvm-storenat-XXXXXX";
    char *T = mkdtemp(tmpl);
    assert(T);
    setenv("PODVM_STORE_DIR", T, 1);

    /* Two diff dirs (base and one layer). */
    char d0[512], d1[512], p[512];
    snprintf(d0, sizeof(d0), "%s/d0", T);
    snprintf(d1, sizeof(d1), "%s/d1", T);
    mk(d0);
    mk(d1);
    snprintf(p, sizeof(p), "%s/a.txt", d0);
    wf(p, "A\n");
    snprintf(p, sizeof(p), "%s/b.txt", d1);
    wf(p, "B\n");

    char dig0[NATIVE_DIGEST_MAX], dig1[NATIVE_DIGEST_MAX], dig0b[NATIVE_DIGEST_MAX];
    assert(native_commit_diff(d0, dig0, sizeof(dig0)) == 0);
    assert(native_commit_diff(d1, dig1, sizeof(dig1)) == 0);
    assert(strncmp(dig0, "sha256:", 7) == 0);
    assert(strncmp(dig1, "sha256:", 7) == 0);
    assert(strcmp(dig0, dig1) != 0);

    /* Dedup: recommitting d0 yields the same digest (content-addressed). */
    assert(native_commit_diff(d0, dig0b, sizeof(dig0b)) == 0);
    assert(strcmp(dig0, dig0b) == 0);

    /* Plant a config blob (OCI image config shape). */
    const char *cfg =
        "{\"architecture\":\"amd64\",\"os\":\"linux\",\"config\":"
        "{\"Cmd\":[\"/bin/echo\",\"hi\"],\"Env\":[\"A=B\"],\"WorkingDir\":\"/w\"}}";
    char cfgd[NATIVE_DIGEST_MAX];
    assert(native_blob_put(cfg, strlen(cfg), cfgd, sizeof(cfgd)) == 0);
    assert(strncmp(cfgd, "sha256:", 7) == 0);

    /* Manifest: layers bottom-first (base=d0, top=d1). */
    const char *layers[2] = { dig0, dig1 };
    assert(native_manifest_put("test/img:latest", cfgd, layers, 2, NULL, 0) == 0);
    assert(native_has("test/img:latest") == 0);
    assert(native_has("test/other:latest") != 0);

    /* Chain is TOP-first: index 0 must be the top layer (d1). */
    NativeLayer L[4];
    int n = native_chain("test/img:latest", L, 4);
    assert(n == 2);
    assert(strcmp(L[0].digest, dig1) == 0);
    assert(strcmp(L[1].digest, dig0) == 0);
    struct stat st;
    assert(stat(L[0].diff, &st) == 0 && S_ISDIR(st.st_mode));
    assert(stat(L[1].diff, &st) == 0 && S_ISDIR(st.st_mode));

    /* Config round-trip. */
    ImageConfig c;
    image_config_init(&c);
    assert(native_config("test/img:latest", &c) == 0);
    assert(c.ncmd == 2);
    assert(strcmp(c.cmd[0], "/bin/echo") == 0);
    assert(strcmp(c.cmd[1], "hi") == 0);
    assert(c.nenv == 1 && strcmp(c.env[0], "A=B") == 0);
    assert(strcmp(c.cwd, "/w") == 0);
    image_config_free(&c);

    printf("PASS: native store (commit, dedup, manifest, chain order, config)\n");
    return 0;
}
