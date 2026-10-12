---
name: uid-mapping-exploration
description: three attempts at guest file ownership (chown/USER) -- subuid mapping designed, metadata-via-9p implemented and falsified by symlink ELOOP, pivoted to guest-side overlay; code landed reverted, finding kept in git
project: podvm
---

# Mission Log: The UID Mapping Exploration

---

## The Question

Building mermaid-ascii (previous log) hit a wall: `RUN chown -R
mermaid:mermaid /app` fails in every podvm VM with
`chown: Invalid argument`. The workaround (`|| true`) shipped, and the
user came back with the follow-up we owed the project: **"we need to
think of how to map uids."**

## The Diagnosis (precise, before any design)

Traced it to the exact syscall chain, which turned out to matter:

- The QEMU child unshares user+mount and writes a **single-uid map**:
  `0 <invoking-uid> 1` (`setup_userns_overlay()`).
- 9p `security_model=none`: guest-root's `chown(N)` becomes a real
  `chown()` by QEMU against the overlay mount -- checked against the
  **userns that owns that mount**. The map has one entry (uid 0);
  `chown(0,0)` is a no-op that passes, `chown(1000)` is unmapped ->
  `EINVAL`.
- And worse, discovered on inspection: ownership was already being
  **discarded at ingest** -- the store unpack used the same 1-uid map
  and `hdr_build()` hardcoded uid 0, so the guest sees `root:root`
  everywhere. `USER` was decorative.

## Attempt 1: Subuid Mapping (designed, ADR drafted, not coded)

The rootless-podman answer: map `guest 0 -> user`, `guest 1..65536 ->
subuid range` via the setuid `newuidmap`. Host already had the
machinery (`snide:165536:65536`, helpers installed). Wrote ADR-028 in
this shape and started `src/subuid.c` (parse + fork dance for the
parent-installs-maps handshake).

Then the user pushed back with the better framing: **"we don't need to
leverage the uid:gid on the host. we can keep them in a metadata file
next by?"** -- i.e. why make the host filesystem represent container
uids at all? Fair. That design was abandoned before any of it was
wired into the build.

## Attempt 2: Ownership as Metadata in 9p (coded, tested, FALSIFIED)

The metadata idea has a stock implementation: QEMU 9p
`security_model=mapped-xattr` -- uid/gid live in `user.v9fs.uid/gid`
xattrs beside the data, guest chown becomes an xattr write QEMU does
server-side. No newuidmap, no subuid requirement, nothing on disk
owned by anyone but the invoking user. Rewrote ADR-028 around it,
implemented it end to end:

- `tar.c`: extract stamps the xattrs from tar headers; pack reads
  them (falls back to subuid translation for podman-owned diffs).
- `subuid.c`: slimmed to a pure id-translation module.
- `main.c`: rootfs fsdev -> `mapped-xattr`.

**Test result:**

```
podvm-init: exec /bin/sh
podvm-init: exec failed: Symbolic link loop
```

Every symlink in the image returns `ELOOP` -- alpine's entire `/bin`
is symlinks to busybox, so nothing execs, `stat` on a symlink fails,
even plain `podvm run alpine` dies. Checked the sibling mode
(`mapped`): identical failure. Root cause is structural: the mapped-*
modes must carry ownership metadata **for symlinks too**, and
ext4/tmpfs don't support xattrs on symlinks. Not configurable around.
A container rootfs is symlink-dense by nature -- dead end, full stop.

**This is the log's headline lesson:** we could have read QEMU's
source or docs and maybe found it, but a two-minute VM test settled it
against the real workload. When a design hinges on a transport's
corner case, test the corner case FIRST (symlinks, hardlinks,
whiteouts, sparse files -- the things real images are made of).

## Attempt 3: Guest-Side Overlay (chosen, not yet coded)

The pivot that actually fits podvm's axioms: **ownership becomes
guest-side state.** 9p rootfs turns into a read-only lower; the
writable overlay is mounted *inside the guest* (tmpfs upper);
`chown`/`USER`/setuid are then guest-kernel operations with no host
check, no uid map, no xattr limits -- and they work on every host.
Where ownership must cross the boundary (build commits, write-back),
the guest ships its upper back as a **tar with uid metadata** -- and
the tar *is* the metadata file from the user's original framing.

It follows from ADR-001/ADR-024 (the VM is the isolation boundary;
plumbing must not carry meaning): instead of translating uids across
the boundary, stop having them on the host side of it at all. Cost:
`CONFIG_OVERLAY_FS` in the synthetic kernel, an overlay+sync protocol
in `guest/init.c`, and the host-side rootfs overlay retiring.

## Git Hygiene (user's call, worth repeating)

The user asked for the exploration to be **committed then reverted**
-- codebase clean at HEAD, history keeping the whole arc:

- `b668067` wip: the mapped-xattr implementation + falsification note
- `649ed24` revert of the code; ADR-028 (now describing the
  guest-side overlay decision + the falsified exploration) kept in
  tree as the record

So when implementation starts, the tar xattr stamping and the subuid
translation module are one `git show` away, already tested for what
they were.

## Carried Forward

- **Implement ADR-028**: overlay in the guest, upper sync-back,
  restore the `b668067` metadata code for ingest/commit. Acceptance
  test: the mermaid-ascii Dockerfile builds **unmodified** (chown and
  all).
- Write-back e2e gains an ownership-persistence assertion.
- Note for whoever picks up virtiofs (ADR-022 performance phase): it
  supports idmapped mounts properly and would revive a serve-side
  design if the guest-overlay sync ever proves too costly.
