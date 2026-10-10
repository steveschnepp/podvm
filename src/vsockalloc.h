/* vsockalloc.h - allocate a unique guest CID and exit-report port.
 *
 * vsock resources are host-global: the kernel refuses a second
 * vhost-vsock device with the same guest-cid, and two listeners cannot
 * bind the same host vsock port. podvm needs one of each per live VM, so
 * concurrent runs must not collide. Allocation is a non-blocking flock
 * over a per-number file in the lock dir -- the same pattern as the
 * container-name lock (see container.c): the file is never deleted, the
 * lock (not the file) is the reservation, and the fd is kept open so the
 * number stays taken exactly as long as the holder lives.
 *
 * The CID fd is deliberately not CLOEXEC, so a forked QEMU inherits it
 * and holds the CID for the life of the VM (even after the podvm process
 * exits, e.g. under --detach).
 */
#ifndef VSOCKALLOC_H
#define VSOCKALLOC_H

/* Lock dir: honors PODVM_VSOCK_LOCKDIR (tests point it at a temp dir),
 * else the host-global /tmp/podvm/vsock-locks. */
const char *vsock_lockdir(void);

/* First free guest CID >= 3 (0 = hypervisor, 2 = host are reserved).
 * Returns the CID, or -1 if the range is exhausted. The reservation fd
 * is kept open for the life of the process. */
int vsock_alloc_cid(void);

/* First free exit-report port >= 5001. Returns the port, or -1 if the
 * range is exhausted. The reservation fd is kept open for the life of
 * the process. */
int vsock_alloc_port(void);

#endif
