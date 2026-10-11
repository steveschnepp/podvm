/*
 * store_native.h - podvm's own layered, content-addressable image store
 *
 * Implements ADR-027. Layers live as sha256 blobs + unpacked diff dirs;
 * an image is a manifest of ordered layer digests + a config blob. This
 * is the store for images podvm itself produces (its registry-client pull
 * once Phase 2 lands, and `podvm build`), as opposed to store.c which
 * READS podman's storage in place (ADR-024).
 *
 * Layout under the store root S:
 *   S/blobs/sha256/<hex>     layer tars + config JSON, keyed by sha256
 *   S/layers/<hex>/diff/     unpacked layer content (overlay lowerdir)
 *   S/manifests/<mangled-name>/<tag>   image manifest JSON
 */
#ifndef STORE_NATIVE_H
#define STORE_NATIVE_H

#include <stddef.h>
#include "image.h"          /* ImageConfig, image_parse */

#define NATIVE_DIGEST_MAX 72          /* "sha256:" + 64 hex + NUL */
#define NATIVE_PATH_MAX   2048

typedef struct {
    char digest[NATIVE_DIGEST_MAX];   /* "sha256:<hex>" */
    char diff[NATIVE_PATH_MAX];       /* S/layers/<hex>/diff (overlay lowerdir) */
} NativeLayer;

typedef struct {
    char   name[MAX_IMAGE_NAME];      /* original repo name (unmangled) */
    char   tag[MAX_IMAGE_NAME];
    int    nlayers;
    unsigned long long size;          /* total layer-blob bytes */
} NativeImage;

/* Store root: honors PODVM_STORE_DIR, else $XDG_DATA_HOME/podvm/store. */
const char *native_store_dir(void);

/* Is ref present in the native store (a manifest exists)? 0 = yes. */
int native_has(const char *image_str);

/* Resolve ref to its layer chain, TOP-first (overlayfs lowerdir order --
 * the reverse of the manifest's bottom-first list). Returns the layer
 * count, or -1 on error/absent. */
int native_chain(const char *image_str, NativeLayer *layers, int max);

/* Load ref's image config (Env/Entrypoint/Cmd/WorkingDir/User). 0 on
 * success; -1 if absent (image carries no defaults). */
int native_config(const char *image_str, ImageConfig *c);

/* Commit a diff directory as a layer: pack to a blob, unpack to
 * layers/<hex>/diff (skipped if the blob already exists -- dedup). Writes
 * "sha256:<hex>" to out_digest. 0 on success. */
int native_commit_diff(const char *diff_dir, char *out_digest, size_t n);

/* Store raw bytes (e.g. a config JSON) as a blob; write its digest. Used
 * by pull (Phase 2) and tests to plant a config. 0 on success. */
int native_blob_put(const char *data, size_t len, char *out_digest, size_t n);

/* Overlay lowerdir path for a layer digest (S/layers/<hex>/diff). */
void native_diff_path(const char *digest, char *out, size_t n);

/* Store an uncompressed layer tar (already in memory, e.g. a registry
 * blob) as a layer: blob + unpack to layers/<hex>/diff (dedup by digest).
 * The pull-side counterpart to native_commit_diff. Writes the digest. */
int native_put_layer(const char *tardata, size_t len, char *out_digest, size_t n);

/* Enumerate manifests for `podvm images`. Returns the count filled. */
int native_list(NativeImage *ents, int max);

/* Remove an image's manifest (the image is then absent; its layer blobs
 * remain until GC). Returns 0 on success, -1 if absent/error. */
int native_rmi(const char *image_str);

/* Save ref as a docker-archive tar at out_path (loadable by podman/docker
 * load). format: 0 = docker-archive (only format supported for now).
 * Returns 0 on success. */
int native_save(const char *image_str, const char *out_path, int format);

/* Load a docker-archive tar into the store (the podman/docker-save
 * counterpart). Returns 0 on success. */
int native_load(const char *tar_path);

/* Garbage-collect: remove blobs + layer dirs not referenced by any
 * manifest (native rmi and failed builds leave orphans). Returns 0; sets
 * *freed (optional) to the number of entries removed. */
int native_gc(int *freed);

/* Create/replace ref's manifest: config digest + ordered layer digests
 * (bottom-first) + optional history lines. 0 on success. */
int native_manifest_put(const char *image_str, const char *config_digest,
                        const char *const *layer_digests, int nlayers,
                        const char *const *history, int nhist);

#endif /* STORE_NATIVE_H */
