/*
 * store.c - podman storage backend
 *
 * Resolves image references directly against podman's on-disk storage
 * (~/.local/share/containers/storage), so podvm can use images that
 * podman already pulled -- no export, no re-pull, no registry client.
 *
 * Storage layout (containers/storage, overlay driver):
 *
 *   overlay-images/images.json  [ { id, names: [...], layer: <top-id> } ]
 *   overlay-layers/layers.json  [ { id, parent: <id> } ]   (chain to base)
 *   overlay/<layer-id>/diff/    unpacked layer content
 *
 * A rootfs is the union of the chain's diff dirs, most recent first.
 * The caller (main.c) mounts that union as an overlay inside the QEMU
 * child's userns -- rootless storage is only meaningful inside a user
 * namespace, and the VM's isolation boundary is unaffected by which
 * namespace staged the files (ADR-024).
 *
 * If the image is not present and podman exists, we "hijack" podman to
 * pull it (store_ensure), then re-read the storage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

#include "store.h"
#include "json.h"
#include "image.h"

#define MAX_CHAIN 64

/* Default rootless podman storage (XDG data home). */
static int storage_root(char *out, size_t n) {
    const char *xdg = getenv("XDG_DATA_HOME");
    char base[STORE_PATH_MAX];

    if (xdg && xdg[0])
        snprintf(base, sizeof(base), "%s/containers/storage", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) return -1;
        snprintf(base, sizeof(base), "%s/.local/share/containers/storage", home);
    }

    struct stat st;
    if (stat(base, &st) < 0 || !S_ISDIR(st.st_mode))
        return -1;
    snprintf(out, n, "%s", base);
    return 0;
}

/* Read a whole file into a malloc'd buffer (NUL-terminated). */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) < 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/*
 * Normalize a user-facing image name to the fully-qualified form
 * podman stores. podman normalizes "debian:bookworm" to
 * "docker.io/library/debian:bookworm" and local images to
 * "localhost/<name>". Try both conventions plus the raw form.
 * (Candidate list is built inline in store_find.)
 */

/*
 * Look up an image by name in images.json, returning its id (the config
 * blob digest) and top layer id. Either output may be NULL.
 * Returns 0 on success.
 */
static int store_lookup(const char *image, char *image_id, size_t idn,
                        char *top_layer, size_t n) {
    char root[STORE_PATH_MAX];
    if (storage_root(root, sizeof(root)) < 0)
        return -1;

    char path[STORE_PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/overlay-images/images.json", root);
    char *data = read_file(path);
    if (!data) return -1;

    JNode *j = json_parse(data);
    free(data);
    if (!j || j->type != JARR) { json_free(j); return -1; }

    /*
     * Build the candidate name list: the raw ref (with :latest default),
     * the docker.io/library/ form for bare names, and localhost/ for
     * bare names (podman-built images).
     */
    char cand[3][STORE_PATH_MAX];
    int nc = 0;
    snprintf(cand[nc++], STORE_PATH_MAX, "%s", image);
    if (!strchr(image, '/') && !strchr(image, ':')) {
        snprintf(cand[nc++], STORE_PATH_MAX, "docker.io/library/%s:latest", image);
        snprintf(cand[nc++], STORE_PATH_MAX, "localhost/%s:latest", image);
    } else if (!strchr(image, '/')) {
        /* "debian:bookworm" -> docker.io/library/debian:bookworm */
        snprintf(cand[nc++], STORE_PATH_MAX, "docker.io/library/%s", image);
    }

    int found = 0;
    for (size_t i = 0; i < j->n && !found; i++) {
        JNode *im = json_at(j, i);
        JNode *names = json_get(im, "names");
        if (!names || names->type != JARR) continue;
        for (int c = 0; c < nc && !found; c++) {
            for (size_t k = 0; k < names->n; k++) {
                const char *nm = json_str(json_at(names, k));
                if (nm && !strcmp(nm, cand[c])) {
                    const char *layer = json_str(json_get(im, "layer"));
                    if (layer) {
                        if (top_layer)
                            snprintf(top_layer, n, "%s", layer);
                        if (image_id) {
                            const char *id = json_str(json_get(im, "id"));
                            if (id) snprintf(image_id, idn, "%s", id);
                        }
                        found = 1;
                    }
                    break;
                }
            }
        }
    }
    json_free(j);
    return found ? 0 : -1;
}

int store_find(const char *image, char *top_layer, size_t n) {
    return store_lookup(image, NULL, 0, top_layer, n);
}

/*
 * Walk the layer chain from top to base via layers.json parent links.
 * Emits diff dir paths top-first (overlayfs lowerdir order).
 */
int store_chain(const char *top_layer, StoreLayer *layers, int max) {
    char root[STORE_PATH_MAX];
    if (storage_root(root, sizeof(root)) < 0)
        return -1;

    char path[STORE_PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/overlay-layers/layers.json", root);
    char *data = read_file(path);
    if (!data) return -1;

    JNode *j = json_parse(data);
    free(data);
    if (!j || j->type != JARR) { json_free(j); return -1; }

    char want[128];
    snprintf(want, sizeof(want), "%s", top_layer);
    int count = 0;

    while (want[0] && count < max) {
        int found = 0;
        for (size_t i = 0; i < j->n; i++) {
            JNode *l = json_at(j, i);
            const char *id = json_str(json_get(l, "id"));
            if (!id || strcmp(id, want)) continue;

            snprintf(layers[count].diff, sizeof(layers[count].diff),
                     "%s/overlay/%s/diff", root, id);
            count++;
            found = 1;

            const char *parent = json_str(json_get(l, "parent"));
            if (parent)
                snprintf(want, sizeof(want), "%s", parent);
            else
                want[0] = '\0';   /* base layer */
            break;
        }
        if (!found) break;   /* dangling parent: tolerate truncated chain */
    }

    json_free(j);
    return count > 0 ? count : -1;
}

/* Is podman available to pull missing images? */
int store_podman_available(void) {
    return access("/usr/bin/podman", X_OK) == 0 ||
           access("/bin/podman", X_OK) == 0;
}

/*
 * Base64-encode a string (standard alphabet, with padding).
 * Used to build podman's big-data filenames: containers/storage stores
 * an image's config blob as overlay-images/<id>/=<base64("sha256:<id>")>.
 * out must hold 4*ceil(len/3)+1 bytes; we bound the copy to outn.
 */
static void b64_encode(const char *in, char *out, size_t outn) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in);
    size_t o = 0;
    /* Bound by len, not by in[i]: with len%3 != 0 the final stride would
     * step past the NUL and read whatever follows the string. */
    for (size_t i = 0; i < len && o + 4 < outn; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < len) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

/*
 * Read an image's config blob out of podman storage and parse it into cfg.
 *
 * containers/storage keeps the OCI config blob as "big data" under the
 * image dir, named =<base64(name)>. For images podman pulled, the image id
 * *is* the config digest, so the blob is at
 *   overlay-images/<id>/=base64("sha256:<id>")
 * which we try first. As a fallback we read the "manifest" big-data entry
 * and follow its config.digest -- covers images whose id is not the config
 * digest (e.g. some locally-built/converted images).
 *
 * Returns 0 on success (cfg filled, possibly partially), -1 if no config
 * blob could be read or parsed. Caller must image_config_init() first and
 * image_config_free() after.
 */
int store_image_config(const char *image, ImageConfig *cfg) {
    char root[STORE_PATH_MAX];
    char id[128];
    char name[256];
    char b64[512];
    char path[STORE_PATH_MAX * 2];
    char *data = NULL;

    if (storage_root(root, sizeof(root)) < 0)
        return -1;
    if (store_lookup(image, id, sizeof(id), NULL, 0) < 0)
        return -1;

    /* Primary: the config blob stored under the image id's digest. */
    snprintf(name, sizeof(name), "sha256:%s", id);
    b64_encode(name, b64, sizeof(b64));
    snprintf(path, sizeof(path), "%s/overlay-images/%s/=%s", root, id, b64);
    data = read_file(path);

    /* Fallback: follow the manifest's config.digest. */
    if (!data) {
        b64_encode("manifest", b64, sizeof(b64));
        snprintf(path, sizeof(path), "%s/overlay-images/%s/=%s", root, id, b64);
        char *mani = read_file(path);
        if (mani) {
            JNode *j = json_parse(mani);
            free(mani);
            const char *cdigest = json_str(json_get(json_get(j, "config"), "digest"));
            if (cdigest) {
                b64_encode(cdigest, b64, sizeof(b64));
                snprintf(path, sizeof(path), "%s/overlay-images/%s/=%s", root, id, b64);
                data = read_file(path);
            }
            json_free(j);
        }
    }

    if (!data)
        return -1;
    int rc = image_config_parse(data, cfg);
    free(data);
    return rc;
}

/*
 * "Hijack" podman to pull an image into its storage. Blocks until done.
 * Returns 0 on success.
 */
int store_podman_pull(const char *image) {
    if (!store_podman_available())
        return -1;
    printf("podvm: pulling %s via podman\n", image);
    pid_t pid = fork();
    if (pid == 0) {
        execlp("podman", "podman", "pull", image, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int store_resolve(const char *image, char *top_layer, size_t n,
                  int allow_pull) {
    if (store_find(image, top_layer, n) == 0)
        return 0;
    if (!allow_pull || !store_podman_available())
        return -1;
    if (store_podman_pull(image) < 0)
        return -1;
    return store_find(image, top_layer, n);
}
