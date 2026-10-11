/*
 * store_native.c - podvm's own layered, content-addressable image store.
 * See store_native.h and ADR-027.
 */
#define _GNU_SOURCE
#include "store_native.h"
#include "tar.h"
#include "sha256.h"
#include "json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOAD_MAX 128   /* max layers a podvm load will ingest */

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

/* Canonical storage/display name: drop the docker hub registry (and its
 * aliases) and the implied "library/" prefix, like docker; a non-hub
 * registry is kept. So "docker.io/library/alpine", "alpine", and
 * "alpine:latest" all map to one manifest. */
static void normalize_ref_name(const char *name, char *out, size_t n) {
    const char *slash = strchr(name, '/');
    const char *repo = name;
    char registry[128];
    registry[0] = '\0';
    int hub = 1;
    if (slash) {
        size_t rl = (size_t)(slash - name);
        if (rl >= sizeof(registry)) rl = sizeof(registry) - 1;
        memcpy(registry, name, rl);
        registry[rl] = '\0';
        if (strchr(registry, '.') || strchr(registry, ':') ||
            !strcmp(registry, "localhost")) {
            repo = slash + 1;
            hub = !strcmp(registry, "docker.io") ||
                  !strcmp(registry, "registry-1.docker.io") ||
                  !strcmp(registry, "index.docker.io");
        }
    }
    if (hub) {
        if (!strncmp(repo, "library/", 8)) repo += 8;
        snprintf(out, n, "%s", repo);
    } else {
        snprintf(out, n, "%s/%s", registry, repo);
    }
}

/* Manifest path: <store>/manifests/<normalized-name with '/'->'+'>/<tag>.
 * Normalization makes "alpine" and "docker.io/library/alpine:latest" the
 * same image. */
static int ref_path(const char *image_str, char *out, size_t n) {
    ImageRef ref;
    if (image_parse(image_str, &ref) < 0) return -1;
    char norm[MAX_IMAGE_NAME];
    normalize_ref_name(ref.name, norm, sizeof(norm));
    char mangled[MAX_IMAGE_NAME];
    size_t i = 0;
    for (; norm[i] && i + 1 < sizeof(mangled); i++)
        mangled[i] = (norm[i] == '/') ? '+' : norm[i];
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

/* Unpack a layer tar into root inside a short-lived user+mount ns, where
 * we are uid 0 so mknod(char 0:0) whiteouts succeed -- the same trick
 * rootless podman uses. The tar data pointer is inherited across fork. */
static int unpack_layer_in_userns(const char *data, size_t len, const char *root) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0) _exit(1);
        int fd = open("/proc/self/setgroups", O_WRONLY);
        if (fd >= 0) { (void)!write(fd, "deny", 4); close(fd); }
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)uid);
        fd = open("/proc/self/uid_map", O_WRONLY);
        if (fd < 0 || write(fd, buf, (size_t)n) != n) _exit(1);
        close(fd);
        n = snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)gid);
        fd = open("/proc/self/gid_map", O_WRONLY);
        if (fd < 0 || write(fd, buf, (size_t)n) != n) _exit(1);
        close(fd);
        _exit(tar_extract_layer(data, len, root) == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* Store layer bytes as a blob + unpack to layers/<hex>/diff (dedup by
 * digest). out_digest = "sha256:<hex>". Shared by commit and pull. */
static int store_layer_bytes(const char *data, size_t len, char *out_digest, size_t n) {
    if (native_blob_put(data, len, out_digest, n) < 0) return -1;
    const char *hex = out_digest + 7;   /* skip "sha256:" */
    char diff[NATIVE_PATH_MAX];
    layer_diff_path(hex, diff, sizeof(diff));
    struct stat st;
    if (stat(diff, &st) != 0) {
        if (mkdir_p(diff, 0755) < 0) return -1;
        if (unpack_layer_in_userns(data, len, diff) < 0) return -1;
    }
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

    int rc = store_layer_bytes(data, len, out_digest, n);
    free(data);
    return rc;
}

int native_put_layer(const char *tardata, size_t len, char *out_digest, size_t n) {
    return store_layer_bytes(tardata, len, out_digest, n);
}

void native_diff_path(const char *digest, char *out, size_t n) {
    const char *hex = (digest && !strncmp(digest, "sha256:", 7)) ? digest + 7 : "";
    layer_diff_path(hex, out, n);
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
    char norm[MAX_IMAGE_NAME];
    normalize_ref_name(ref.name, norm, sizeof(norm));

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "{\"schemaVersion\":1,\"name\":\"%s\",\"tag\":\"%s\",",
            norm, ref.tag);
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

int native_list(NativeImage *ents, int max) {
    char mroot[NATIVE_PATH_MAX];
    snprintf(mroot, sizeof(mroot), "%s/manifests", native_store_dir());
    DIR *d = opendir(mroot);
    if (!d) return 0;
    int count = 0;
    struct dirent *nd;
    while ((nd = readdir(d)) != NULL && count < max) {
        if (nd->d_name[0] == '.') continue;
        char namedir[NATIVE_PATH_MAX];
        snprintf(namedir, sizeof(namedir), "%s/%s", mroot, nd->d_name);
        struct stat dst;
        if (stat(namedir, &dst) != 0 || !S_ISDIR(dst.st_mode)) continue;
        DIR *d2 = opendir(namedir);
        if (!d2) continue;
        struct dirent *te;
        while ((te = readdir(d2)) != NULL && count < max) {
            if (te->d_name[0] == '.') continue;
            char mpath[NATIVE_PATH_MAX];
            snprintf(mpath, sizeof(mpath), "%s/%s", namedir, te->d_name);
            char *text = NULL;
            if (read_file(mpath, &text, NULL) < 0) continue;
            JNode *m = json_parse(text);
            free(text);
            if (!m) continue;
            const char *nm = json_str(json_get(m, "name"));
            const char *tg = json_str(json_get(m, "tag"));
            snprintf(ents[count].name, sizeof(ents[count].name), "%s", nm ? nm : "");
            snprintf(ents[count].tag, sizeof(ents[count].tag), "%s", tg ? tg : "");
            JNode *arr = json_get(m, "layers");
            int nl = (arr && arr->type == JARR) ? (int)arr->n : 0;
            ents[count].nlayers = nl;
            unsigned long long sz = 0;
            for (int i = 0; i < nl; i++) {
                const char *dg = json_str(json_at(arr, (size_t)i));
                if (!dg || strncmp(dg, "sha256:", 7) != 0) continue;
                char bp[NATIVE_PATH_MAX];
                blob_path(dg + 7, bp, sizeof(bp));
                struct stat bs;
                if (stat(bp, &bs) == 0) sz += (unsigned long long)bs.st_size;
            }
            ents[count].size = sz;
            json_free(m);
            count++;
        }
        closedir(d2);
    }
    closedir(d);
    return count;
}

int native_rmi(const char *image_str) {
    char path[NATIVE_PATH_MAX];
    if (ref_path(image_str, path, sizeof(path)) < 0) return -1;
    if (unlink(path) < 0) return -1;
    return 0;
}

/* Hardlink src to dst (same fs), else copy. */
static int link_or_copy(const char *src, const char *dst) {
    if (link(src, dst) == 0) return 0;
    char *data = NULL;
    size_t len = 0;
    if (read_file(src, &data, &len) < 0) return -1;
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(data); return -1; }
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; close(fd); free(data); return -1; }
        p += w; left -= (size_t)w;
    }
    close(fd);
    free(data);
    return 0;
}

/* Recursive delete (for save/load staging dirs under <store>/tmp). */
static void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char p[NATIVE_PATH_MAX];
                snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
                rm_rf(p);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

int native_save(const char *image_str, const char *out_path, int format) {
    if (format != 0) {
        fprintf(stderr, "podvm save: only docker-archive format is supported\n");
        return -1;
    }
    JNode *m = manifest_load(image_str);
    if (!m) { fprintf(stderr, "podvm save: no such image: %s\n", image_str); return -1; }
    const char *name = json_str(json_get(m, "name"));
    const char *tag = json_str(json_get(m, "tag"));
    const char *cfgdigest = json_str(json_get(m, "config"));
    JNode *arr = json_get(m, "layers");
    if (!arr || arr->type != JARR || !cfgdigest || strncmp(cfgdigest, "sha256:", 7) != 0) {
        fprintf(stderr, "podvm save: bad manifest for %s\n", image_str);
        json_free(m); return -1;
    }
    int nl = (int)arr->n;
    const char *cfghex = cfgdigest + 7;

    char staging[NATIVE_PATH_MAX], blob[NATIVE_PATH_MAX], dst[NATIVE_PATH_MAX + 96];
    snprintf(staging, sizeof(staging), "%s/tmp/save-%d", native_store_dir(), (int)getpid());
    rm_rf(staging);
    if (mkdir_p(staging, 0755) < 0) { json_free(m); return -1; }

    blob_path(cfghex, blob, sizeof(blob));
    snprintf(dst, sizeof(dst), "%s/%s.json", staging, cfghex);
    if (link_or_copy(blob, dst) < 0) {
        fprintf(stderr, "podvm save: missing config blob\n");
        rm_rf(staging); json_free(m); return -1;
    }
    for (int i = 0; i < nl; i++) {
        const char *dg = json_str(json_at(arr, (size_t)i));
        if (!dg || strncmp(dg, "sha256:", 7) != 0) { rm_rf(staging); json_free(m); return -1; }
        const char *hex = dg + 7;
        char ldir[NATIVE_PATH_MAX + 80];
        snprintf(ldir, sizeof(ldir), "%s/%s", staging, hex);
        if (mkdir_p(ldir, 0755) < 0) { rm_rf(staging); json_free(m); return -1; }
        blob_path(hex, blob, sizeof(blob));
        snprintf(dst, sizeof(dst), "%s/layer.tar", ldir);
        if (link_or_copy(blob, dst) < 0) { rm_rf(staging); json_free(m); return -1; }
    }

    {   /* manifest.json (docker-archive) */
        char mp[NATIVE_PATH_MAX + 32];
        snprintf(mp, sizeof(mp), "%s/manifest.json", staging);
        FILE *f = fopen(mp, "w");
        if (!f) { rm_rf(staging); json_free(m); return -1; }
        fprintf(f, "[{\"Config\":\"%s.json\",\"RepoTags\":[\"%s:%s\"],\"Layers\":[",
                cfghex, name ? name : "unnamed", tag ? tag : "latest");
        for (int i = 0; i < nl; i++) {
            const char *dg = json_str(json_at(arr, (size_t)i));
            fprintf(f, "%s\"%s/layer.tar\"", i ? "," : "", dg + 7);
        }
        fprintf(f, "]}]\n");
        fclose(f);
    }
    {   /* repositories (legacy docker-load compat) */
        char rp[NATIVE_PATH_MAX + 32];
        snprintf(rp, sizeof(rp), "%s/repositories", staging);
        FILE *f = fopen(rp, "w");
        if (f) {
            const char *top = nl ? json_str(json_at(arr, (size_t)(nl - 1))) : NULL;
            fprintf(f, "{\"%s\":{\"%s\":\"%s\"}}\n",
                    name ? name : "unnamed", tag ? tag : "latest", top ? top + 7 : "");
            fclose(f);
        }
    }
    json_free(m);

    int rc = tar_pack_dir(staging, out_path);
    if (rc < 0) fprintf(stderr, "podvm save: writing %s failed\n", out_path);
    rm_rf(staging);
    return rc;
}

int native_load(const char *tar_path) {
    char *data = NULL;
    size_t len = 0;
    if (read_file(tar_path, &data, &len) < 0) {
        fprintf(stderr, "podvm load: cannot read %s\n", tar_path);
        return -1;
    }
    char staging[NATIVE_PATH_MAX];
    snprintf(staging, sizeof(staging), "%s/tmp/load-%d", native_store_dir(), (int)getpid());
    rm_rf(staging);
    if (mkdir_p(staging, 0755) < 0) { free(data); return -1; }
    if (tar_extract_mem(data, len, staging) < 0) { free(data); rm_rf(staging); return -1; }
    free(data);

    char mp[NATIVE_PATH_MAX + 32];
    snprintf(mp, sizeof(mp), "%s/manifest.json", staging);
    char *mtext = NULL;
    if (read_file(mp, &mtext, NULL) < 0) {
        fprintf(stderr, "podvm load: no manifest.json (not a docker archive?)\n");
        rm_rf(staging); return -1;
    }
    JNode *mani = json_parse(mtext);
    free(mtext);
    if (!mani || mani->type != JARR || mani->n == 0) {
        fprintf(stderr, "podvm load: bad manifest.json\n");
        json_free(mani); rm_rf(staging); return -1;
    }
    JNode *obj = json_at(mani, 0);
    const char *cfgpath = json_str(json_get(obj, "Config"));
    JNode *lays = json_get(obj, "Layers");
    JNode *tags = json_get(obj, "RepoTags");
    const char *reftag = (tags && tags->type == JARR && tags->n)
                       ? json_str(json_at(tags, 0)) : NULL;
    if (!cfgpath || !lays || lays->type != JARR) {
        fprintf(stderr, "podvm load: manifest missing Config/Layers\n");
        json_free(mani); rm_rf(staging); return -1;
    }
    int nl = (int)lays->n;

    char cfgfull[NATIVE_PATH_MAX + 128];
    snprintf(cfgfull, sizeof(cfgfull), "%s/%s", staging, cfgpath);
    char *cfgdata = NULL;
    size_t cfglen = 0;
    if (read_file(cfgfull, &cfgdata, &cfglen) < 0) {
        fprintf(stderr, "podvm load: missing config %s\n", cfgpath);
        json_free(mani); rm_rf(staging); return -1;
    }
    char cfgdigest[NATIVE_DIGEST_MAX];
    int crc = native_blob_put(cfgdata, cfglen, cfgdigest, sizeof(cfgdigest));
    free(cfgdata);
    if (crc < 0) { json_free(mani); rm_rf(staging); return -1; }

    const char *dptrs[LOAD_MAX];
    char dbufs[LOAD_MAX][NATIVE_DIGEST_MAX];
    int nd = 0;
    for (int i = 0; i < nl && nd < LOAD_MAX; i++) {
        const char *lp = json_str(json_at(lays, (size_t)i));
        if (!lp) continue;
        char lfull[NATIVE_PATH_MAX + 128];
        snprintf(lfull, sizeof(lfull), "%s/%s", staging, lp);
        char *ldata = NULL;
        size_t llen = 0;
        if (read_file(lfull, &ldata, &llen) < 0) {
            fprintf(stderr, "podvm load: missing layer %s\n", lp);
            json_free(mani); rm_rf(staging); return -1;
        }
        if (native_put_layer(ldata, llen, dbufs[nd], sizeof(dbufs[nd])) < 0) {
            free(ldata); json_free(mani); rm_rf(staging); return -1;
        }
        free(ldata);
        dptrs[nd] = dbufs[nd];
        nd++;
    }

    char refbuf[256];
    if (reftag) snprintf(refbuf, sizeof(refbuf), "%s", reftag);
    else snprintf(refbuf, sizeof(refbuf), "loaded/image:latest");
    int prc = native_manifest_put(refbuf, cfgdigest, dptrs, nd, NULL, 0);
    json_free(mani);
    rm_rf(staging);
    if (prc < 0) { fprintf(stderr, "podvm load: cannot write the manifest\n"); return -1; }
    printf("podvm: loaded %s (%d layers)\n", refbuf, nd);
    return 0;
}

int native_gc(int *freed) {
    if (freed) *freed = 0;
    enum { MAXREF = 8192 };
    char (*refs)[65] = malloc(MAXREF * 65);
    if (!refs) return -1;
    int nref = 0;

    /* Collect every digest (config + layers) referenced by a manifest. */
    char mroot[NATIVE_PATH_MAX];
    snprintf(mroot, sizeof(mroot), "%s/manifests", native_store_dir());
    DIR *d = opendir(mroot);
    if (d) {
        struct dirent *nd;
        while ((nd = readdir(d))) {
            if (nd->d_name[0] == '.') continue;
            char namedir[NATIVE_PATH_MAX];
            snprintf(namedir, sizeof(namedir), "%s/%s", mroot, nd->d_name);
            struct stat st;
            if (stat(namedir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            DIR *d2 = opendir(namedir);
            if (!d2) continue;
            struct dirent *te;
            while ((te = readdir(d2))) {
                if (te->d_name[0] == '.') continue;
                char mp[NATIVE_PATH_MAX];
                snprintf(mp, sizeof(mp), "%s/%s", namedir, te->d_name);
                char *text = NULL;
                if (read_file(mp, &text, NULL) < 0) continue;
                JNode *m = json_parse(text);
                free(text);
                if (!m) continue;
                const char *cfg = json_str(json_get(m, "config"));
                if (cfg && !strncmp(cfg, "sha256:", 7) && nref < MAXREF)
                    snprintf(refs[nref++], 65, "%s", cfg + 7);
                JNode *arr = json_get(m, "layers");
                if (arr && arr->type == JARR) {
                    for (size_t i = 0; i < arr->n && nref < MAXREF; i++) {
                        const char *dg = json_str(json_at(arr, i));
                        if (dg && !strncmp(dg, "sha256:", 7))
                            snprintf(refs[nref++], 65, "%s", dg + 7);
                    }
                }
                json_free(m);
            }
            closedir(d2);
        }
        closedir(d);
    }

    int nfreed = 0;
    /* Delete unreferenced blobs. */
    char broot[NATIVE_PATH_MAX];
    snprintf(broot, sizeof(broot), "%s/blobs/sha256", native_store_dir());
    DIR *bd = opendir(broot);
    if (bd) {
        struct dirent *be;
        while ((be = readdir(bd))) {
            if (be->d_name[0] == '.') continue;
            int ref = 0;
            for (int i = 0; i < nref; i++)
                if (!strcmp(refs[i], be->d_name)) { ref = 1; break; }
            if (!ref) {
                char bp[NATIVE_PATH_MAX];
                snprintf(bp, sizeof(bp), "%s/%s", broot, be->d_name);
                if (unlink(bp) == 0) nfreed++;
            }
        }
        closedir(bd);
    }
    /* Delete unreferenced layer diff dirs. */
    char lroot[NATIVE_PATH_MAX];
    snprintf(lroot, sizeof(lroot), "%s/layers", native_store_dir());
    DIR *ld = opendir(lroot);
    if (ld) {
        struct dirent *le;
        while ((le = readdir(ld))) {
            if (le->d_name[0] == '.') continue;
            int ref = 0;
            for (int i = 0; i < nref; i++)
                if (!strcmp(refs[i], le->d_name)) { ref = 1; break; }
            if (!ref) {
                char lp[NATIVE_PATH_MAX];
                snprintf(lp, sizeof(lp), "%s/%s", lroot, le->d_name);
                rm_rf(lp);
                nfreed++;
            }
        }
        closedir(ld);
    }
    free(refs);
    if (freed) *freed = nfreed;
    return 0;
}
