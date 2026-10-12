/*
 * subuid.h - subuid-range id translation (ADR-028)
 *
 * podvm keeps file ownership as metadata (9p mapped-xattr), so the
 * host never has to represent uids the invoking user doesn't own.
 * This module translates between ON-DISK uids (what rootless podman
 * stores: invoking user = container root, subuid range = the rest)
 * and GUEST uids (0 = root, 1.. = the container's users).
 */
#ifndef SUBUID_H
#define SUBUID_H

/*
 * Parse an /etc/subuid-style file ("<name>:<base>:<count>") for user
 * `name`. Returns 0 and fills base/count, or -1 when there is no entry.
 * Exposed separately so tests can point it at a fixture file.
 */
int subuid_parse_file(const char *path, const char *name,
                      unsigned long *base, unsigned long *count);

/* The invoking user's subuid allocation (/etc/subuid). Cached. -1 when
 * absent -- callers then collapse every id to guest root. */
int subuid_self(unsigned long *base, unsigned long *count);

/*
 * Translate a HOST uid/gid to the guest id: invoking user -> 0,
 * subuid-range -> range-relative (base -> 1), anything else -> 0.
 */
unsigned subuid_to_guest(unsigned long host_id);

/*
 * Inverse: guest id -> host id (invoking user), for commit-side
 * sanity checks and tests. Not used on the hot path (the xattr is
 * already in guest ids).
 */
unsigned long subuid_from_guest(unsigned guest_id);

#endif /* SUBUID_H */
