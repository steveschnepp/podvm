/*
 * image.h - OCI image handling
 *
 * Pull and resolve container images.
 */

#ifndef IMAGE_H
#define IMAGE_H

#include <stddef.h>

#define MAX_IMAGE_NAME 256
#define MAX_PATH 1024

/* Image reference */
typedef struct {
    char name[MAX_IMAGE_NAME];      /* e.g., "debian" or "registry/repo:tag" */
    char tag[MAX_IMAGE_NAME];       /* e.g., "latest", "12" */
    char digest[128];               /* e.g., "sha256:abc..." */
    char rootfs[MAX_PATH];          /* Path to assembled rootfs */
} ImageRef;

/* Limits on an image config's command/env arrays. */
#define MAX_ENV_VARS  64
#define MAX_CMD_PARTS 64

/*
 * OCI image config (the "config" object of an image's config blob).
 * This is what lets `podvm <image>` run an image's default command
 * without the user typing one (ADR-005: an image carries its own
 * default command).
 *
 * Owned strings; init before parse, free after use.
 */
typedef struct {
    char *env[MAX_ENV_VARS];        /* "K=V" strings */
    int   nenv;
    char *entrypoint[MAX_CMD_PARTS];
    int   nentry;
    char *cmd[MAX_CMD_PARTS];
    int   ncmd;
    char  cwd[MAX_PATH];            /* WorkingDir ("" if unset) */
    char  user[128];                /* User, e.g. "1000:1000" ("" if unset) */
} ImageConfig;

/* Zero an ImageConfig (call before image_config_parse). */
void image_config_init(ImageConfig *c);

/* Free strings held by an ImageConfig (safe to call twice). */
void image_config_free(ImageConfig *c);

/*
 * Parse an OCI config blob (the JSON whose top level has a "config"
 * object) into an ImageConfig. Missing fields are left empty.
 * Returns 0 on success, -1 if the text is not a usable config blob.
 */
int image_config_parse(const char *json_text, ImageConfig *c);

/*
 * Load a cached image's config blob into c (init before, free after).
 * Returns 0 on success, -1 if absent/unparseable.
 */
int image_config_load(const char *image_str, ImageConfig *c);

/*
 * Parse image name (e.g., "debian:latest" -> name="debian", tag="latest")
 * Returns 0 on success.
 */
int image_parse(const char *image_str, ImageRef *ref);

/*
 * Check if string is a directory path (vs image name)
 * Returns 1 if path, 0 if image name.
 */
int image_is_path(const char *str);

/*
 * Resolve image to rootfs directory.
 * If image is a path, use directly.
 * If image is a name, look in cache or pull.
 * Returns 0 on success (rootfs_path holds a single merged dir),
 * IMAGE_RESOLVED_NATIVE when the image lives in podvm's layered store
 * (rootfs_path is NOT set; the caller stacks the chain via the native
 * store), or -1 on error.
 */
#define IMAGE_RESOLVED_NATIVE 1
int image_resolve(const char *image_str, char *rootfs_path, size_t path_size);

/*
 * Pull image from registry (simplified POC).
 * Returns 0 on success.
 */
int image_pull(const char *image_name, const char *tag);

/*
 * Get image cache directory
 */
const char *image_cache_dir(void);

/*
 * Cache management (podvm images / podvm rmi).
 *
 * The cache holds one directory per <registry>/<repo>/<tag> with a
 * flattened rootfs and (usually) a .podvm-config.json. Built images
 * (podvm build) land in the same layout, so run/images/rmi see one
 * cache.
 */

typedef struct {
    char name[MAX_IMAGE_NAME];       /* display registry/repo (normalized) */
    char tag[MAX_IMAGE_NAME];
    char id[16];                     /* 12 hex, derived from the cache path */
    char path[MAX_PATH];             /* absolute cache dir */
    unsigned long long size;         /* bytes used (recursive) */
} ImageCacheEntry;

/*
 * Map an image reference to its cache directory (no network, no pull).
 * Returns 0 on success, -1 for a path reference or an unparseable ref.
 */
int image_cache_path(const char *image_str, char *out, size_t n);

/*
 * List cached images (entries filled up to max). Returns the count
 * (0 = empty cache), -1 on error. Display names are normalized:
 * docker.io/registry-1.docker.io and library/ are dropped.
 */
int image_cache_list(ImageCacheEntry *ents, int max);

/*
 * Per-image locking, the mirror of the container-name guard: a run
 * that serves a cached rootfs to a VM holds a SHARED lock, and rmi
 * takes an EXCLUSIVE one, so a live container's image cannot be
 * deleted out from under it. Lock files live in <cache>-locks/ (a
 * sibling of the cache, never inside a deletable image dir) and are
 * never deleted -- same inode-is-identity rule as container locks.
 *
 * Returns the fd (keep open for the run's lifetime; NOT CLOEXEC --
 * the QEMU child inherits it), or -1; errno is EWOULDBLOCK for the
 * exclusive case when the image is in use.
 */
int image_lock(const char *image_str, int exclusive);

#endif /* IMAGE_H */
