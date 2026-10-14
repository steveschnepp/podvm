---
name: ownership-store-round-trip
description: ownership was lost at BOTH ends of the native store (ingest ignored headers, commit hardcoded uid 0) -- fixed so tar carries container-ids and subuid translation happens only at the two disk edges; USER and chown now survive the round-trip; surfaced a latent musl portability bug
project: podvm
previous: 2026-10-14_subuid-ownership-path-b.md
---

# Mission Log: Ownership Survives the Store Round-Trip

---

## The Ask

"yes. correctness matters" -- the user approved the two follow-ups I had
flagged after landing path B: (1) the native store's own single-uid userns
needed subuid treatment so build-step commits preserve ownership, and (2)
save/load ownership across podman interop was untested.

Investigating both revealed they were one systemic bug wearing two hats, and
bigger than either follow-up: **ownership was discarded at BOTH ends of the
store**, so `USER` and `chown`'d files were root-owned in the guest even with
path B fully active at run time.

---

## What We Did

### 1. Ground truth first (the mapped-xattr lesson, applied)

Before designing anything, I proved what actually round-trips wrong -- rather
than reasoning from the code:

- **Ingest:** fed a GNU-tar `--owner=1000` layer through the real
  `native_put_layer`. The header said uid 1000; the stored file landed owned
  by **1001** (the invoking user). `tar_extract_layer` never read uid/gid from
  headers, and the unpack ns was single-uid, so every image file became
  root-in-guest.
- **Commit:** packed a subuid-166535-owned file (what a path-B guest `chown`
  produces). The tar header said **0/0** -- `hdr_build` hardcoded uid/gid to 0.

Both are ADR-028's "Ingest/commit" section, never implemented. The blast
radius is contained: `tar_extract_layer` (ingest) and `tar_pack_dir` (commit
at store_native.c:212 + save at :498). The `native_load` staging extract does
not need ownership -- it re-ingests each layer through `native_put_layer`,
which does.

### 2. The design decision, and the one semantic call

The structural fact that made the fix clean: `store_layer_bytes` stores the
**raw tar verbatim** as the blob and separately unpacks to the diff dir. So
the **tar format carries container-ids** (the image spec, and what podman
expects), and subuid translation happens only at the two disk edges.

The user chose the one real semantic fork -- what uid the packed tar carries
for a subuid-owned file:

| option | verdict |
|--------|---------|
| **container-uid (1000)** | chosen -- interop-correct, and ingest's range-ns chown round-trips it exactly |
| host subuid (166535) | leaks an implementation detail into the portable format; podman sees a wrong uid |
| collapse to 0 | keeps today's commit loss; only fixes the guest path |

### 3. De-risk the kernel mechanism first

Proven standalone before writing feature code, in a subuid-range ns:
`chown(1000)` lands on subuid+999; `mknod` whiteouts **still work** as
ns-root (so the range-map doesn't break overlay whiteouts -- the thing most
likely to silently regress); out-of-range uid (70000) gives a clean `EINVAL`,
rejectable. And the two translators round-tripped exactly over a spread of
ids (166535<->1000, invoking<->0, foreign passthrough).

### 4. The implementation

- **Ingest** (`tar_extract_layer`): read uid/gid from the header (offsets
  108/116) and `lchown` after each entry is created -- `lchown` so symlinks
  get their recorded owner (a real `chown` would follow the link). Applied
  only on the layer path (`preserve`); the load-staging path runs outside any
  ns and must not chown to ids we don't own. Best-effort: an unsettable id
  doesn't abort the layer.
- **Unpack ns** (`unpack_layer_in_userns`): now uses the subuid **range** map
  via the same parent-writes-map handshake as the QEMU child, so the header's
  container uid lands on the right host subuid.
- **Commit/save** (`tar_pack_dir`): `hdr_build`/`emit_entry`/`pack_walk`
  thread real `st_uid`/`st_gid` (was hardcoded 0) and translate host->container
  via the new `subuid_host_to_container`. Whiteouts stay container-root -- a
  deletion marker has no owner.
- **`subuid`**: gains `subuid_host_to_container` / `subuid_container_to_host`.

### 5. The latent musl bug the guard surfaced

The full suite went red (19 failures) with a **musl guest build failure** I
hadn't touched. Root cause, verified: the build depended on the
`/usr/include/asm` symlink (a `libc6-dev` artifact) for `asm/types.h` (pulled
in by `linux/vm_sockets.h`). musl-gcc drops `/usr/include` and the Makefile's
`-idirafter /usr/include` doesn't cover the **multiarch** dir
(`/usr/include/x86_64-linux-gnu`) where `asm/` actually lives. Plain gcc finds
it automatically; musl doesn't. Fixed by adding
`-idirafter /usr/include/<multiarch>`, detected via `gcc -print-multiarch` so
it's portable across arches. This is exactly what the musl+glibc guard from
the previous session existed to catch -- it caught a real one.

### Result

```
Ownership translate           PASS   (new test_subuid)
Guest build (musl + glibc)    PASS   (musl fixed)
E2E ownership (chown->subuid) PASS
E2E ownership save (commit)   PASS   (new: build-step chown -> save -> container-uid 1000)
...
PASS: 41   WARN: 0   FAIL: 0
```

Two independent end-to-end proofs:
- a tar header saying uid 1000 -> stored file owned by subuid 166535;
- a build-step `chown 1000` -> `podvm save` -> layer tar carries container-uid
  **1000** (not 0, not a raw subuid).

Commit: `eb5d3b6` fix: ownership survives the store round-trip (ADR-028 ingest/commit)

---

## What We Learned

### Technical

- **The tar format must speak container-ids; the disk speaks subuids.** Once
  you decide the portable format carries logical ids, translation collapses to
  two edge functions and the kernel does the rest (the range-ns `lchown` at
  ingest, the ns map at guest-read). No metadata sidecar needed -- that is the
  elegance path B buys over the xattr approach.
- **`store_layer_bytes` stores the raw tar verbatim** and unpacks separately.
  That decoupling is what let the blob stay interop-correct while the diff dir
  carries subuids.
- **`lchown`, not `chown`, on extract** -- otherwise a symlink's recorded owner
  is lost (chown follows the link).
- **Overlay whiteouts still `mknod` fine in a subuid-range ns.** This was the
  one regression most likely to sneak in (changing the unpack map), and testing
  it up front turned it into a non-event.
- **A musl build that works can still be broken.** It compiled here because the
  `/usr/include/asm` symlink happened to exist; on a host without it, musl
  fails on `asm/types.h` while glibc sails through. The `-Wall` + dual-libc
  guard is what surfaced it.

### Process

- **Prove the round-trip at both ends before designing.** "Ingest loses it"
  and "commit loses it" sound like two bugs; they are one decision (the tar
  format never carried ownership) seen from two sides. Fixing the decision --
  not the two symptoms -- is why the change is ~190 lines and not a rewrite.
- **A red suite that includes a failure you didn't cause is a gift.** The musl
  break was hiding among my changes; the instinct was to assume I broke it. The
  discipline (check `git status` on the guest sources, confirm the mechanism)
  found a real portability bug instead of a wrong revert.
- **De-risk the likely-regression first.** "Does changing the unpack map break
  whiteouts?" was the obvious question; answering it in isolation before the
  refactor meant the answer never became a 2am bisect.

---

## What We Decided

- **The tar format carries container-ids; subuid translation happens only at
  the ingest (unpack) and commit/save (pack) edges.** Blob and `save` output
  are therefore interop-correct for podman.
- **Extract applies ownership only on the layer path** (`preserve`), never on
  load-staging (outside a ns).
- **Whiteouts pack as container-root** -- a deletion marker has no owner.
- **Foreign host uids pass through** the translators unchanged (a real
  host-owned file podvm didn't create is not a container id).
- **The guest musl build adds `-idirafter /usr/include/<multiarch>`**, detected
  dynamically, so it stops depending on the `/usr/include/asm` symlink.

---

## Rules Added

- **Portable tar formats speak logical (container) ids; translate at the disk
  edges.** Never bake an on-disk representation (subuid) into an interchange
  format.
- **`lchown` when extracting ownership onto paths that may be symlinks.**
- **A libc build that succeeds locally may still be broken elsewhere -- check
  the multiarch include dir when adding UAPI headers to a musl build.**

---

## What We'd Do Differently

- The two follow-ups I named ("native store userns" + "save/load interop") were
  actually one bug. I spent investigation effort treating them separately
  before the ground-truth tests revealed they shared a root cause. Running the
  two round-trip probes (ingest + commit) *first*, before splitting the task,
  would have collapsed them immediately.
- `test_tarwrite` still doesn't cover ownership directly (the coverage lives in
  `test_subuid` for translation and the two e2es for the disk edges). A pure
  pack->extract ownership unit test inside `test_tarwrite` would be cheaper
  than the e2es for the ingest direction.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/tar.c` | extract applies header uid/gid via `lchown` (layer path only); pack reads real `st_uid`/`st_gid` and translates to container-ids; whiteouts stay root |
| `src/store_native.c` | `unpack_layer_in_userns` uses the subuid-range map (same handshake as the QEMU child) |
| `src/subuid.h`, `src/subuid.c` | `subuid_host_to_container` / `subuid_container_to_host` |
| `guest/Makefile` | musl build adds `-idirafter /usr/include/<multiarch>` (fixes the missing-`asm`-symlink build) |
| `tests/test_subuid.c` (new) | translation round-trip over a spread of ids |
| `tests/e2e_ownership_save.sh` (new) | build-step chown -> save -> layer tar carries container-uid 1000 |
| `tests/Makefile`, `tests/run_tests.sh` | wire the new tests; add `subuid.c` to the four store/tar test targets |

## Test Results

```
Ownership translate           PASS   (new)
Guest build (musl + glibc)    PASS   (musl fixed)
E2E ownership (chown->subuid) PASS
E2E ownership save (commit)   PASS   (new)
...
PASS: 41   WARN: 0   FAIL: 0
```

Load-side is covered by the same ingest path proved in ground truth
(`native_load` -> `native_put_layer` -> `unpack_layer_in_userns`), which maps a
uid-1000 header onto a subuid.

---

## Next Steps

- Ownership now round-trips ingest->run->commit->save->load on the native store.
  The remaining untested corner is **podman producing an archive that podvm
  loads** with non-trivial ownership (podman's own tar uid conventions); the
  path is right but no e2e drives a podman-authored non-root file through
  `podvm load`.
- `tar_extract_mem` (used only by load-staging) still drops ownership by
  design. If a future feature needs the staging copy to be ownership-faithful,
  it needs its own ns -- currently unnecessary because layers re-ingest.
- ADR-028's full acceptance (mermaid-ascii Dockerfile unmodified) is now much
  closer: `chown` works at run time AND survives the store. Worth re-running
  that build as the real acceptance test.
