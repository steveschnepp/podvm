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
