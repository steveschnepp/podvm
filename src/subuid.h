/*
 * subuid.h - the invoking user's subuid/subgid allocation (ADR-028, path B)
 *
 * podvm maps ONE uid per VM by default, which is why cross-uid chown in
 * the guest fails: the overlay's userns knows only uid 0, so a chown to
 * any other uid has no mapping and the kernel rejects it (EINVAL). Path
 * B fixes that by mapping a uid RANGE -- the invoking user -> 0, plus the
 * user's subuid allocation -> 1..N -- so guest uids land on real (but
 * still unprivileged, subuid-owned) host files. The guest asks over 9p,
 * the host executes, and ownership now round-trips.
 *
 * The multi-line uid_map an unprivileged process cannot write itself:
 * writing a range needs CAP_SETUID in the *parent* namespace, which only
 * the setuid newuidmap/newgidmap helpers have. So the PARENT process
 * writes the child's map via these helpers after the child unshares.
 */
#ifndef PODVM_SUBUID_H
#define PODVM_SUBUID_H

#include <sys/types.h>

/* A subuid/subgid allocation: [base, base+count) available for mapping. */
typedef struct {
    unsigned uid_base, uid_count;
    unsigned gid_base, gid_count;
} SubUidAlloc;

/*
 * Read the invoking user's allocation from /etc/subuid + /etc/subgid.
 * Returns 1 if a usable range was found in BOTH (alloc filled), 0 if the
 * user has no allocation (caller keeps the single-uid map -- today's
 * behavior). A 0 here is not an error: hosts without subuid setup keep
 * working, just without cross-uid chown.
 */
int subuid_read_alloc(SubUidAlloc *alloc);

/*
 * Write `child`'s uid_map + gid_map, mapping:
 *     ns 0          -> the invoking uid/gid   (guest root)
 *     ns 1..count   -> the subuid/subgid range (guest uids)
 *
 * via setuid newuidmap/newgidmap. The child MUST have already unshared
 * (CLONE_NEWUSER) -- this is called by the parent. setgroups is denied
 * first (required before any gid_map write; harmless to newgidmap).
 *
 * Returns:
 *      0  range map fully applied;
 *     -1  NOTHING was written (helper missing/refused the uid map) --
 *         the caller may safely fall back to subuid_map_child_single;
 *     -2  uid_map WAS written but the gid map failed -- the caller must
 *         NOT fall back (uid_map is write-once; a retry would hit EPERM
 *         on the already-written map). Treat as fatal.
 */
int subuid_map_child(pid_t child, const SubUidAlloc *alloc);

/*
 * Write `child`'s uid_map + gid_map as the single-uid map
 * (0 -> invoking uid). Used when there is no subuid allocation or the
 * range map failed. The parent writes it directly -- an unprivileged
 * parent may map its own uid into a child it forked, no CAP_SETUID
 * needed. Returns 0 on success, -1 on failure.
 */
int subuid_map_child_single(pid_t child);

#endif /* PODVM_SUBUID_H */
