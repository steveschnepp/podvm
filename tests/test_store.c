/*
 * test_store.c - unit tests for src/store.c (podman storage reader)
 *
 * store.c reads podman's on-disk layout through XDG_DATA_HOME, so a
 * temp fixture tree exercises it hermetically -- no podman, no VM, no
 * network. This is the coverage the native-pull e2e can't give: it
 * needs podman and a real store, and it skips when either is absent.
 *
 * Two regressions are pinned here on purpose:
 *   1. the localhost/<name>:<tag> candidate (a locally-built image
 *      referenced as "myimg:v1" used to fall through to a registry pull)
 *   2. the big-data base64 filename encoder (a stride past the NUL once
 *      produced a spurious "GA==" suffix and every config lookup missed)
 *
 * Build: gcc -o test_store tests/test_store.c src/store.c src/image.c \
 *            src/json.c src/sha256.c -Isrc -lcurl -lz -lzstd
 * Run: ./test_store
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "store.h"
#include "image.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* 64-hex ids, as containers/storage uses. */
#define IMG_PULLED  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define IMG_LOCAL   "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define IMG_BROKEN  "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define CFG_TARGET  "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

static char work[256];

/* Independent base64 (RFC 4648, padded) -- deliberately a different
 * implementation than store.c's, so the fixture filename it builds
 * cross-checks store.c's encoder instead of echoing it. */
static void ref_b64(const char *in, char *out) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in), o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < len) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? t[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? t[v & 63] : '=';
    }
    out[o] = '\0';
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static char *storage(void) {
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s/data/containers/storage", work);
    return buf;
}

/* Write a "big data" entry: overlay-images/<imgid>/=<base64(name)>. */
static void put_bigdata(const char *imgid, const char *name, const char *content) {
    char dir[512], b64[512], path[1024];
    snprintf(dir, sizeof(dir), "%s/overlay-images/%s", storage(), imgid);
    mkdir(dir, 0755);
    ref_b64(name, b64);
    snprintf(path, sizeof(path), "%s/=%s", dir, b64);
    write_file(path, content);
}

static void make_fixture(void) {
    char path[1024];

    /* data/ exists (made by main); the containers/storage chain does not. */
    snprintf(path, sizeof(path), "%s/data/containers", work);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/data/containers/storage", work);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/overlay-images", storage());
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/overlay-layers", storage());
    mkdir(path, 0755);

    /* images.json: one pulled image, one locally-built (localhost/),
     * one with no layer (must not match), one whose config must be
     * found via the manifest fallback. */
    snprintf(path, sizeof(path), "%s/overlay-images/images.json", storage());
    write_file(path,
        "["
        "{\"id\":\"" IMG_PULLED "\","
         "\"names\":[\"docker.io/library/alpine:latest\"],"
         "\"layer\":\"layer-top\"},"
        "{\"id\":\"" IMG_LOCAL "\","
         "\"names\":[\"localhost/myimg:v1\"],"
         "\"layer\":\"layer-top\"},"
        "{\"id\":\"" IMG_BROKEN "\","
         "\"names\":[\"docker.io/library/noconfig:latest\"]},"
        "{\"id\":\"" CFG_TARGET "\","
         "\"names\":[\"docker.io/library/manifest-only:latest\"],"
         "\"layer\":\"layer-top\"}"
        "]");

    snprintf(path, sizeof(path), "%s/overlay-layers/layers.json", storage());
    write_file(path,
        "["
        "{\"id\":\"layer-top\",\"parent\":\"layer-mid\"},"
        "{\"id\":\"layer-mid\",\"parent\":\"layer-base\"},"
        "{\"id\":\"layer-base\"},"
        "{\"id\":\"layer-broken\",\"parent\":\"layer-ghost\"}"
        "]");

    /* Config blob for the pulled image: stored under the id itself. */
    put_bigdata(IMG_PULLED, "sha256:" IMG_PULLED,
        "{\"config\":{\"Entrypoint\":[\"/bin/sh\"],"
        "\"Env\":[\"PATH=/usr/bin\"],\"WorkingDir\":\"/app\"}}");

    /* manifest-only image: no sha256:<id> blob; a manifest entry points
     * at a differently-named config blob. */
    put_bigdata(CFG_TARGET, "manifest",
        "{\"config\":{\"digest\":\"sha256:" CFG_TARGET "\"}}");
    put_bigdata(CFG_TARGET, "sha256:" CFG_TARGET,
        "{\"config\":{\"Cmd\":[\"/bin/echo\",\"hi\"],\"User\":\"nobody\"}}");
}

static void test_find(void) {
    char layer[STORE_PATH_MAX];

    /* Unqualified name resolves through the docker.io/library candidate. */
    CHECK(store_find("alpine", layer, sizeof(layer)) == 0);
    CHECK(strcmp(layer, "layer-top") == 0);

    /* Explicit tag and fully-qualified forms resolve too. */
    CHECK(store_find("alpine:latest", layer, sizeof(layer)) == 0);
    CHECK(store_find("docker.io/library/alpine:latest", layer, sizeof(layer)) == 0);

    /* Regression: a locally-built image stored as localhost/<name>:<tag>
     * must match the short "name:tag" form. */
    CHECK(store_find("myimg:v1", layer, sizeof(layer)) == 0);
    CHECK(strcmp(layer, "layer-top") == 0);

    /* Missing image, and an image with no layer (unusable, must not match). */
    CHECK(store_find("nosuch", layer, sizeof(layer)) == -1);
    CHECK(store_find("noconfig", layer, sizeof(layer)) == -1);
}

static void test_chain(void) {
    StoreLayer layers[8];

    /* Top-first order = overlayfs lowerdir order. */
    int n = store_chain("layer-top", layers, 8);
    CHECK(n == 3);
    CHECK(strstr(layers[0].diff, "/overlay/layer-top/diff") != NULL);
    CHECK(strstr(layers[1].diff, "/overlay/layer-mid/diff") != NULL);
    CHECK(strstr(layers[2].diff, "/overlay/layer-base/diff") != NULL);

    /* A dangling parent truncates the chain instead of failing it. */
    CHECK(store_chain("layer-broken", layers, 8) == 1);

    /* Unknown top layer is an error. */
    CHECK(store_chain("layer-ghost", layers, 8) == -1);
}

static void test_image_config(void) {
    ImageConfig cfg;

    /* Primary path: the config blob stored under the image id. */
    image_config_init(&cfg);
    CHECK(store_image_config("alpine", &cfg) == 0);
    CHECK(cfg.nentry == 1 && strcmp(cfg.entrypoint[0], "/bin/sh") == 0);
    CHECK(cfg.nenv == 1 && strcmp(cfg.env[0], "PATH=/usr/bin") == 0);
    CHECK(strcmp(cfg.cwd, "/app") == 0);
    image_config_free(&cfg);

    /* Fallback path: manifest -> config.digest -> the real blob. */
    image_config_init(&cfg);
    CHECK(store_image_config("manifest-only:latest", &cfg) == 0);
    CHECK(cfg.ncmd == 2 && strcmp(cfg.cmd[0], "/bin/echo") == 0 &&
          strcmp(cfg.cmd[1], "hi") == 0);
    CHECK(strcmp(cfg.user, "nobody") == 0);
    image_config_free(&cfg);

    /* Image resolves but no config blob is readable. */
    image_config_init(&cfg);
    CHECK(store_image_config("noconfig:latest", &cfg) == -1);
    image_config_free(&cfg);

    /* Unknown image never reaches the blob stage. */
    image_config_init(&cfg);
    CHECK(store_image_config("nosuch", &cfg) == -1);
    image_config_free(&cfg);
}

static void test_no_storage(void) {
    char layer[STORE_PATH_MAX];
    StoreLayer layers[4];
    /* Point at a nonexistent store; every lookup must fail cleanly. */
    setenv("XDG_DATA_HOME", "/nonexistent-podvm-store", 1);
    CHECK(store_find("alpine", layer, sizeof(layer)) == -1);
    CHECK(store_chain("layer-top", layers, 4) == -1);
    {   /* restore the fixture value for any later case */
        static char restore[512];
        snprintf(restore, sizeof(restore), "%s/data", work);
        setenv("XDG_DATA_HOME", restore, 1);
    }
}

int main(void) {
    char tmpl[] = "/tmp/podvm-test-store.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(work, sizeof(work), "%s", tmpl);

    char data[512], home[512];
    snprintf(data, sizeof(data), "%s/data", work);
    mkdir(data, 0755);
    snprintf(home, sizeof(home), "%s/home", work);
    mkdir(home, 0755);
    setenv("XDG_DATA_HOME", data, 1);
    setenv("HOME", home, 1);   /* keep any fallback off the real ~/.local */

    make_fixture();
    test_find();
    test_chain();
    test_image_config();
    test_no_storage();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", work);
    if (system(cmd) != 0) fprintf(stderr, "warning: cleanup failed for %s\n", work);

    if (failures) {
        printf("store tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("store tests: all passed\n");
    return 0;
}
