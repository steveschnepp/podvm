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
 * Returns 0 on success.
 */
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

#endif /* IMAGE_H */
