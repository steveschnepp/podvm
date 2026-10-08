/*
 * image.c - OCI image handling
 *
 * Resolve an image reference to a rootfs directory, pulling from an OCI
 * registry (Docker Hub / any v2 registry) when needed.
 *
 * Pull pipeline:
 *   1. Parse ref into registry / repository / tag.
 *   2. GET manifest (Bearer token auth on 401). If the response is a
 *      manifest list / index, select the linux/amd64 image manifest.
 *   3. For each layer blob: download, verify its sha256 against the
 *      manifest digest, decompress (gzip/zstd/raw), untar into the
 *      rootfs, applying OCI whiteout semantics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <curl/curl.h>
#include <zlib.h>
#include <zstd.h>

#include "image.h"
#include "json.h"
#include "sha256.h"

/* Default cache directory */
#define DEFAULT_CACHE_DIR "/tmp/podvm/images"
#define PATH_SIZE 2048

static char cache_dir[PATH_SIZE] = DEFAULT_CACHE_DIR;

/* ───────────────────────── small helpers ───────────────────────── */

static int mkdir_p(const char *path, int mode) {
    char tmp[PATH_SIZE];
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

/* Growable byte buffer */
typedef struct { char *data; size_t len; size_t cap; } Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf *b) { free(b->data); buf_init(b); }

static int buf_reserve(Buf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < b->len + need + 1) ncap *= 2;
    char *nd = realloc(b->data, ncap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static size_t buf_write(void *ptr, size_t sz, size_t nmemb, void *ud) {
    Buf *b = (Buf *)ud;
    size_t total = sz * nmemb;
    if (buf_reserve(b, total) < 0) return 0;
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

/* ───────────────────────── gzip (zlib) ───────────────────────── */

/* Decompress a gzip/zlib buffer into a new malloc'd buffer. */
static int gunzip_mem(const char *in, size_t inlen, Buf *out) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* 15+32: auto-detect gzip/zlib header */
    if (inflateInit2(&strm, 15 + 32) != Z_OK) return -1;

    strm.next_in = (Bytef *)in;
    strm.avail_in = (uInt)inlen;

    if (buf_reserve(out, inlen * 4 + 1024) < 0) { inflateEnd(&strm); return -1; }

    int ret = Z_OK;
    do {
        if (strm.total_out >= out->cap - 1) {
            if (buf_reserve(out, 65536) < 0) { inflateEnd(&strm); return -1; }
        }
        strm.next_out = (Bytef *)(out->data + out->len);
        strm.avail_out = (uInt)(out->cap - out->len - 1);
        ret = inflate(&strm, Z_NO_FLUSH);
        out->len = strm.total_out;
        out->data[out->len] = '\0';
    } while (ret == Z_OK && strm.avail_in > 0);

    inflateEnd(&strm);
    return (ret == Z_OK || ret == Z_STREAM_END) ? 0 : -1;
}

/* zstd magic number (RFC 8878): 0x28 0xB5 0x2F 0xFD. */
#define ZSTD_MAGIC 0xFD2FB528U

/*
 * Decompress a zstd frame into a new malloc'd buffer.
 *
 * Registries increasingly publish zstd layers (OCI media type
 * ...tar+zstd); without this the pull fails on them. We ask zstd for the
 * decompressed size when the frame advertises it, and otherwise grow the
 * buffer geometrically -- same shape as gunzip_mem.
 */
static int zstd_decompress_mem(const char *in, size_t inlen, Buf *out) {
    unsigned long long dsz = ZSTD_getFrameContentSize(in, inlen);
    size_t want = (dsz == ZSTD_CONTENTSIZE_UNKNOWN || dsz == ZSTD_CONTENTSIZE_ERROR)
                  ? inlen * 4 + 1024
                  : (size_t)dsz;
    if (buf_reserve(out, want + 1) < 0) return -1;

    size_t rc;
    for (;;) {
        rc = ZSTD_decompress(out->data + out->len, out->cap - out->len - 1,
                             in, inlen);
        if (!ZSTD_isError(rc)) {
            out->len += rc;
            out->data[out->len] = '\0';
            return 0;
        }
        /* OutDstSize: grow and retry. Anything else is fatal. */
        if (ZSTD_getErrorCode(rc) != ZSTD_error_dstSize_tooSmall)
            return -1;
        if (buf_reserve(out, 65536) < 0) return -1;
    }
}

/* ───────────────────────── tar extraction ───────────────────────── */

static unsigned long long tar_octal(const char *s, size_t n) {
    unsigned long long v = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = (v << 3) + (unsigned long long)(s[i] - '0');
    }
    return v;
}

/* Join destroot + entry name, rejecting absolute paths and "..". */
static int safe_path(char *out, size_t outsz, const char *root, const char *name) {
    while (*name == '/') name++;
    if (strstr(name, "../") != NULL) return -1;
    if (strcmp(name, "..") == 0) return -1;
    if (snprintf(out, outsz, "%s/%s", root, name) >= (int)outsz) return -1;
    return 0;
}

static void apply_whiteout(const char *root, const char *dirname, const char *base) {
    /* base is ".wh.<name>" or ".wh..wh..opq" */
    if (strcmp(base, ".wh..wh..opq") == 0) {
        /* Opaque dir: remove everything already present in dirname */
        char dirpath[PATH_SIZE];
        if (safe_path(dirpath, sizeof(dirpath), root, dirname) < 0) return;
        DIR *d = opendir(dirpath);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[PATH_SIZE];
            if (snprintf(child, sizeof(child), "%s/%s", dirpath, de->d_name) < (int)sizeof(child))
                remove(child);
        }
        closedir(d);
        return;
    }
    /* Regular whiteout: delete "<dirname>/<name-after-.wh.>" */
    const char *target = base + 4; /* skip ".wh." */
    char path[PATH_SIZE];
    char dirbuf[PATH_SIZE];
    if (dirname[0])
        snprintf(dirbuf, sizeof(dirbuf), "%s/%s", dirname, target);
    else
        snprintf(dirbuf, sizeof(dirbuf), "%s", target);
    if (safe_path(path, sizeof(path), root, dirbuf) == 0)
        remove(path);
}

static int untar_mem(const char *tar, size_t tarlen, const char *root) {
    size_t off = 0;
    char longname[PATH_SIZE] = "";

    while (off + 512 <= tarlen) {
        const char *h = tar + off;
        int empty = 1;
        for (int i = 0; i < 512; i++) if (h[i]) { empty = 0; break; }
        if (empty) break; /* end-of-archive marker */

        char name[PATH_SIZE];
        char prefix[160];
        memcpy(name, h + 0, 100);
        name[100] = '\0';
        unsigned long long size = tar_octal(h + 124, 12);
        char typeflag = h[156];
        memcpy(prefix, h + 345, 155);
        prefix[155] = '\0';

        off += 512;

        if (longname[0]) {
            /* Previous header was GNU 'L' — its data is the real name */
            snprintf(name, sizeof(name), "%s", longname);
            longname[0] = '\0';
            if (typeflag == 'L' || typeflag == 'K') { /* nested longname: skip data */ }
        }

        size_t data_blocks = (size_t)((size + 511) / 512) * 512;
        const char *data = tar + off;

        if (typeflag == 'L') {
            /* GNU long name for the NEXT header */
            size_t cp = size < sizeof(longname) - 1 ? size : sizeof(longname) - 1;
            memcpy(longname, data, cp);
            longname[cp] = '\0';
            /* strip trailing NULs */
            for (size_t k = cp; k > 0 && longname[k - 1] == '\0'; k--) longname[k - 1] = '\0';
            off += data_blocks;
            continue;
        }
        if (typeflag == 'K' || typeflag == 'x' || typeflag == 'g') {
            /* longlink / pax extended headers: skip */
            off += data_blocks;
            continue;
        }

        /* Build full name from prefix + name */
        char full[PATH_SIZE];
        if (prefix[0])
            snprintf(full, sizeof(full), "%s/%s", prefix, name);
        else
            snprintf(full, sizeof(full), "%s", name);

        char base[PATH_SIZE];
        { const char *sl = strrchr(full, '/'); snprintf(base, sizeof(base), "%s", sl ? sl + 1 : full); }

        /* Whiteout handling */
        if (!strncmp(base, ".wh.", 4)) {
            char dir[PATH_SIZE];
            const char *slash = strrchr(full, '/');
            if (slash) { size_t dl = (size_t)(slash - full); if (dl >= sizeof(dir)) dl = sizeof(dir) - 1; memcpy(dir, full, dl); dir[dl] = '\0'; }
            else dir[0] = '\0';
            apply_whiteout(root, dir, base);
            off += data_blocks;
            continue;
        }

        char path[PATH_SIZE];
        if (safe_path(path, sizeof(path), root, full) < 0) {
            off += data_blocks;
            continue;
        }

        if (typeflag == '5') { /* directory */
            mkdir_p(path, 0755);
        } else if (typeflag == '2') { /* symlink */
            char target[PATH_SIZE];
            memcpy(target, h + 157, 100);
            target[100] = '\0';
            char parent[PATH_SIZE];
            snprintf(parent, sizeof(parent), "%s", path);
            char *slash = strrchr(parent, '/');
            if (slash) *slash = '\0';
            mkdir_p(parent, 0755);
            remove(path);
            symlink(target, path);
        } else if (typeflag == '1') { /* hardlink: treat as symlink-ish copy target */
            char target[PATH_SIZE];
            memcpy(target, h + 157, 100);
            target[100] = '\0';
            char linkpath[PATH_SIZE];
            char parent[PATH_SIZE];
            snprintf(parent, sizeof(parent), "%s", path);
            char *slash = strrchr(parent, '/');
            if (slash) *slash = '\0';
            if (safe_path(linkpath, sizeof(linkpath), root, target) == 0) {
                mkdir_p(parent, 0755);
                remove(path);
                link(linkpath, path);
            }
        } else { /* regular file ('0' or '\0') */
            char parent[PATH_SIZE];
            snprintf(parent, sizeof(parent), "%s", path);
            char *slash = strrchr(parent, '/');
            if (slash) *slash = '\0';
            mkdir_p(parent, 0755);

            unsigned mode = (unsigned)tar_octal(h + 100, 8);
            if (!mode) mode = 0644;
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
            if (fd >= 0) {
                if (size) { ssize_t w = write(fd, data, (size_t)size); (void)w; }
                close(fd);
            }
        }

        off += data_blocks;
    }
    return 0;
}

/* ───────────────────────── digest verification ───────────────────────── */

/*
 * Check a downloaded buffer against the manifest's expected digest
 * ("sha256:<hex>"). Returns 1 if it matches (or there is nothing to
 * check against -- a missing/unsupported-algorithm digest is not a
 * mismatch we can judge), 0 if it does not match.
 */
static int digest_matches(const char *data, size_t len, const char *digest) {
    if (!digest || strncmp(digest, "sha256:", 7) != 0)
        return 1;
    char got[65];
    sha256_hex(data, len, got);
    return strcmp(got, digest + 7) == 0;
}

/* ───────────────────────── registry client ───────────────────────── */

typedef struct { char registry[256]; char repo[256]; char tag[128]; } RegRef;

/* Map a logical registry name to the endpoint hostname. */
static const char *registry_host(const char *registry) {
    if (!strcmp(registry, "docker.io") || !strcmp(registry, "index.docker.io") ||
        !strcmp(registry, "registry-1.docker.io"))
        return "registry-1.docker.io";
    return registry;
}

static int reg_parse(const char *name, const char *tag, RegRef *r) {
    memset(r, 0, sizeof(*r));

    /* Split first path component */
    const char *slash = strchr(name, '/');
    size_t firstlen = slash ? (size_t)(slash - name) : strlen(name);
    char first[256];
    if (firstlen >= sizeof(first)) return -1;
    memcpy(first, name, firstlen);
    first[firstlen] = '\0';

    int has_registry = (strchr(first, '.') != NULL || strchr(first, ':') != NULL ||
                        !strcmp(first, "localhost"));
    if (has_registry) {
        snprintf(r->registry, sizeof(r->registry), "%s", first);
        snprintf(r->repo, sizeof(r->repo), "%s", slash ? slash + 1 : "");
    } else {
        snprintf(r->registry, sizeof(r->registry), "docker.io");
        snprintf(r->repo, sizeof(r->repo), "%s", name);
    }
    if (r->repo[0] == '\0') return -1;

    /* Official image on Docker Hub -> library/ prefix */
    if (!strcmp(r->registry, "docker.io") && !strchr(r->repo, '/')) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "library/%s", r->repo);
        snprintf(r->repo, sizeof(r->repo), "%s", tmp);
    }

    snprintf(r->tag, sizeof(r->tag), "%s", (tag && tag[0]) ? tag : "latest");
    return 0;
}

typedef struct { long status; Buf body; Buf headers; } HttpRes;

static size_t hdr_write(void *ptr, size_t sz, size_t nmemb, void *ud) {
    return buf_write(ptr, sz, nmemb, ud);
}

static int http_get(const char *url, const char *accept, const char *bearer,
                    HttpRes *res) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    buf_init(&res->body);
    buf_init(&res->headers);
    res->status = 0;

    struct curl_slist *hdrs = NULL;
    if (accept)
        hdrs = curl_slist_append(hdrs, accept);
    if (bearer) {
        /* Bearer tokens run ~2.7KB; the header buffer must hold
         * "Authorization: Bearer " plus the whole token. */
        char auth[4096];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
        hdrs = curl_slist_append(hdrs, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res->body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, hdr_write);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res->headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "podvm/0.1");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res->status);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return (rc == CURLE_OK) ? 0 : -1;
}

/* Extract a parameter from a WWW-Authenticate: Bearer k=v,k2=v2 header. */
static int challenge_param(const char *hdr, const char *key, char *out, size_t n) {
    const char *p = strstr(hdr, key);
    if (!p) return -1;
    p += strlen(key);
    if (*p == '=') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
        out[i] = '\0';
        return 0;
    }
    size_t i = 0;
    while (*p && *p != ',' && *p != ' ' && i + 1 < n) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

/* Fetch a Bearer token for the given realm/service/scope. */
static int registry_token(const char *realm, const char *service,
                          const char *scope, char *token, size_t n) {
    char url[1024];
    if (scope && scope[0])
        snprintf(url, sizeof(url), "%s?service=%s&scope=%s", realm, service, scope);
    else
        snprintf(url, sizeof(url), "%s?service=%s", realm, service);

    HttpRes res;
    if (http_get(url, NULL, NULL, &res) < 0) return -1;
    int ok = -1;
    if (res.status == 200 && res.body.data) {
        JNode *j = json_parse(res.body.data);
        if (j) {
            JNode *t = json_get(j, "token");
            if (!t) t = json_get(j, "access_token");
            const char *ts = json_str(t);
            if (ts) {
                /* Docker Hub JWTs are ~2.7KB; detect truncation
                 * instead of silently clipping the token (which
                 * yields an invalid JWT and a confusing 401). */
                int w = snprintf(token, n, "%s", ts);
                if (w < 0 || (size_t)w >= n) {
                    fprintf(stderr, "podvm: auth token too long (%zu bytes)\n",
                            strlen(ts));
                } else {
                    ok = 0;
                }
            }
            json_free(j);
        }
    }
    buf_free(&res.body);
    buf_free(&res.headers);
    return ok;
}

/*
 * GET url with Bearer auth. On 401, parse the WWW-Authenticate challenge,
 * fetch a token, and retry once. Returns 0 on HTTP 200 with body in res.
 */
static int registry_get(const char *url, const char *accept,
                        const char *default_scope, HttpRes *res) {
    if (http_get(url, accept, NULL, res) < 0) return -1;
    if (res->status != 401) return (res->status == 200) ? 0 : -1;

    /* Find WWW-Authenticate header */
    char *ww = NULL;
    if (res->headers.data) {
        char *p = res->headers.data;
        while (p && *p) {
            if (!strncasecmp(p, "www-authenticate:", 17)) {
                ww = p;
                break;
            }
            p = strchr(p, '\n');
            if (p) p++;
        }
    }
    if (!ww) return -1;

    char realm[512] = "", service[256] = "", chal_scope[256] = "";
    if (challenge_param(ww, "realm", realm, sizeof(realm)) < 0) return -1;
    challenge_param(ww, "service", service, sizeof(service));
    challenge_param(ww, "scope", chal_scope, sizeof(chal_scope));
    const char *scope = chal_scope[0] ? chal_scope : default_scope;

    /* Docker Hub bearer tokens run ~2.7KB; keep headroom. */
    char token[4096];
    if (registry_token(realm, service, scope, token, sizeof(token)) < 0) return -1;

    buf_free(&res->body);
    buf_free(&res->headers);
    if (http_get(url, accept, token, res) < 0) return -1;
    return (res->status == 200) ? 0 : -1;
}

/* If the parsed manifest is an index/list, return the selected digest. */
static int select_platform_manifest(JNode *mani, char *digest, size_t n) {
    JNode *manifests = json_get(mani, "manifests");
    if (!manifests || manifests->type != JARR) return 0; /* not an index */

    const char *want_digest = NULL;
    for (size_t i = 0; i < manifests->n; i++) {
        JNode *m = json_at(manifests, i);
        JNode *pl = json_get(m, "platform");
        const char *os = json_str(json_get(pl, "os"));
        const char *arch = json_str(json_get(pl, "architecture"));
        const char *d = json_str(json_get(m, "digest"));
        if (!d) continue;
        int match = os && arch && !strcmp(os, "linux") && !strcmp(arch, "x86_64");
        if (!match && os && arch && !strcmp(os, "linux") && !strcmp(arch, "amd64"))
            match = 1;
        if (match) { want_digest = d; break; }
        if (!want_digest) want_digest = d; /* fallback: first entry */
    }
    if (!want_digest) return -1;
    snprintf(digest, n, "%s", want_digest);
    return 1;
}

/* ───────────────────────── image config blob ───────────────────────── */

void image_config_init(ImageConfig *c) {
    if (c) memset(c, 0, sizeof(*c));
}

void image_config_free(ImageConfig *c) {
    if (!c) return;
    for (int i = 0; i < c->nenv; i++)   free(c->env[i]);
    for (int i = 0; i < c->nentry; i++) free(c->entrypoint[i]);
    for (int i = 0; i < c->ncmd; i++)   free(c->cmd[i]);
    memset(c, 0, sizeof(*c));
}

/* Copy a JSON string array into a slot array of malloc'd strings. */
static void cfg_copy_strarr(JNode *arr, char **dst, int *n, int max) {
    if (!arr || arr->type != JARR) return;
    for (size_t i = 0; i < arr->n && *n < max; i++) {
        const char *s = json_str(json_at(arr, i));
        if (!s) continue;
        dst[*n] = strdup(s);
        if (dst[*n]) (*n)++;
    }
}

int image_config_parse(const char *json_text, ImageConfig *c) {
    if (!json_text || !c) return -1;

    JNode *j = json_parse(json_text);
    if (!j || j->type != JOBJ) { json_free(j); return -1; }

    JNode *cfg = json_get(j, "config");
    if (!cfg || cfg->type != JOBJ) {
        /* Not a config blob (may be a manifest): not usable, but not
         * an error worth failing the caller over -- they treat empty
         * config as "image carries no defaults". */
        json_free(j);
        return -1;
    }

    cfg_copy_strarr(json_get(cfg, "Env"),   c->env,   &c->nenv,   MAX_ENV_VARS);
    cfg_copy_strarr(json_get(cfg, "Entrypoint"), c->entrypoint, &c->nentry, MAX_CMD_PARTS);
    cfg_copy_strarr(json_get(cfg, "Cmd"),   c->cmd,   &c->ncmd,   MAX_CMD_PARTS);

    const char *wd = json_str(json_get(cfg, "WorkingDir"));
    if (wd) snprintf(c->cwd, sizeof(c->cwd), "%s", wd);

    const char *user = json_str(json_get(cfg, "User"));
    if (user) snprintf(c->user, sizeof(c->user), "%s", user);

    json_free(j);
    return 0;
}

/* ───────────────────────── public API ───────────────────────── */

int image_parse(const char *image_str, ImageRef *ref) {
    if (!image_str || !ref) return -1;

    memset(ref, 0, sizeof(*ref));

    const char *colon = strrchr(image_str, ':');
    const char *slash = strrchr(image_str, '/');

    /* A ':' after the last '/' is a tag. A ':' before it is a registry
     * port (host:port/repo), which is part of the name, not a tag. */
    int colon_is_tag = (colon && (!slash || colon > slash));

    if (colon_is_tag) {
        size_t name_len = (size_t)(colon - image_str);
        if (name_len >= MAX_IMAGE_NAME) name_len = MAX_IMAGE_NAME - 1;
        memcpy(ref->name, image_str, name_len);
        ref->name[name_len] = '\0';
        strncpy(ref->tag, colon + 1, MAX_IMAGE_NAME - 1);
    } else {
        strncpy(ref->name, image_str, MAX_IMAGE_NAME - 1);
        strcpy(ref->tag, "latest");
    }
    return 0;
}

int image_is_path(const char *str) {
    if (!str) return 0;
    if (str[0] == '/') return 1;
    if (!strncmp(str, "./", 2) || !strncmp(str, "../", 3)) return 1;
    struct stat st;
    if (stat(str, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    return 0;
}

const char *image_cache_dir(void) {
    return cache_dir;
}

int image_pull(const char *image_name, const char *tag) {
    RegRef rr;
    if (reg_parse(image_name, tag, &rr) < 0) {
        fprintf(stderr, "podvm: cannot parse image reference: %s\n", image_name);
        return -1;
    }

    const char *host = registry_host(rr.registry);
    char scope[512];
    snprintf(scope, sizeof(scope), "repository:%s:pull", rr.repo);

    char base[1024];
    snprintf(base, sizeof(base), "https://%s/v2/%s", host, rr.repo);

    char accept[512];
    snprintf(accept, sizeof(accept),
             "Accept: application/vnd.oci.image.index.v1+json,"
             "application/vnd.docker.distribution.manifest.list.v2+json,"
             "application/vnd.oci.image.manifest.v1+json,"
             "application/vnd.docker.distribution.manifest.v2+json");

    printf("podvm: pulling %s/%s:%s ...\n", rr.registry, rr.repo, rr.tag);

    /* 1. Fetch manifest (or index) */
    char mani_url[1200];
    snprintf(mani_url, sizeof(mani_url), "%s/manifests/%s", base, rr.tag);
    HttpRes res;
    if (registry_get(mani_url, accept, scope, &res) < 0) {
        fprintf(stderr, "podvm: manifest fetch failed for %s (HTTP %ld)\n",
                mani_url, res.status);
        buf_free(&res.body); buf_free(&res.headers);
        return -1;
    }

    JNode *mani = json_parse(res.body.data ? res.body.data : "");
    buf_free(&res.body); buf_free(&res.headers);
    if (!mani) {
        fprintf(stderr, "podvm: cannot parse manifest JSON\n");
        return -1;
    }

    /* 2. If it's an index, resolve the platform manifest */
    char subdigest[128];
    int sel = select_platform_manifest(mani, subdigest, sizeof(subdigest));
    if (sel < 0) {
        fprintf(stderr, "podvm: no linux/amd64 manifest in index\n");
        json_free(mani);
        return -1;
    }
    if (sel > 0) {
        json_free(mani);
        snprintf(mani_url, sizeof(mani_url), "%s/manifests/%s", base, subdigest);
        if (registry_get(mani_url, accept, scope, &res) < 0) {
            fprintf(stderr, "podvm: sub-manifest fetch failed (HTTP %ld)\n", res.status);
            buf_free(&res.body); buf_free(&res.headers);
            return -1;
        }
        mani = json_parse(res.body.data ? res.body.data : "");
        buf_free(&res.body); buf_free(&res.headers);
        if (!mani) return -1;
    }

    /* 3. Download + extract layers */
    char root[PATH_SIZE];
    snprintf(root, sizeof(root), "%s/%s/%s", cache_dir, rr.registry, rr.repo);
    if (snprintf(root, sizeof(root), "%s/%s/%s/%s", cache_dir, rr.registry, rr.repo, rr.tag)
        >= (int)sizeof(root)) {
        json_free(mani);
        return -1;
    }
    if (mkdir_p(root, 0755) < 0) {
        fprintf(stderr, "podvm: cannot create rootfs dir %s: %s\n", root, strerror(errno));
        json_free(mani);
        return -1;
    }

    JNode *layers = json_get(mani, "layers");
    if (!layers || layers->type != JARR) {
        fprintf(stderr, "podvm: manifest has no layers\n");
        json_free(mani);
        return -1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    int rc = 0;
    for (size_t i = 0; i < layers->n && rc == 0; i++) {
        JNode *L = json_at(layers, i);
        const char *digest = json_str(json_get(L, "digest"));
        if (!digest) continue;

        char blob_url[1400];
        snprintf(blob_url, sizeof(blob_url), "%s/blobs/%s", base, digest);

        HttpRes bres;
        if (registry_get(blob_url, NULL, scope, &bres) < 0) {
            fprintf(stderr, "podvm: layer fetch failed: %s (HTTP %ld)\n",
                    digest, bres.status);
            buf_free(&bres.body); buf_free(&bres.headers);
            rc = -1;
            break;
        }

        /* Integrity: the manifest names the blob by its sha256, so a
         * truncated or tampered download must not be extracted. */
        if (!digest_matches(bres.body.data ? bres.body.data : "", bres.body.len,
                            digest)) {
            char got[65];
            sha256_hex(bres.body.data ? bres.body.data : "", bres.body.len, got);
            fprintf(stderr, "podvm: layer digest mismatch for %s\n", digest);
            fprintf(stderr, "podvm:   expected %s\n", digest);
            fprintf(stderr, "podvm:   got      sha256:%s\n", got);
            buf_free(&bres.body); buf_free(&bres.headers);
            rc = -1;
            break;
        }

        /* Layers are gzipped tar, zstd tar, or raw tar -- detect by
         * magic bytes rather than trusting the manifest media type. */
        Buf tar;
        buf_init(&tar);
        int is_gzip = bres.body.len >= 2 &&
                      (unsigned char)bres.body.data[0] == 0x1f &&
                      (unsigned char)bres.body.data[1] == 0x8b;
        int is_zstd = bres.body.len >= 4 &&
                      (unsigned char)bres.body.data[0] == 0x28 &&
                      (unsigned char)bres.body.data[1] == 0xB5 &&
                      (unsigned char)bres.body.data[2] == 0x2F &&
                      (unsigned char)bres.body.data[3] == 0xFD;
        int decompressed = is_gzip || is_zstd;
        if (is_gzip) {
            if (gunzip_mem(bres.body.data, bres.body.len, &tar) < 0) {
                fprintf(stderr, "podvm: gunzip failed for layer %s\n", digest);
                rc = -1;
            }
        } else if (is_zstd) {
            if (zstd_decompress_mem(bres.body.data, bres.body.len, &tar) < 0) {
                fprintf(stderr, "podvm: zstd decompress failed for layer %s\n", digest);
                rc = -1;
            }
        } else {
            /* raw tar: alias the buffer */
            tar.data = bres.body.data;
            tar.len = bres.body.len;
            tar.cap = bres.body.cap;
            bres.body.data = NULL; /* prevent double free */
        }

        if (rc == 0 && untar_mem(tar.data ? tar.data : "", tar.len, root) < 0) {
            fprintf(stderr, "podvm: untar failed for layer %s\n", digest);
            rc = -1;
        }

        if (decompressed) buf_free(&tar);
        buf_free(&bres.body);
        buf_free(&bres.headers);
    }

    /*
     * Fetch the config blob (Env/Entrypoint/Cmd/WorkingDir/User) and
     * persist it next to the rootfs as .podvm-config.json so a cache hit
     * still carries the image's default command. Best-effort: a missing
     * config blob just means the image carries no defaults.
     */
    if (rc == 0) {
        const char *cfgdigest = json_str(json_get(json_get(mani, "config"), "digest"));
        if (cfgdigest) {
            char cfgurl[1400];
            snprintf(cfgurl, sizeof(cfgurl), "%s/blobs/%s", base, cfgdigest);
            HttpRes cres;
            buf_init(&cres.body);
            buf_init(&cres.headers);
            if (registry_get(cfgurl, NULL, scope, &cres) == 0 && cres.body.data &&
                digest_matches(cres.body.data, cres.body.len, cfgdigest)) {
                char cfgpath[PATH_SIZE];
                snprintf(cfgpath, sizeof(cfgpath), "%s/.podvm-config.json", root);
                FILE *cf = fopen(cfgpath, "wb");
                if (cf) {
                    fwrite(cres.body.data, 1, cres.body.len, cf);
                    fclose(cf);
                }
            }
            buf_free(&cres.body);
            buf_free(&cres.headers);
        }
    }

    json_free(mani);
    curl_global_cleanup();

    if (rc == 0)
        printf("podvm: image %s:%s ready at %s\n", image_name, rr.tag, root);
    return rc;
}

int image_resolve(const char *image_str, char *rootfs_path, size_t path_size) {
    if (!image_str || !rootfs_path) return -1;

    /* If it's a path, use directly */
    if (image_is_path(image_str)) {
        strncpy(rootfs_path, image_str, path_size - 1);
        rootfs_path[path_size - 1] = '\0';
        struct stat st;
        if (stat(rootfs_path, &st) < 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "podvm: directory not found: %s\n", rootfs_path);
            return -1;
        }
        return 0;
    }

    ImageRef ref;
    if (image_parse(image_str, &ref) < 0) {
        fprintf(stderr, "podvm: failed to parse image name: %s\n", image_str);
        return -1;
    }

    RegRef rr;
    if (reg_parse(ref.name, ref.tag, &rr) < 0) {
        fprintf(stderr, "podvm: cannot parse image reference: %s\n", ref.name);
        return -1;
    }

    /* Check cache: <cache>/<registry>/<repo>/<tag> */
    char image_dir[PATH_SIZE];
    if (snprintf(image_dir, sizeof(image_dir), "%s/%s/%s/%s",
                 cache_dir, rr.registry, rr.repo, rr.tag) >= (int)path_size)
        return -1;

    struct stat st;
    if (stat(image_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        strncpy(rootfs_path, image_dir, path_size - 1);
        rootfs_path[path_size - 1] = '\0';
        printf("podvm: using cached image %s:%s\n", ref.name, ref.tag);
        return 0;
    }

    if (image_pull(ref.name, ref.tag) < 0)
        return -1;

    if (stat(image_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "podvm: image dir missing after pull: %s\n", image_dir);
        return -1;
    }

    strncpy(rootfs_path, image_dir, path_size - 1);
    rootfs_path[path_size - 1] = '\0';
    return 0;
}

/*
 * Load a cached image's config blob (Env/Entrypoint/Cmd/WorkingDir/User).
 *
 * image_pull() drops the config blob next to the unpacked rootfs as
 * <dir>/.podvm-config.json; this reads it back so a cache hit still
 * carries the image's default command. Returns 0 on success, -1 if the
 * cached config is absent or unparseable (caller treats that as "image
 * carries no defaults" and requires an explicit command).
 */
int image_config_load(const char *image_str, ImageConfig *c) {
    if (!image_str || !c || image_is_path(image_str)) return -1;

    ImageRef ref;
    if (image_parse(image_str, &ref) < 0) return -1;

    RegRef rr;
    if (reg_parse(ref.name, ref.tag, &rr) < 0) return -1;

    char cfgpath[PATH_SIZE];
    if (snprintf(cfgpath, sizeof(cfgpath), "%s/%s/%s/%s/.podvm-config.json",
                 cache_dir, rr.registry, rr.repo, rr.tag) >= (int)sizeof(cfgpath))
        return -1;

    FILE *f = fopen(cfgpath, "rb");
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

    int rc = image_config_parse(buf, c);
    free(buf);
    return rc;
}
