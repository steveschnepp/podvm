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

/*
 * Serialize live runs per container name. overlayfs cannot serve two
 * writers on one upper, so a named run must hold an exclusive,
 * non-blocking flock while its VM is up.
 *
 * The lock file lives at <base>/locks/<name> -- a sibling of
 * <base>/containers/, NOT inside the state dir -- and is never deleted:
 * its inode is the name's identity, so `rm`/`--rm` removing the state
 * dir can never race a starting run into locking a fresh file.
 *
 * The returned fd must stay open and must NOT be made CLOEXEC: the
 * QEMU child inherits it, so the lock lives exactly as long as the VM
 * even if the launcher dies first.
 *
 * Returns the fd (>= 0) on success, -1 on failure; errno is
 * EWOULDBLOCK when another run (or a concurrent `rm`) holds the lock,
 * EINVAL for an invalid name.
 */
int container_lock(const char *name);

/*
 * Non-creating probe: is anything holding this name's lock?
 *
 * Returns 1 if the lock is held (the container is live), 0 if it is
 * free (including when no lock file exists -- a state dir from before
 * the guard existed still counts as not-running), -1 on other errors
 * (e.g. invalid name).
 */
int container_locked(const char *name);

#endif /* CONTAINER_H */
