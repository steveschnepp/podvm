/*
 * container.h - named-container state (write-back layer, ADR-026)
 *
 * Pure host-side helpers shared by `podvm run --name/--rm` and the
 * `podvm rm` subcommand. Kept out of main.c so the unit tests link the
 * real code instead of a copy (same reasoning as imgcfg.c/userdb.c).
 */
#ifndef CONTAINER_H
#define CONTAINER_H

#include <stddef.h>

#define CONTAINER_PATH_MAX 1024

/*
 * A container name becomes a directory name under the state root, so it
 * must not be able to escape it: no '/', not "." or "..". Also rejects
 * an empty name and a leading '-' (which `podvm rm` would otherwise read
 * as an option).
 *
 * Returns 1 if usable, 0 otherwise.
 */
int container_name_valid(const char *name);

/*
 * Host dir holding a named container's writable layer (the overlay
 * upper + work), under $XDG_STATE_HOME or ~/.local/state.
 *
 * An empty (or NULL) name yields out[0] = '\0' and success: "no
 * persistent state", the caller falls back to a throwaway tmpfs upper.
 * Returns -1 when neither XDG_STATE_HOME nor HOME is set (out is then
 * also empty).
 */
int container_state_dir(const char *name, char *out, size_t n);

/*
 * Recursively delete a tree (like `rm -rf path`). Symlinks are removed,
 * never followed (FTW_PHYS), so the walk cannot escape the tree.
 *
 * Returns 0 on success. Returns -1 with errno set on failure; errno is
 * ENOENT when the path did not exist.
 */
int container_rm_rf(const char *path);

#endif /* CONTAINER_H */
