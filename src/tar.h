/*
 * tar.h - ustar/GNU tar reading and writing
 *
 * The reader was extracted from image.c so the layered store can unpack
 * a committed layer without linking the registry client (curl/zlib/zstd).
 * The writer packs a directory tree into a tar -- used to commit a build
 * step's diff and by `podvm save`.
 */
#ifndef TAR_H
#define TAR_H

#include <stddef.h>

/* Extract a ustar/GNU tar image in memory into root (mkdir -p as needed).
 * Handles directories, regular files, symlinks, hardlinks, and overlay
 * whiteouts (.wh.<name>, .wh..wh..opq). Returns 0 on success. */
int tar_extract_mem(const char *tar, size_t len, const char *root);

/* Layer unpack for the native store: whiteouts become char 0:0 devices
 * (overlayfs honors them when layers are stacked as lowerdirs). Must run
 * inside a userns so mknod of a char device succeeds. */
int tar_extract_layer(const char *tar, size_t len, const char *root);

/* Recursively pack the directory tree at root into a ustar tar written to
 * out_path. Emits directories, regular files, and symlinks; an overlay
 * whiteout (char 0:0 device) becomes a portable .wh.<name> entry. Names
 * longer than the ustar fields allow are written as GNU longname ('L').
 * Returns 0 on success. */
int tar_pack_dir(const char *root, const char *out_path);

#endif /* TAR_H */
