---
name: subuid-ownership-path-b
description: ADR-028's decision reversed -- chose subuid range mapping (B, keeps guest-asks/host-executes) over guest-side overlay (C); empirically de-risked the kernel mechanism first, then implemented the fork handshake; cross-uid chown now works
project: podvm
previous: 2026-10-14_warning-root-cause-sweep.md
---

# Mission Log: ADR-028 Path B (Subuid Ownership)

---

## The Ask

Continuing the queued next-steps, item 2 was "ADR-028 guest-side overlay."
Investigating it turned into a genuine architecture fork that consumed
most of the session, and ended in a **decision reversal**: the project did
NOT build the guest-side overlay the ADR specifies. It built subuid range
mapping instead -- the option the ADR explicitly rejected -- because that
option is the one that preserves the project's defining model.

---

## What We Did

### 1. The premise correction (before any code)

I initially flagged ADR-028 as blocked on a guest-kernel rebuild, because
`kernel/config` says `# OVERLAY_FS - host handles this`. That was **wrong**
and worth recording: podvm boots the **host's** kernel
(`get_host_kernel()` -> `/boot/vmlinuz-$(uname -r)`, here Debian 6.12.107),
not a podvm-built one. `make kernel` is a stub (no source in repo). The host
kernel already has `CONFIG_OVERLAY_FS=m`, and I verified `overlay.ko.xz`
decompresses and is bundleable into the initramfs. So ADR-028 was NOT
kernel-blocked on the default path -- the `# OVERLAY_FS` comment applies to
the non-default synthetic kernel only. I corrected this to the user rather
than let a bad premise drive the decision.

### 2. The architecture fork

The user's framing -- "the guest asks, the host executes" -- named ADR-002's
shipped model exactly, and exposed the real question. The overlay is mounted
in a userns whose uid_map is ONE line (`0 <invoking-uid> 1`,
`main.c:setup_userns_overlay`). A guest `chown app:app` becomes a 9p request
the host executes inside that namespace, which only knows uid 0 -- so any
other uid is EINVAL. That is not a missing feature; it is the *rootless*
security property ("you may only own files as yourself") surfacing through
the ask/executes channel.

Three places to fix ownership, only one of which keeps ask/executes:

| | who executes | ownership | cost |
|---|---|---|---|
| A. current | host (9p) | no (single-uid) | clean, thin guest |
| **B. subuid range** | host (9p) | yes | uid-map plumbing; host fs holds subuid-owned files |
| C. overlay in guest | guest (tmpfs) | yes | guest overlay module + tar write-back |

ADR-028 had chosen C and rejected B. The user chose **B**, on the grounds
that it keeps "the guest asks, the host executes" intact (C inverts it --
writes move into the guest) and is far cheaper.

### 3. The 9p-server option, explored and rejected

Before settling, the user asked whether we could "hack the 9p server." That
is the intellectually clean continuation of ask/executes. Two versions:

- **Cheap:** QEMU's built-in `security_model=mapped-xattr`/`mapped` -- this
  is literally designed for guest-chown-becomes-server-side-xattr. Already
  tried (`b668067 wip: uid ownership via 9p mapped-xattr (falsified)`) and
  dead: both modes `ELOOP` on symlinks. I **verified the mechanism** rather
  than trusting the ADR's prose: `lsetxattr` on a symlink fails
  (`EPERM`/`EINVAL`) while regular files take xattrs fine -- symlinks cannot
  carry the metadata those modes need, and alpine's `/bin` is all symlinks.
- **Expensive:** podvm runs its own 9p server (sidecar ownership map, no
  per-dentry xattrs) or patches QEMU's local-9p backend. Both dwarf B and
  C. Not worth it to avoid a setuid helper.

### 4. Empirical de-risking FIRST (the lesson from mapped-xattr)

The prior attempt died on an *untested assumption*. So before writing a line
of feature code, I proved B's kernel mechanism standalone -- and found **four
sequencing traps** that each would have silently broken the implementation:

1. **Both ranges in ONE `newuidmap` call.** `uid_map` is write-once; my
   first attempt split them across two calls and the second got EPERM.
2. **The child must `unshare()` BEFORE the parent calls `newuidmap`** -- I
   inverted it once and got EPERM (the helper hit a process not yet in a new
   userns).
3. **`setgroups=deny` does not interfere with `newgidmap`** (verified, so
   the existing deny-write can stay).
4. **The parent can write the child's single-line map directly**, so the
   parent ALWAYS writes the map -- one uniform handshake, not a fork in the
   logic.

The isolation test proved the round-trip exactly:

```
guest chown(1000) -> host uid 166535   (subuid 165536 + 999)
guest chown(425)  -> host uid 165960   (subuid 165536 + 424)
```

### 5. Implementation

The QEMU fork in `start_qemu` became a handshake:

```
child:  unshare -> signal "unshared" -> block
parent: read /etc/subuid+subgid -> newuidmap (range) or direct (single) -> signal "go"
child:  mount overlay -> exec QEMU
```

- **`src/subuid.{c,h}` (new):** read the allocation; invoke the setuid
  helpers; single-uid fallback.
- **`subuid_map_child` returns -2** when the uid map was committed but the
  gid map failed -- the caller must NOT fall back then (uid_map is
  write-once; a retry hits a misleading EPERM), so it aborts rather than
  launch half-mapped. I caught this edge case in my own first draft by
  re-reading the fallback path before committing.
- **`setup_userns_overlay(ready_fd, go_fd)`:** unshare + handshake; the map
  write moved parent-side.
- Wired into `CMakeLists.txt` **and** the `Makefile` prereq list (Phase-2
  lesson, applied from memory).
- **`tests/e2e_ownership.sh` (new):** a named container chowns a file to
  1000; asserts the host-side upper file is owned by a uid in the subuid
  range (not the invoking user, not root).

### Result

```
E2E-OWNERSHIP PASS: guest chown 1000 -> host uid 166535 (subuid range 165536..231072)

39 tests pass, 0 warnings, 0 fail
```

`RUN chown -R app:app /app` before `USER app` now works -- the acceptance
criterion ADR-028 wanted, achieved via B instead of C.

Commits:
- `d5e3881` feat: subuid range mapping -- cross-uid chown works (ADR-028, path B)
- (ADR reversal record + this log in the following commit)

---

## What We Learned

### Technical

- **podvm boots the HOST kernel, not a podvm-built one.** `get_host_kernel()`
  resolves `/boot/vmlinuz-$(uname -r)`; `make kernel` is a stub. Any
  assumption of the form "we need kernel feature X rebuilt" must first check
  whether the host kernel already has it. It usually does.
- **`uid_map` is write-once.** All ranges must go in a single `newuidmap`
  invocation. A second write (to add a range, or to fall back after a
  partial failure) silently gets EPERM.
- **Writing another process's uid_map needs CAP_SETUID in the parent
  namespace** -- which is exactly why only the setuid newuidmap/newgidmap
  helpers can write a range, and why the map write must be parent-side. An
  unprivileged parent CAN still write the child's single-line map (own uid).
- **`lsetxattr` on a symlink fails; on a regular file it succeeds.** This is
  the structural reason 9p `mapped-xattr`/`mapped` die on symlink-dense
  images -- and it is verifiable in 20 lines, so the ADR's prose claim need
  never be taken on faith.
- **The userns does the uid translation for free.** Once the map is a range,
  a guest `chown(1000)` becomes a host file owned by `subuid+999` with no
  code beyond the map -- the kernel handles it. The whole of B is the map.

### Process

- **De-risk the load-bearing kernel mechanism in isolation BEFORE writing
  feature code.** This is the direct lesson from the falsified mapped-xattr
  attempt, and it paid off: four sequencing traps surfaced in a standalone
  test that would each have been a confusing bug inside the real fork/exec
  path. Proving `chown -> subuid` round-trips first made the implementation
  a known quantity.
- **Correct a bad premise out loud, before it drives a decision.** My
  initial "ADR-028 is kernel-blocked" was wrong (host kernel has overlay).
  Had I not re-checked, the whole fork would have been reasoned from a
  false constraint.
- **Re-read your own fallback paths before committing.** The `-2` edge case
  (uid map committed, gid map failed -> cannot fall back) was invisible in
  the happy path and only appeared on a deliberate second read.

---

## What We Decided

- **ADR-028 is implemented as B (subuid range), NOT C (guest-side overlay).**
  The ADR's title rejected UID Translation; that rejection is reversed. B
  preserves "the guest asks, the host executes"; C would invert it.
- **Host filesystems now hold subuid-owned files** for cross-uid containers
  -- accepted as the cost of keeping ask/executes. This was the exact reason
  the ADR originally preferred C; the project re-weighed it.
- **Hosts without a subuid allocation keep the single-uid behavior.** B is
  additive: no allocation / no newuidmap -> identical to before (no cross-uid
  chown, but nothing breaks).
- **The 9p-server route (own server or QEMU patch) is rejected** -- far more
  expensive than B, only warranted if subuid's host-fs-ownership downside
  were a hard blocker.

---

## Rules Added

- **Verify a kernel feature on the HOST kernel before claiming a rebuild is
  needed.** podvm boots the host kernel; `kernel/config` describes the
  non-default synthetic kernel only.
- **De-risk the load-bearing kernel/syscall mechanism in a standalone test
  before building feature code on top of it.** Especially when a previous
  attempt at the same goal was falsified by an untested assumption.
- **`uid_map` is write-once -- all ranges in one `newuidmap` call**, and
  never retry/fallback after a partial map commit (abort instead).

---

## What We'd Do Differently

- The ADR-028 reversal should ideally have been a *new* ADR superseding the
  old one, rather than editing the old ADR's status in place. Editing kept
  the falsified-exploration record adjacent to the reversal, which is
  useful, but a clean supersede chain is more idiomatic. Worth deciding per
  project convention next time.
- I spent exploratory effort on the 9p-server idea before settling on B. The
  cost table (A/B/C) should have been built *first* -- it made the decision
  obvious in retrospect, and constructing it earlier would have skipped the
  detour.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/subuid.h` (new) | `SubUidAlloc` + the parent-side map-writing contract (0 / -1 fall-back / -2 fatal) |
| `src/subuid.c` (new) | Read /etc/subuid+subgid; invoke setuid newuidmap/newgidmap; single-uid fallback |
| `src/main.c` | `start_qemu` fork -> parent-maps handshake (two pipes); `setup_userns_overlay(ready,go)` loses its map write |
| `CMakeLists.txt`, `Makefile` | Build subuid.c (prereq list included) |
| `tests/e2e_ownership.sh` (new) | Named container chowns; asserts host upper file is subuid-owned |
| `tests/run_tests.sh` | Wire the ownership e2e |
| `adrs/podvm_adr028_uid-mapping.md` | Status -> reversal record (B over C); premise correction |

## Test Results

```
E2E ownership (chown->subuid)  PASS   (new)
...
PASS: 39   WARN: 0   FAIL: 0
```

Binary proof of the round-trip:

```
$ ./tests/e2e_ownership.sh
E2E-OWNERSHIP PASS: guest chown 1000 -> host uid 166535 (subuid range 165536..231072)
```

---

## Next Steps

- Ownership now works for cross-uid containers on the podman-store path.
  The **native layered store** (`store_native.c:unpack_layer_in_userns`)
  has its OWN single-uid userns for whiteout `mknod` -- if build-step
  commits should preserve ownership too, that path needs the same subuid
  treatment (currently out of scope for B).
- ADR-028's *other* consequence -- `USER` and setuid bits from images -- is
  now theoretically unblocked; worth an e2e that runs an image with
  `-u <uid>` and asserts the process uid from inside the guest.
- `save`/`load` and podman interop: subuid-owned upper files may need
  ownership metadata preserved across a podvm->podman save. Untested.
