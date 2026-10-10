/*
 * store_native.c - podvm's own layered, content-addressable image store.
 * See store_native.h and ADR-027.
 */
#define _GNU_SOURCE
#include "store_native.h"
#include "tar.h"
#include "sha256.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int mkdir_p(const char *path, int mode) {
    char tmp[NATIVE_PATH_MAX];
    char *p;
    if (strlen(path) >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

const char *native_store_dir(void) {
    const char *d = getenv("PODVM_STORE_DIR");
    if (d && *d) return d;
    const char *xdg = getenv("XDG_DATA_HOME");
    static char buf[NATIVE_PATH_MAX];
    if (xdg && *xdg)
        snprintf(buf, sizeof(buf), "%s/podvm/store", xdg);
    else
        snprintf(buf, sizeof(buf), "%s/.local/share/podvm/store",
                 getenv("HOME") ? getenv("HOME") : "");
    return buf;
}

/* Manifest path: <store>/manifests/<name with '/'->'+'>/<tag>. The ref is
 * used verbatim (writer and reader agree); full registry normalization is
 * a later concern. */
static int ref_path(const char *image_str, char *out, size_t n) {
    ImageRef ref;
    if (image_parse(image_str, &ref) < 0) return -1;
    char mangled[MAX_IMAGE_NAME];
    size_t i = 0;
    for (; ref.name[i] && i + 1 < sizeof(mangled); i++)
        mangled[i] = (ref.name[i] == '/') ? '+' : ref.name[i];
    mangled[i] = '\0';
    if (snprintf(out, n, "%s/manifests/%s/%s", native_store_dir(), mangled, ref.tag)
        >= (int)n)
        return -1;
    return 0;
}

static int read_file(const char *path, char **out, size_t *outlen) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) < 0) { fclose(f); return -1; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return -1; }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);
    *out = buf;
    if (outlen) *outlen = got;
    return 0;
}

static void blob_path(const char *hex, char *out, size_t n) {
    snprintf(out, n, "%s/blobs/sha256/%s", native_store_dir(), hex);
}

static void layer_diff_path(const char *hex, char *out, size_t n) {
    snprintf(out, n, "%s/layers/%s/diff", native_store_dir(), hex);
}

int native_blob_put(const char *data, size_t len, char *out_digest, size_t n) {
    char hex[65];
    sha256_hex(data, len, hex);
    if (snprintf(out_digest, n, "sha256:%s", hex) >= (int)n) return -1;

    char dir[NATIVE_PATH_MAX], path[NATIVE_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/blobs/sha256", native_store_dir());
    if (mkdir_p(dir, 0755) < 0) return -1;
    blob_path(hex, path, sizeof(path));
    if (access(path, F_OK) == 0) return 0;   /* dedup: already stored */

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        p += w; left -= (size_t)w;
    }
    close(fd);
    return 0;
}

int native_commit_diff(const char *diff_dir, char *out_digest, size_t n) {
    char tmpdir[NATIVE_PATH_MAX], tmptar[NATIVE_PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", native_store_dir());
    if (mkdir_p(tmpdir, 0755) < 0) return -1;
    snprintf(tmptar, sizeof(tmptar), "%s/commit-%d.tar", tmpdir, (int)getpid());
    if (tar_pack_dir(diff_dir, tmptar) < 0) return -1;

    char *data = NULL;
    size_t len = 0;
    if (read_file(tmptar, &data, &len) < 0) { unlink(tmptar); return -1; }
    unlink(tmptar);

    if (native_blob_put(data, len, out_digest, n) < 0) { free(data); return -1; }

    /* Unpack to layers/<hex>/diff unless already present (dedup). */
    const char *hex = out_digest + 7;   /* skip "sha256:" */
    char diff[NATIVE_PATH_MAX];
    layer_diff_path(hex, diff, sizeof(diff));
    struct stat st;
    if (stat(diff, &st) != 0) {
        if (mkdir_p(diff, 0755) < 0) { free(data); return -1; }
        if (tar_extract_mem(data, len, diff) < 0) { free(data); return -1; }
    }
    free(data);
    return 0;
}

int native_has(const char *image_str) {
    char path[NATIVE_PATH_MAX];
    struct stat st;
    if (ref_path(image_str, path, sizeof(path)) < 0) return -1;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : -1;
}

/* Parse a manifest; returns a json node the caller frees, or NULL. */
static JNode *manifest_load(const char *image_str) {
    char path[NATIVE_PATH_MAX];
    if (ref_path(image_str, path, sizeof(path)) < 0) return NULL;
    char *text = NULL;
    if (read_file(path, &text, NULL) < 0) return NULL;
    JNode *m = json_parse(text);
    free(text);
    return m;
}

int native_chain(const char *image_str, NativeLayer *layers, int max) {
    JNode *m = manifest_load(image_str);
    if (!m) return -1;
    JNode *arr = json_get(m, "layers");
    if (!arr || arr->type != JARR) { json_free(m); return -1; }
    int n = (int)arr->n;
    if (n > max) n = max;
    /* Manifest is bottom-first; overlay lowerdir is top-first: reverse. */
    for (int i = 0; i < n; i++) {
        const char *d = json_str(json_at(arr, (size_t)(n - 1 - i)));
        if (!d || strncmp(d, "sha256:", 7) != 0) {
            layers[i].digest[0] = '\0';
            layers[i].diff[0] = '\0';
            continue;
        }
        snprintf(layers[i].digest, sizeof(layers[i].digest), "%s", d);
        layer_diff_path(d + 7, layers[i].diff, sizeof(layers[i].diff));
    }
    json_free(m);
    return n;
}

int native_config(const char *image_str, ImageConfig *c) {
    JNode *m = manifest_load(image_str);
    if (!m) return -1;
    const char *cfgdigest = json_str(json_get(m, "config"));
    char hex[NATIVE_DIGEST_MAX];
    snprintf(hex, sizeof(hex), "%s", cfgdigest ? cfgdigest + 7 : "");
    json_free(m);
    if (!cfgdigest) return -1;

    char path[NATIVE_PATH_MAX];
    blob_path(hex, path, sizeof(path));
    char *text = NULL;
    if (read_file(path, &text, NULL) < 0) return -1;
    int rc = image_config_parse(text, c);
    free(text);
    return rc;
}

int native_manifest_put(const char *image_str, const char *config_digest,
                        const char *const *layer_digests, int nlayers,
                        const char *const *history, int nhist) {
    char path[NATIVE_PATH_MAX];
    if (ref_path(image_str, path, sizeof(path)) < 0) return -1;

    char dir[NATIVE_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    if (mkdir_p(dir, 0755) < 0) return -1;

    ImageRef ref;
    if (image_parse(image_str, &ref) < 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "{\"schemaVersion\":1,\"name\":\"%s\",\"tag\":\"%s\",",
            ref.name, ref.tag);
    fprintf(f, "\"config\":\"%s\",", config_digest ? config_digest : "");
    fprintf(f, "\"layers\":[");
    for (int i = 0; i < nlayers; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", layer_digests[i]);
    fprintf(f, "]");
    if (nhist > 0) {
        fprintf(f, ",\"history\":[");
        for (int i = 0; i < nhist; i++)
            fprintf(f, "%s\"%s\"", i ? "," : "", history[i]);
        fprintf(f, "]");
    }
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}
