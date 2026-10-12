# ADR-028: Guest-Side Ownership (Overlay in the Guest, Not UID Translation)

**Date:** 2026-10-12

**Status:** Proposed

## Context

podvm maps exactly one uid per VM. The QEMU child unshares
user+mount, writes `0 <invoking-uid> 1` into its own `uid_map`
(`setup_userns_overlay()`, `src/main.c`), mounts the overlay inside
that namespace, and execs QEMU there; 9p serves the merged rootfs with
`security_model=none`.

Two consequences, both visible in the field:

1. **Cross-uid `chown` fails in the guest** with `Invalid argument`.
   Guest-root's `chown(N)` becomes a real `chown()` by QEMU against
   the overlay, checked against the userns that owns the mount -- whose
   map contains only uid 0. Every Dockerfile that does `RUN chown -R
   app:app /app` before `USER` breaks (hit concretely building
   mermaid-ascii; see `2026-10-12_multistage-build.md`).
2. **Image ownership is discarded at ingest.** The unpack userns maps
   one uid and the tar writer hardcoded uid 0, so every layer file
   lands owned by the invoking user and the guest sees `root:root`
   everywhere. `USER`, setuid bits, and multi-user images cannot work.

## Decision

**Ownership becomes guest-side state. The host filesystem never
represents uids the invoking user doesn't own.**

Concretely, the rootfs 9p share turns into a **read-only lower** and
the writable overlay is mounted **inside the guest** (tmpfs upper):

- `chown`/`chmod`/`USER`/setuid are guest-kernel operations on a
  guest-owned tmpfs -- no host check, no uid map, no xattr limits.
  They work on every host, with no `newuidmap`/`/etc/subuid`
  requirement.
- The synthetic kernel gains `CONFIG_OVERLAY_FS` (+ tmpfs, already
  present); `/init` mounts `overlay` with the 9p rootfs as lowerdir
  and a tmpfs upper/work pair, same as any container runtime mounts
  its rootfs -- except the runtime here is init, and the VM is the
  isolation boundary (ADR-001), so nothing outside can observe or
  care about the ids inside.
- Where ownership must cross the VM boundary -- build-step commits
  and named-container write-back (ADR-026) -- the guest ships its
  upper back **as a tar with uid/gid metadata** (ustar header fields;
  `src/tar.c` already stamps/reads `user.v9fs.uid/gid` xattrs as the
  on-disk sidecar for that tar). The tar *is* the metadata file.

### Ingest / commit

- **Ingest** stamps ownership metadata (xattr beside the data) from
  the tar headers, so commit and save can reproduce it.
- **Commit** reads that metadata back; podman-owned diffs (subuid
  ids on disk, no xattrs) translate via the subuid allocation
  (`src/subuid.c`: invoking user -> 0, subuid range -> range-relative).

### Fallback

Hosts/kernels without overlayfs in the guest kernel -- i.e. a podvm
kernel built without `CONFIG_OVERLAY_FS` -- degrade to today's
behavior: host-side merged view, chown limited to root. Loud warning,
podvm keeps running.

## Exploration: why not metadata in 9p (falsified)

The first cut of this ADR chose QEMU 9p's `security_model=mapped-xattr`
-- ownership as `user.v9fs.uid/gid` xattrs beside the data, guest
chown becomes an xattr write server-side. Implemented and tested:
**both `mapped-xattr` and `mapped` return `ELOOP` for every symlink
in the image** (`exec /bin/sh` -> `Symbolic link loop`; alpine's whole
`/bin` is symlinks to busybox). The modes must record ownership
metadata for symlinks too, and ext4/tmpfs don't support xattrs on
symlinks. Structural, not a bug we could configure around. The
tar-side xattr stamping from that attempt survives -- it is exactly
the commit/save metadata this decision needs. The commit is in git
history (`wip: uid ownership via 9p mapped-xattr (falsified)`) and
reverted immediately after.

The other rejected direction: **subuid-range mapping via newuidmap**
(the original ADR-028). Proven (rootless podman's approach) and free
podman-store interop, but it makes the host filesystem represent
container uids (every store/state file becomes subuid-owned), adds a
soft host requirement (setuid helper + allocation, with an AppArmor
history on older Ubuntus), and pushes translation to the serve side --
the opposite of where this project wants the boundary (ADR-001,
ADR-024: the VM is the boundary; plumbing should not carry meaning).
Kept as a live fallback option if the guest-side overlay proves too
costly in the performance phase.

## Implications

- `guest/init.c`: mount the overlay (lower = 9p rootfs read-only,
  upper+work = tmpfs), sync the upper back on exit (tar over vsock or
  a 9p staging dir) when the run is named (write-back) or the step is
  a build commit. PID 1 keeps owning the exit-code report.
- `kernel/config`: `CONFIG_OVERLAY_FS=y` (built-in; modules are not
  loaded in the guest).
- `src/main.c`: rootfs 9p share gains `readonly=on` always; the
  host-side overlay for the rootfs goes away (volumes keep their
  current per-volume 9p shares; they are host dirs by definition).
- `src/tar.c` / `src/subuid.c`: the ingest/commit metadata path from
  the exploration, as described above.
- `tests`: an e2e that chowns in a build step, runs the image with
  `-u`, and asserts ownership from inside the guest; write-back e2e
  gains an ownership-persistence assertion.
- The host-side userns overlay remains only for podman-store
  *staging* (mounting untrusted images read-only to serve them), not
  for ownership.
