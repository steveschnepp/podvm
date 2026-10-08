/*
 * test_image.c - unit tests for the OCI image pipeline.
 *
 * Includes ../src/image.c directly so the static helpers (json_parse,
 * gunzip_mem, untar_mem, reg_parse) are reachable without a network.
 *
 * Build: gcc -Wall -Wextra -O2 -g -o test_image test_image.c -I../src -lcurl -lz
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>

#include "../src/image.c"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* ── JSON ─────────────────────────────────────────────── */
static void test_json(void) {
    const char *s =
        "{\"schemaVersion\":2,\"config\":{\"digest\":\"sha256:abc\"},"
        "\"layers\":[{\"digest\":\"sha256:one\",\"size\":10},"
        "{\"digest\":\"sha256:two\"}],"
        "\"esc\":\"line\\nbreak \\u0041\",\"pi\":3.14,\"ok\":true,\"none\":null}";
    JNode *j = json_parse(s);
    CHECK(j != NULL);
    CHECK(j && j->type == JOBJ);

    JNode *sv = json_get(j, "schemaVersion");
    CHECK(sv && sv->type == JNUM && strcmp(sv->str, "2") == 0);

    JNode *layers = json_get(j, "layers");
    CHECK(layers && layers->type == JARR && layers->n == 2);
    JNode *l0 = json_at(layers, 0);
    CHECK(l0 && strcmp(json_str(json_get(l0, "digest")), "sha256:one") == 0);

    JNode *esc = json_get(j, "esc");
    CHECK(esc && strcmp(esc->str, "line\nbreak A") == 0);

    JNode *ok = json_get(j, "ok");
    CHECK(ok && ok->type == JBOOL && ok->bval == 1);

    JNode *none = json_get(j, "none");
    CHECK(none && none->type == JNULL);

    json_free(j);
}

/* ── gzip round-trip ──────────────────────────────────── */
static void test_gzip(void) {
    const char *plain = "the quick brown fox jumps over the lazy dog. "
                        "pack my box with five dozen liquor jugs.";
    size_t plain_len = strlen(plain);

    /* deflate -> gzip */
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    CHECK(deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                       Z_DEFAULT_STRATEGY) == Z_OK);

    unsigned char comp[4096];
    strm.next_in = (Bytef *)plain;
    strm.avail_in = (uInt)plain_len;
    strm.next_out = comp;
    strm.avail_out = sizeof(comp);
    CHECK(deflate(&strm, Z_FINISH) == Z_STREAM_END);
    size_t comp_len = strm.total_out;
    deflateEnd(&strm);

    /* gunzip_mem -> compare */
    Buf out;
    buf_init(&out);
    CHECK(gunzip_mem((char *)comp, comp_len, &out) == 0);
    CHECK(out.len == plain_len);
    CHECK(out.data && memcmp(out.data, plain, plain_len) == 0);
    buf_free(&out);
}

/* ── tar construction + extraction ────────────────────── */
static void tar_header(char *h, const char *name, size_t size, char type) {
    memset(h, 0, 512);
    strncpy(h + 0, name, 99);
    snprintf(h + 100, 8, "%07o", 0644);
    snprintf(h + 108, 8, "%07o", 0);
    snprintf(h + 116, 8, "%07o", 0);
    snprintf(h + 124, 12, "%011o", (unsigned)size);
    snprintf(h + 136, 12, "%011o", 0);
    memset(h + 148, ' ', 8);          /* checksum placeholder */
    h[156] = type;
    memcpy(h + 257, "ustar", 5);
    h[262] = '\0';
    h[263] = '0'; h[264] = '0';

    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)h[i];
    snprintf(h + 148, 8, "%06o", sum);
    h[154] = '\0';
    h[155] = ' ';
}

static void test_untar(void) {
    /* Build a tar in memory: one regular file + one dir */
    char tar[2048];
    memset(tar, 0, sizeof(tar));

    const char *content = "hello podvm\n";
    size_t clen = strlen(content);

    /* file: etc/motd */
    tar_header(tar + 0, "etc/motd", clen, '0');
    memcpy(tar + 512, content, clen);

    /* dir: var */
    tar_header(tar + 1024, "var", 0, '5');

    /* EOF: two zero blocks already zeroed */
    size_t tarlen = 1536;

    char tmpl[] = "/tmp/podvm_untar_XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);

    CHECK(untar_mem(tar, tarlen, dir) == 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/etc/motd", dir);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) {
        char buf[64] = {0};
        size_t r = fread(buf, 1, sizeof(buf) - 1, f);
        CHECK(r == clen && memcmp(buf, content, clen) == 0);
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/var", dir);
    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISDIR(st.st_mode));

    /* cleanup */
    snprintf(path, sizeof(path), "%s/etc/motd", dir); remove(path);
    snprintf(path, sizeof(path), "%s/etc", dir); rmdir(path);
    snprintf(path, sizeof(path), "%s/var", dir); rmdir(path);
    rmdir(dir);
}

/* ── reference parsing ────────────────────────────────── */
static void test_image_parse(void) {
    ImageRef ref;

    image_parse("debian", &ref);
    CHECK(strcmp(ref.name, "debian") == 0 && strcmp(ref.tag, "latest") == 0);

    image_parse("debian:12", &ref);
    CHECK(strcmp(ref.name, "debian") == 0 && strcmp(ref.tag, "12") == 0);

    image_parse("registry.example.com:5000/foo/bar:v1", &ref);
    CHECK(strcmp(ref.name, "registry.example.com:5000/foo/bar") == 0);
    CHECK(strcmp(ref.tag, "v1") == 0);

    /* host:port with no tag -> whole thing is the name */
    image_parse("registry.example.com:5000/foo/bar", &ref);
    CHECK(strcmp(ref.name, "registry.example.com:5000/foo/bar") == 0);
    CHECK(strcmp(ref.tag, "latest") == 0);
}

static void test_reg_parse(void) {
    RegRef rr;

    reg_parse("debian", "latest", &rr);
    CHECK(strcmp(rr.registry, "docker.io") == 0);
    CHECK(strcmp(rr.repo, "library/debian") == 0);
    CHECK(strcmp(rr.tag, "latest") == 0);

    reg_parse("myuser/app", NULL, &rr);
    CHECK(strcmp(rr.registry, "docker.io") == 0);
    CHECK(strcmp(rr.repo, "myuser/app") == 0);
    CHECK(strcmp(rr.tag, "latest") == 0);

    reg_parse("registry.example.com:5000/foo/bar", "v2", &rr);
    CHECK(strcmp(rr.registry, "registry.example.com:5000") == 0);
    CHECK(strcmp(rr.repo, "foo/bar") == 0);
    CHECK(strcmp(rr.tag, "v2") == 0);

    /* docker.io endpoint mapping */
    CHECK(strcmp(registry_host("docker.io"), "registry-1.docker.io") == 0);
    CHECK(strcmp(registry_host("quay.io"), "quay.io") == 0);
}

static void test_is_path(void) {
    CHECK(image_is_path("/abs/path") == 1);
    CHECK(image_is_path("./rel") == 1);
    CHECK(image_is_path("debian") == 0);
    CHECK(image_is_path("myuser/app") == 0);
}

/* ── image config blob parsing ─────────────────────── */
static void test_config_parse(void) {
    /* Real Debian bookworm config blob (as stored by podman). */
    const char *debian =
        "{\"config\":{\"Env\":[\"PATH=/usr/local/sbin:/usr/local/bin\"],"
        "\"Entrypoint\":[],\"Cmd\":[\"bash\"]},"
        "\"created\":\"2026-08-24T00:00:00Z\","
        "\"rootfs\":{\"type\":\"layers\"},\"os\":\"linux\","
        "\"architecture\":\"amd64\"}";
    ImageConfig c;
    image_config_init(&c);
    CHECK(image_config_parse(debian, &c) == 0);
    CHECK(c.nenv == 1 && strcmp(c.env[0], "PATH=/usr/local/sbin:/usr/local/bin") == 0);
    CHECK(c.nentry == 0);              /* explicit empty Entrypoint */
    CHECK(c.ncmd == 1 && strcmp(c.cmd[0], "bash") == 0);
    CHECK(c.cwd[0] == '\0');            /* WorkingDir absent */
    CHECK(c.user[0] == '\0');
    image_config_free(&c);

    /* Full config: entrypoint + cmd + workingdir + user. */
    const char *full =
        "{\"config\":{\"Env\":[\"A=1\",\"B=2\"],"
        "\"Entrypoint\":[\"/entry.sh\"],\"Cmd\":[\"serve\",\"--port\",\"80\"],"
        "\"WorkingDir\":\"/app\",\"User\":\"1000:1000\"}}";
    image_config_init(&c);
    CHECK(image_config_parse(full, &c) == 0);
    CHECK(c.nenv == 2 && strcmp(c.env[1], "B=2") == 0);
    CHECK(c.nentry == 1 && strcmp(c.entrypoint[0], "/entry.sh") == 0);
    CHECK(c.ncmd == 3 && strcmp(c.cmd[2], "80") == 0);
    CHECK(strcmp(c.cwd, "/app") == 0);
    CHECK(strcmp(c.user, "1000:1000") == 0);
    image_config_free(&c);

    /* A manifest (no "config" object) is rejected, not crashed on. */
    const char *manifest =
        "{\"schemaVersion\":2,\"layers\":[{\"digest\":\"sha256:x\"}]}";
    image_config_init(&c);
    CHECK(image_config_parse(manifest, &c) == -1);
    image_config_free(&c);

    /* Garbage / empty input rejected. */
    image_config_init(&c);
    CHECK(image_config_parse("not json", &c) == -1);
    CHECK(image_config_parse("", &c) == -1);
    CHECK(image_config_parse(NULL, &c) == -1);
    image_config_free(&c);

    /* Double free is safe (free zeroes the struct). */
    image_config_init(&c);
    image_config_parse(full, &c);
    image_config_free(&c);
    image_config_free(&c);
}

int main(void) {
    test_json();
    test_gzip();
    test_untar();
    test_image_parse();
    test_reg_parse();
    test_is_path();
    test_config_parse();

    if (failures == 0) {
        printf("image tests: all passed\n");
        return 0;
    }
    printf("image tests: %d failure(s)\n", failures);
    return 1;
}
