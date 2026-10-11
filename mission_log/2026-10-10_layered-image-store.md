---
name: layered-image-store
description: evolve podvm off flatten-on-ingest to a podvm-native layered store (ADR-027) -- Phase 1 lands the store skeleton + run path, the tar module, and a hand-planted image that boots via the overlay chain
project: podvm
previous: 2026-10-10_unique-vsock-cids.md
---

# Mission Log: Podvm-Native Layered Image Store

---

## Phase 1: Store skeleton + run path (2026-10-10)

First slice of ADR-027 / `specs/01_LAYERED_IMAGE_STORE.md`. The user's
pivot: podvm flattens every image into one merged rootfs on ingest (pull
and build), which wastes disk (no dedup), blocks a build cache, and makes
layer identity -- and `podvm save` / build-vs-docker comparison --
impossible. "The flattening has served its course; we need to evolve" into
a podman replacement. Decisions locked via a design handshake: a
**podvm-native layered store** (not containers/storage, not a full OCI live
store), sequenced **ADR + spec first, then incremental slices**.

Phase 1 only makes the *run* path consume the new store. Pull and build
still flatten (Phases 2-3).

### What We Did

1. **`src/tar.{c,h}` (new).** Moved the ustar/GNU reader verbatim out of
   `image.c` (`untar_mem` -> `tar_extract_mem`) so the store can unpack a
   layer without linking the registry client (curl/zlib/zstd), and added a
   writer (`tar_pack_dir`): dirs, regular files, symlinks, overlay
   char-0:0 whiteouts emitted as portable `.wh.<name>`, and GNU longname
   (`L`) for paths that do not fit the ustar fields.
2. **`src/store_native.{c,h}` (new).** The store itself: content-addressed
   commit (`native_commit_diff` packs a diff dir to a blob keyed by its
   uncompressed sha256 and unpacks it to `layers/<hex>/diff`, dedup by
   digest), blob put, manifest write/read, and `native_chain` -- which
   returns the chain **top-first** (overlayfs lowerdir order), reversing
   the manifest's bottom-first list.
3. **`src/main.c`.** Run-path resolution becomes path -> **native store**
   -> podman -> registry client; the native branch fills the same
   `store_layers[]` the existing `setup_userns_overlay()` consumes, so the
   overlay mount / 9p / guest are untouched. Config load via
   `native_config`.
4. **Tests.** `test_tarwrite` (pack/extract round-trip incl. a 150-char
   name forcing the `L` path), `test_store_native` (commit, dedup, chain
   ORDER, config round-trip). `test_image`/`test_store` now link `tar.c`;
   `test_image` calls `tar_extract_mem`.
5. **Proof.** A scratch `plant` helper committed the busybox rootfs as a
   single-layer image; `podvm run` printed `using native store:
   local/busybox:test (1 layers)`, booted via the overlay chain, and
   reported exit code 7 over vsock.

### What We Learned

#### Technical

- **Moving a static function out of a translation unit breaks any test
  that reached into it.** `test_image.c` `#include`s `image.c` to reach
  statics and called `untar_mem` directly; the rename/move broke its
  build, and because `run_tests.sh` does `make clean && make all`, one
  link failure aborted the whole build and made *every* later test "FAIL"
  (missing binary) -- a scary 12-failure cascade that was really one
  undefined symbol.
- **The overlay lowerdir order hazard is real and exactly as specced:**
  manifest = bottom-first, overlay `lowerdir` = top-first. `native_chain`
  reverses; the unit test pins `L[0] == top layer` so a silent reversal
  cannot ship.
- **`-p` hostfwd plumbing was never broken.** The `e2e_runflags` `-p`
  failure was the e2e's image auto-selection (`grep -iE "alpine|debian" |
  head -1`) grabbing `debian:bookworm-slim` (listed first, newer after my
  munin detour added podman images) -- debian has no busybox `nc`, so the
  guest server never listened. curl rc=56 (reset, forward reached QEMU but
  no listener) vs rc=7 (refused, no VM) was the tell.
- **A manual repro that omits the initramfs looks like a podvm bug:**
  "initramfs not found" -> no QEMU -> no hostfwd -> "did not publish".
  Two of my diagnostic runs were invalid for exactly this reason before I
  noticed the missing `.cpio`.

#### Process

- **Moving code between files: grep every consumer first.** The
  `untar_mem` consumers were `image.c` (the call) and `test_image.c` (a
  direct call). I found the first, missed the second until the suite
  cascade. A repo-wide grep for the symbol before the move would have
  caught it.
- **`make clean && make all` in the harness turns one broken test into a
  wall of red.** Useful signal: the FIRST failing target names the real
  cause; the rest are just missing binaries.

### What We Decided

- podvm-native layered store (full control; interop via save/load later),
  not containers/storage and not a live OCI store -- per ADR-027.
- Store root `$XDG_DATA_HOME/podvm/store` (persistent) so dedup/build
  cache survive reboots; `PODVM_STORE_DIR` overrides for tests.
- Tar reader shared in `tar.c` (not exposed through `image.h`) so the
  store stays free of curl.
- `e2e_runflags` requires alpine (drop the debian fallback) because the
  `-p` test needs busybox `nc`.

### Rules Added

- **Moving a function across translation units: grep the whole repo for
  its name (including tests that `#include` the source) before the move.**
- **overlayfs lowerdir is top-first; a manifest/chain that is
  bottom-first must be reversed at the boundary -- and pinned by a test.**
- **An e2e that picks its image by `head -1` over a pattern must pick an
  image that actually has the tool the test needs** (else a newer,
  unrelated image silently breaks it).

### What We'd Do Differently

- **Grep every consumer before moving a function.** The `untar_mem` move
  broke `test_image.c` (which `#include`s `image.c` and called it
  directly) and, because the harness does `make clean && make all`, turned
  one undefined symbol into a 12-test red wall. A repo-wide grep for the
  symbol first would have caught the test caller alongside the one in
  `image.c`.
- **Land a pure move as its own commit** (reader out of `image.c` into
  `tar.c`) before adding the writer + store on top, so a break in the move
  is isolated from the new feature.
- **Verify the fixture exists before drawing conclusions from a manual
  repro.** Two diagnostic runs were invalid (missing initramfs) and read
  as a `-p` regression until I noticed "initramfs not found" in the log.

## Files Changed

| File | Purpose |
|------|---------|
| `src/tar.c` / `.h` | New: shared ustar/GNU reader (moved from image.c) + writer |
| `src/store_native.c` / `.h` | New: content-addressed layered store |
| `src/image.c` | Drop the moved reader; call `tar_extract_mem` |
| `src/main.c` | Native-store resolution branch + `native_config` |
| `tests/test_tarwrite.c`, `tests/test_store_native.c` | New unit tests |
| `tests/test_image.c`, `tests/Makefile`, `tests/run_tests.sh` | Wire tests; link `tar.c` |
| `tests/e2e_runflags.sh` | Require alpine for the `-p`/nc test |
| `CMakeLists.txt`, `Makefile` | Build `tar.c`, `store_native.c` |

### Test Results

```
Tar write                     PASS   (new)
Native store                  PASS   (new)
...
PASS: 35   WARN: 0   FAIL: 0
```

### Next Steps

1. Phase 2: pull keeps layers (`image.c` stores per-layer blobs + a
   manifest instead of flattening).
2. Phase 3: build commits each RUN as a layer (drop the final merge).
3. Phase 4: `podvm save`/`load` (OCI/docker archive interop with podman).
4. Phase 5 (optional): GC / `podvm system prune`.

---

## Phase 2: Pull keeps layers (2026-10-10)

The native-client pull now stores layers instead of flattening. A pulled
image lands as content-addressed blobs + unpacked diffs + a manifest, and
the run path, `podvm build`'s FROM, and `podvm images`/`rmi` all follow it
into the layered store. Commit `aad323d`.

### What We Did

1. **`image_pull` -> layered store.** Each layer blob is downloaded,
   decompressed, its diff_id (uncompressed sha256) computed, and stored
   via `native_put_layer` (blob + unpack, dedup by digest); the config
   blob is stored and a manifest written (bottom-first digests). The old
   flattened cache dir is gone on this path.
2. **`image_resolve` sentinel.** Returns `IMAGE_RESOLVED_NATIVE` when the
   image is a chain (rootfs_path not set); the legacy flattened-dir check
   stays for pre-ADR-027 entries. The run path and build's FROM stack the
   chain on that sentinel (`fill_native_chain`).
3. **Ref normalization.** `alpine`, `docker.io/library/alpine`, and
   `alpine:latest` now map to ONE manifest (canonical name drops the hub
   registry + `library/`), so run/images/rmi agree on the storage key.
   Without it, a full-name pull + short-name `rmi` missed each other.
4. **`podvm images`/`rmi` follow.** `images` lists native images with a
   LAYERS column; `rmi` removes from both the native store and the legacy
   cache (an image can live in either), reusing the shared image lock for
   the in-use guard.

### What We Learned

- **A ref->path store key must normalize, or the write and read disagree.**
  `image_pull("docker.io/library/alpine")` wrote the manifest under the
  full name, but `rmi alpine` looked it up as `alpine` -- missed. The
  canonical-name normalization (same rule the old cache listing used)
  fixes the storage key, not just the display name.
- **Moving an image's home breaks every surface that points at the old
  one.** `podvm images`/`rmi` still targeted the legacy cache; `e2e_images`
  caught it immediately. "Pull keeps layers" was really "pull + images +
  rmi move to the layered store."
- **A missing Makefile prereq silently skips recompiles.** `store_native.c`
  wasn't in the root Makefile's `$(BUILD_DIR)/podvm` prereq list, so
  `make` considered build/podvm up-to-date and never recompiled my
  normalize change -- the binary stayed stale through two e2e runs. The
  cmake build tracks it internally, but the outer make target gated on an
  incomplete list. Symptom: source changed, behavior didn't.
- **The default native store accumulates stale state across runs.**
  Pre-normalize manifests in `~/.local/share/podvm/store` (from earlier
  suite runs) listed as `docker.io/library/alpine` and defeated `rmi
  alpine`. Isolated `PODVM_STORE_DIR` per e2e (as `e2e_native` already
  did) makes them hermetic; `e2e_images` now isolates too.

### Files Changed

| File | Purpose |
|------|---------|
| `src/image.c` / `.h` | pull -> layered store; `IMAGE_RESOLVED_NATIVE` sentinel |
| `src/store_native.c` / `.h` | `native_put_layer`, `native_list`, `native_rmi`, ref normalization |
| `src/main.c` | `fill_native_chain`; sentinel in run + build FROM; native images/rmi |
| `Makefile` | add `store_native.c` to the podvm prereq list |
| `tests/e2e_native.sh`, `tests/e2e_images.sh` | isolate `PODVM_STORE_DIR`; native-store assertions |
| `tests/test_store_native.c` | `native_put_layer` + `native_list` cases |
| `tests/Makefile` | link `store_native.c` into `test_image`/`test_store` |

### Test Results

```
Native store                  PASS   (+ put_layer, list)
E2E native pull               PASS   (lands as a manifest, boots via chain)
E2E images/rmi (cache)        PASS   (lists layers, rmi removes, in-use guard)
...
PASS: 35   WARN: 0   FAIL: 0
```

### Next Steps

1. Phase 3: build commits each RUN as a layer (drop the final merge).
2. Phase 4: `podvm save`/`load` (OCI/docker archive interop with podman).
3. Phase 5 (optional): GC / `podvm system prune` (native `rmi` currently
   leaves orphan blobs).
4. Retire the legacy flattened cache once build output moves (Phase 3).

---

## Phase 3: Build commits each RUN as a layer (2026-10-11)

`podvm build` now writes a layered image (manifest + config blob) to the
native store instead of cp-merging into a flattened cache dir. The headline
was not the build rework but the **whiteout blocker** it exposed. Commit
`2176722`.

### The whiteout blocker (found while scoping)

`e2e_build` tests `RUN rm /etc/hostname` (a cross-layer deletion). Today
that works via overlay+cp: the step upper already has a char-0:0 whiteout
(created by the VM's userns, where the user is root). But a layered store
stacks diff dirs as overlayfs lowerdirs, so the deletion must be a
char-0:0 device *in that layer's diff dir* to hide the lower file. And a
rootless process **cannot `mknod` char 0:0** (CAP_MKNOD). Worse, the
existing pack->unpack turned the char-0:0 into a `.wh.<name>` tar entry
that `tar_extract_mem` then *deleted* (a no-op) -- so the deletion was
silently lost. A latent Phase 2 pull bug too (unnoticed: alpine/busybox
are single-layer).

Verified empirically (`unshare -Urm` + `mknod c 0 0` + a stacked overlay
honors it) that a rootless userns CAN create an overlay whiteout. So
layer unpack now forks a short-lived user+mount ns and `tar_extract_layer`
materializes `.wh.<name>` as char 0:0 -- rootless-podman's own trick. The
legacy flatten path keeps applying whiteouts (it extracts into one dir).

### What We Did

1. **Whiteout fix.** `tar_extract_layer` (preserve `.wh.<name>` as char
   0:0) beside `tar_extract_mem` (apply); `store_layer_bytes` unpacks via
   `unpack_layer_in_userns`, so both pull and build-commit keep whiteouts.
2. **Build rework.** FROM base -> digests (reuse a native base's digests;
   copy a podman/legacy base to native, per "simple now, optimize later");
   each COPY/RUN commits its upper to the native store and the native diff
   dir feeds the next step's overlay; final manifest = base (bottom-first)
   + steps (build order) + config blob (`open_memstream`). No cp merge.

### What We Learned

- **The layered model turns whiteouts from a non-issue into the crux.**
  overlay+cp hid the problem (the kernel applied whiteouts at merge time);
  stacking lowerdirs pushes it into the store's representation, where
  rootless `mknod` is the wall.
- **A rootless userns can create overlay whiteouts** (`mknod c 0 0` as
  userns root, honored by a stacked overlay) -- empirically verified before
  committing to the design. This is exactly what rootless podman does at
  unpack.
- **A latent bug hides behind single-layer fixtures.** The Phase 2 pull
  path lost whiteouts, but alpine/busybox (1 layer) never exercised a
  cross-layer deletion, so the suite stayed green.
- **`cp`-merge was masking a representation problem.** Removing it (the
  whole point of layers) surfaced what it had papered over.

### What We Decided

- Whiteouts = char 0:0 in diff dirs, materialized at unpack inside a
  userns (not `.wh` files converted at mount, which would mutate shared
  diffs / race concurrent runs). Opaque dirs (`.wh..wh..opq`) not yet
  materialized -- a noted limitation.
- Non-native FROM bases are copied into the native store (self-contained
  image); a native base reuses its digests. Optimize dedup later.

### Files Changed

| File | Purpose |
|------|---------|
| `src/tar.c` / `.h` | `tar_extract_layer` (preserve whiteouts as char 0:0) |
| `src/store_native.c` / `.h` | userns unpack; `native_diff_path` |
| `src/main.c` | `cmd_build` -> layered manifest + config blob |
| `tests/e2e_build.sh` | isolate `PODVM_STORE_DIR` |

### Test Results

```
E2E build (Dockerfile)        PASS   (2 RUNs, whiteout -> GONE, COPY, config, rmi)
...
PASS: 35   WARN: 0   FAIL: 0
```

### Next Steps

1. Phase 4: `podvm save`/`load` (OCI/docker archive interop with podman) --
   `tar_pack_dir` already emits the portable `.wh` form for save.
2. Phase 5 (optional): GC / `podvm system prune` (native `rmi` + failed
   builds leave orphan blobs).
3. Materialize opaque whiteouts (`.wh..wh..opq`) if a real image needs
   them.
4. Retire the legacy flattened cache (build + pull both native now).

---

## Phases 4-5: save/load interop + system prune (2026-10-11)

Closed the archive boundary with podman (Phase 4) and garbage-collected
the store (Phase 5). Commit `7c2317b`. The whole ADR-027 layered store
(Phases 1-5) is now feature-complete: pull, build, run, save, load, GC.

### What We Did

1. **`podvm save [-o F] IMAGE`** -- writes a docker-archive tar
   (manifest.json + config + layer blobs as `<hex>/layer.tar`, hardlinked
   so no data copy). Mirrors `podman save`'s CLI; stdout when `-o` is
   omitted. **`podvm load [-i F]`** ingests one (extract, per-layer
   `native_put_layer`, config blob, manifest); stdin when `-i` omitted.
2. **The rejection fix.** `podman load` first refused the archive --
   "payload does not match any of the supported image formats." Ground
   truth (a real `podman save`) showed the config must carry
   `rootfs.diff_ids`; ours lacked it. `cmd_build` now emits
   `rootfs.diff_ids` + `history`. For a *pulled* image the registry
   config already has diff_ids -- and they equal podvm's native digests,
   because both hash the *uncompressed* tar.
3. **`podvm system prune`** -- `native_gc` drops blobs/layers no manifest
   references (native `rmi` and failed builds leave orphans).

### What We Learned

- **`podman load` validates against `rootfs.diff_ids`.** An archive whose
  config omits rootfs is rejected outright, not repaired. Adding diff_ids
  (the layer digests, bottom-first) is what makes a hand-rolled
  docker-archive loadable.
- **podvm's native digests ARE the canonical diff_ids** for pulled images
  -- both are sha256 of the uncompressed layer tar. So a registry config
  dropped into a podvm archive validates with no re-hashing.
- **Bidirectional interop is the real proof.** `podvm save` -> `podman
  load` -> `podman run` (sees the file a RUN step created) and `podman
  save` -> `podvm load` -> `podvm run` both green. The "compare a podvm
  build to a docker build" goal that started this whole effort is now
  closable: export either side and diff.

### Files Changed

| File | Purpose |
|------|---------|
| `src/store_native.c` / `.h` | `native_save`, `native_load`, `native_gc` |
| `src/main.c` | `save`/`load`/`system prune` subcommands; `cmd_build` config gains `rootfs.diff_ids` + `history` |
| `tests/test_store_native.c` | GC case |
| `tests/e2e_saveload.sh` | podvm<->podman save/load interop + safe prune |
| `tests/run_tests.sh` | wire the save/load e2e |

### Test Results

```
Native store                  PASS   (+ gc)
E2E save/load (podman interop) PASS   (new)
...
PASS: 36   WARN: 0   FAIL: 0
```

### ADR-027 Status: all five phases landed

| Phase | Delivered |
|-------|-----------|
| 1 | Store skeleton + run path (tar module, store_native, chain via overlay) |
| 2 | Pull keeps layers (blobs + manifest; ref normalization; images/rmi) |
| 3 | Build commits each RUN/COPY as a layer + the whiteout fix (userns unpack) |
| 4 | `podvm save`/`load` (docker-archive interop with podman) |
| 5 | `podvm system prune` (GC orphan blobs/layers) |

### Remaining / follow-ups

- Opaque whiteouts (`.wh..wh..opq`) not materialized (noted limitation).
- `--format oci-archive` for save/load (only docker-archive today).
- Retire the legacy flattened cache (`/tmp/podvm/images`) now that pull
  and build both write to the native store.
- A build cache reusing committed step layers across builds (digest
  lookup before running a RUN) -- the dedup foundation is in place.
