# ADR-028: Subuid-Range UID Mapping (Rootless podman Parity)

**Date:** 2026-10-12

**Status:** Proposed

## Context

podvm maps exactly one uid per VM. The QEMU child unshares
user+mount, writes `0 <invoking-uid> 1` into its own `uid_map`
(`setup_userns_overlay()`, `src/main.c`), mounts the overlay inside
that namespace, and execs QEMU there. The store unpack
(`unpack_layer_in_userns()`, `src/store_native.c`) uses the same
single-uid map when it unpacks layer tars.

Two consequences, both visible in the field:

1. **Cross-uid `chown` fails in the guest** with `Invalid argument`.
   9p runs `security_model=none`, so guest-root's `chown(N)` becomes a
   real `chown()` by QEMU against the overlay -- checked against the
   *userns that owns the mount*, whose map contains only uid 0. Any
   `N != 0` is unmapped -> `EINVAL`. Every Dockerfile that does
   `RUN chown -R app:app /app` before `USER` breaks (hit concretely
   building mermaid-ascii; see `2026-10-12_multistage-build.md`).
2. **Image ownership is discarded at ingest.** Because the unpack
   userns maps one uid, every file in a layer lands on disk owned by
   the invoking user, and the guest sees the entire rootfs as
   `root:root`. `USER`, setuid bits, and multi-user images cannot
   work; `podvm run -u` resolves the user but nothing on disk
   honors it.

ADR-024's principle stands -- the VM is the isolation boundary, the
userns staging is plumbing -- but the plumbing is currently too thin
to carry the ownership information that images contain.

## Decision

**Map a full subuid range, exactly as rootless podman does.**

Read the invoking user's allocation from `/etc/subuid` and
`/etc/subgid` (e.g. `snide:165536:65536`) and install a two-range map
in the QEMU child's userns, via the setuid `newuidmap`/`newgidmap`
helpers (multi-range maps cannot be self-written without privileges):

```
guest uid 0        -> invoking uid          (today's mapping, kept)
guest uid 1..65536 -> subuid base + 0..65535
```

The store unpack uses the same map, so layers keep their numeric
ownership on disk (subuid-owned files in the store, identical to
podman's graphroot). With ADR-024, this makes mounting podman's diff
dirs *more* correct, not less: those files are already subuid-owned
and will now render in the guest with their true image uids.

### Fallback

No `/etc/subuid` entry, no `newuidmap`/`newgidmap`, or the helper
failing (AppArmor restrictions on some older hosts) -> keep today's
single-uid map, print one loud warning that cross-uid chown and image
ownership are degraded. podvm must keep working everywhere; this is a
capability, not a requirement.

## Rationale

- **It's the ecosystem's answer.** rootless podman, rootless LXC, and
  Buildah all solve rootless cross-uid ownership with the subuid
  mechanism. Matching their mapping convention means podman-stored
  images (ADR-024) are directly usable with correct ownership, and
  users who already run rootless podman already have the config.
- **The kernel work is done and stable.** `newuidmap`/`newgidmap` are
  standard (shadow-utils), `/etc/subuid` is allocated by every major
  distro's user-creation path; we consume, not invent.
- **The alternative directions were checked and rejected** (below).
- **The degradation path is graceful.** The single-uid map is a
  special case of the design, not a fork of it -- same code path with
  a one-entry map.

## Implications

- **Fork-dance reorder in `setup_userns_overlay()`.** The uid_map
  write moves from "child writes its own map" (legal only for the
  invoking uid) to "parent calls `newuidmap <child-pid>` after the
  child unshares and blocks". The gid dance follows, still after
  `setgroups=deny`. ~40 lines of new helper (`src/subuid.c`), plus a
  small parse of `/etc/subuid`.
- **State dirs gain subuid-owned files.** Container uppers and store
  diffs will contain files owned by uids 165536+. They stay
  deletable (directory write is yours), readable (0644), and
  invisible to `ls` mapping-wise -- the same lived experience as any
  rootless podman user. Documented, not defended.
- **Whiteouts are unaffected.** Char 0:0 devices still require only
  uid 0 in the owning userns, which the map keeps.
- **`podvm save`/`load` OCI interop improves.** Ownership now
  round-trips through the tar layer instead of being flattened.
- **A new soft host requirement** (subuid + newuidmap) for full
  functionality; README requirements section gets a line.
- **Test surface**: an e2e that builds a Dockerfile with
  `RUN chown -R <uid> /app`, runs it with `-u`, and asserts the
  ownership and writability from inside the guest; plus a unit-ish
  test for the `/etc/subuid` parser and the fallback path.

## Alternatives Considered

**Idmapped mounts (`mount_setattr`, `MOUNT_ATTR_IDMAP`).** The
modern kernel answer, but the 9p client gives no mount-side hook for
the *server's* view: QEMU performs the chown against the host
filesystem, and an idmapped mount there would need to be set up
outside our userns with privileges we don't have. Would also pin us
to newer kernels for zero gain over newuidmap. Revisit only if
newuidmap proves unshippable in practice.

**Nested userns inside the guest.** Does not help: the ownership
check happens on the host side of 9p regardless of what the guest
does internally.

**Teach /init to fake it (chown interception, LD_PRELOAD in the
guest).** Fragile, transparently wrong for anything stat'ing
ownership, and breaks the "no guest runtime" principle (ADR-001).

**Run QEMU as the real root via a setuid helper.** Rejected outright:
it moves the trust boundary the project is built on (ADR-001,
ADR-024).

**Status quo + `|| true` in affected Dockerfiles.** Rejected as the
plan of record; kept only as the documented fallback for hosts
without newuidmap.
