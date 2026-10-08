/*
 * store.h - podman storage backend
 *
 * Resolve image refs against podman's on-disk rootless storage.
 */

#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include "image.h"

#define STORE_PATH_MAX 1024

typedef struct {
    char diff[STORE_PATH_MAX];   /* <storage>/overlay/<id>/diff */
} StoreLayer;

/* Find an image's top layer id by name. Returns 0 on success. */
int store_find(const char *image, char *top_layer, size_t n);

/*
 * Walk the layer chain top-first (overlayfs lowerdir order).
 * Returns layer count, or -1 on error.
 */
int store_chain(const char *top_layer, StoreLayer *layers, int max);

/* Is podman available (to pull missing images)? */
int store_podman_available(void);

/* Pull an image via `podman pull` (blocks). Returns 0 on success. */
int store_podman_pull(const char *image);

/* Find, pulling via podman if missing and allow_pull.
 * Returns 0 on success with top_layer set.
 */
int store_resolve(const char *image, char *top_layer, size_t n,
                  int allow_pull);

/*
 * Read an image's OCI config blob (Env/Entrypoint/Cmd/WorkingDir/User)
 * from podman storage and parse it into cfg (init before, free after).
 * Returns 0 on success, -1 if no config blob could be read.
 */
int store_image_config(const char *image, ImageConfig *cfg);

#endif /* STORE_H */
