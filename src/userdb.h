/*
 * userdb.h - resolve a podvm.user spec to uid:gid
 *
 * Reads a /etc/passwd and /etc/group (paths are passed in, so after
 * chroot the caller points them at the *container's* databases, matching
 * docker: an image's User is resolved against the image, not the host).
 *
 * Hand-rolled rather than getpwnam/getgrnam: the guest links statically
 * (musl when available, glibc otherwise) and NSS under static linking is
 * fragile. A passwd/group line is three colon-separated fields; parsing
 * it is ~40 lines with no dependency.
 */

#ifndef USERDB_H
#define USERDB_H

/*
 * Resolve spec into *uid and *gid.
 *
 * Accepted forms (docker semantics):
 *   "name"        -> uid/gid from the passwd entry's primary group
 *   "uid"         -> that uid; gid from the passwd entry matching it, or 0
 *   "name:group"  -> name resolved; gid from the group entry
 *   "name:gid"    -> name resolved; gid as given
 *   "uid:group"   -> uid as given; gid from the group entry
 *   "uid:gid"     -> both as given
 *
 * Returns 0 on success. Returns -1 if the user (or a named group) cannot
 * be found -- the caller decides whether to fall back to root.
 */
int userdb_resolve(const char *passwd_path, const char *group_path,
                   const char *spec, int *uid, int *gid);

#endif /* USERDB_H */
