# Podvm-Native Layered Image Store Specification

## Overview

Replace podvm's flatten-on-ingest image storage with a persistent,
content-addressable, **layered** store. Layers are kept as sha256-addressed
blobs plus unpacked per-layer directories; an image is a manifest listing an
ordered set of layer digests and an image config. `podvm run` stacks the
chain through the overlay path podvm already uses for podman images. This
unlocks dedup, a build cache, real layer identity, and `podvm save`/`load`
interop with podman/docker.

Implements ADR-027.

## Motivation

podvm currently flattens every image into one merged rootfs: `src/image.c`
untars all pulled layers into a single directory, and `podvm build` merges
each step's upper into one directory. The consequences are the exact gaps
flagged for a podman replacement:

- **No dedup**: two images sharing a base each store a private full copy.
- **No build cache**: intermediate layers are merged away, so every RUN
  re-executes on rebuild.
- **No layer identity**: a podvm build is 1 layer vs docker's N, so builds
  cannot be compared and `podvm save` has no per-layer material to emit.

The *run* path is already layered (ADR-024, `src/store.c` +
`setup_userns_overlay()` mount an ordered chain of `overlay/<id>/diff`
dirs). Only podvm's *own* store loses the structure. This spec makes
podvm's own store layered too, reusing that run path.

## Goals

- Store pulled and built images as an ordered chain of content-addressable
  layers (no flatten).
- Identical layer content is stored once (dedup by sha256 digest).
- `podvm run` executes a native-store image by stacking its chain in the
  existing userns overlay.
- `podvm build` commits each RUN as a layer; the manifest, not a merge,
  represents the image.
- `podvm save <image> -o f.tar` emits a loadable OCI/docker archive;
  `podvm load f.tar` ingests one (podman/docker interop).
- The store persists across reboots (`$XDG_DATA_HOME/podvm/store`).
- Every phase leaves `make test` green.

## Non-Goals

- Byte-level containers/storage (podman) on-disk compatibility for the
  *live* store (interop is via `save`/`load` archives only). ADR-027.
- Native OCI registry `push` (only pull + local store in this spec).
- Multi-stage builds and ARG/ADD (still v1 build scope; layers make them
  possible later).
- Garbage collection / `podvm system prune` (designed for, not delivered).
- Compressing store blobs (store uncompressed; compression is a later
  knob).

## Design

### Store layout

Store root `S = $XDG_DATA_HOME/podvm/store` (default
`~/.local/share/podvm/store`), persistent.

```
S/oci-layout                    {"imageLayoutVersion":"1.0.0"}
S/blobs/sha256/<hex>            layer tars (uncompressed) + config JSON, keyed by sha256 of content
S/layers/<hex>/diff/            unpacked layer content (overlay lowerdir); <hex> = layer digest
S/manifests/<esc>               image manifest JSON; <esc> = "repo+tag" (the '+' flattening image.c already uses)
S/locks/<esc>                   flock reservation files (never deleted; inode = identity), reusing image_lock's pattern
```

- **Layer digest** = sha256 of the *uncompressed* layer tar bytes (the OCI
  diff_id). Blob at `blobs/sha256/<hex>` is that tar; `layers/<hex>/diff`
  is it unpacked. Digest hex is 64 chars; JSON uses `sha256:<hex>` (71).
- Storing both the blob (for save/dedup) and the unpacked dir (for the
  overlay lowerdir) mirrors what podman does (content store + overlay
  diffs).

### Manifest

`S/manifests/<esc>` JSON:

```json
{
  "schemaVersion": 1,
  "name": "docker.io/library/alpine",
  "tag": "latest",
  "config": "sha256:<config-digest-hex>",
  "layers": ["sha256:<layer0-hex>", "sha256:<layer1-hex>"],
  "history": ["FROM alpine", "RUN mkdir /app"]
}
```

- `layers` is **bottom-first** (base first, most recent last) -- the
  natural build order.
- `history` is optional, one entry per layer (build records the Dockerfile
  instruction; pull records the registry's history or omits it).
- `config` is a digest into `blobs/sha256/`, holding the OCI image config
  (reuse `ImageConfig` in `src/image.h`; add `rootfs.diff_ids` = the layer
  diff_ids, bottom-first).

### Config blob

OCI image config JSON, produced from `ImageConfig` plus `architecture`,
`os`, and `rootfs: {"type":"layers","diff_ids":[...]}`. This is the same
shape `podvm save` needs and the shape `.podvm-config.json` already approximates.

### Store API (`src/store_native.h`)

```c
#define MAX_DIGEST 72                 /* "sha256:" + 64 hex + NUL */

typedef struct {
    char digest[MAX_DIGEST];          /* "sha256:<hex>" */
    char diff[STORE_PATH_MAX];        /* S/layers/<hex>/diff (overlay lowerdir) */
} NativeLayer;

/* Is ref present in the native store? 0 = yes. */
int  native_has(const char *image_str);

/* Resolve ref to its layer chain. Emits TOP-first (overlayfs lowerdir
 * order) by reversing the manifest's bottom-first list. Returns the layer
 * count, or -1 if absent/error. */
int  native_chain(const char *image_str, NativeLayer *layers, int max);

/* Load the image config for ref. Returns 0 on success. */
int  native_config(const char *image_str, ImageConfig *c);

/* Commit a diff directory as a layer: pack to S/blobs/sha256/<hex>,
 * unpack to S/layers/<hex>/diff. Writes the digest (sha256:<hex>) to
 * out_digest. Returns 0 on success. Used by build and the native pull. */
int  native_commit_diff(const char *diff_dir, char *out_digest, size_t n);

/* Create/replace a manifest: config digest + ordered layer digests
 * (bottom-first) + history lines. Returns 0 on success. */
int  native_manifest_put(const char *image_str, const char *config_digest,
                         const char *const *layer_digests, int nlayers,
                         const char *const *history, int nhist);

/* podvm save: write ref to out_path as an OCI (or docker) archive.
 * format: 0 = docker-archive (default), 1 = oci-archive. 0 on success. */
int  native_save(const char *image_str, const char *out_path, int format);

/* podvm load: ingest an OCI/docker archive into the store. 0 on success. */
int  native_load(const char *tar_path);
```

- `native_chain` reverses the manifest order: overlayfs `lowerdir` is
  **top-first** (most recent first), the manifest is bottom-first. This
  reversal is the one ordering hazard; it gets a unit test.
- The returned `NativeLayer.diff[]` paths feed `setup_userns_overlay()`
  exactly as `store.c`'s `StoreLayer.diff[]` does today.

### Run-path integration (`src/main.c`)

Today `image_resolve()` returns a single flattened `rootfs_path` and the
run serves it directly over 9p; the layered overlay path runs only when
`store_nlayers > 0` (podman images). Change resolution order to:

1. directory path (unchanged)
2. **native store** (`native_has` -> `native_chain`) -- set
   `store_nlayers` + `store_layers[]` (the same arrays `store.c` fills),
   reusing `setup_userns_overlay()` with no change to the mount code.
3. podman storage (ADR-024, unchanged)
4. native OCI client fallback -- after Phase 2 this writes to the native
   store, so it converges with (2).

The overlay mount, 9p fsdev, and guest are untouched: they already consume
a `lowerdir` chain.

### Pull (`src/image.c`)

On the native-client path, stop flattening. For each registry layer:
decompress, compute the uncompressed sha256 (diff_id), store the blob at
`blobs/sha256/<hex>`, unpack to `layers/<hex>/diff`. Write the config blob
and a manifest (`native_manifest_put`) with the registry's layer order
(bottom-first) and the image config. `image_resolve` then finds it via
`native_has`.

### Build (`src/build.c`)

Each RUN: run the step VM with chain = [already-committed step layers
bottom-first] as lowers + a fresh upper; after the step, `native_commit_diff`
the upper as the next layer and append to the manifest. The final image is
the manifest (base chain + step layers) -- **no host-side merge/cp**, since
the chain *is* the assembly. A failing RUN aborts and drops the partial
manifest. COPY/metadata remain host-side steps that also commit a layer (a
COPY-only layer) or mutate the config.

### Save / Load (`src/store_native.c`)

- **save**: emit a docker-archive (default) or oci-archive tar: the config
  blob, one layer tar per manifest layer (from `blobs/sha256/`), a
  `manifest.json` (docker) or `oci-layout`+`index.json`+`blobs` (oci)
  referencing them in order. Loadable by `podman load`/`docker load`.
- **load**: read an OCI/docker archive, store each layer as a blob+unpack,
  write the config + manifest. Needs a **tar writer** (for save) and
  already has a tar reader (`untar_mem` in `image.c`); the writer is new
  (`src/tarwrite.c`).

## Build Changes

| Target | Change |
|--------|--------|
| `podvm` executable (CMake) | Add `src/store_native.c`, `src/tarwrite.c` |
| root `Makefile` | Add new sources to the `$(BUILD_DIR)/podvm` prereq list |
| `tests/Makefile` | Add `test_store_native`, `test_tarwrite` targets |
| `tests/run_tests.sh` | Wire new unit rows + native-store e2e rows |

## File Manifest

| File | Action | Purpose |
|------|--------|---------|
| `specs/01_LAYERED_IMAGE_STORE.md` | Create | This spec |
| `adrs/podvm_adr027_podvm-native-layered-store.md` | Create | The decision |
| `src/store_native.c` / `.h` | Create | Native layered store: blobs, unpack, manifest, chain, save/load |
| `src/tarwrite.c` / `.h` | Create | ustar writer (pack a dir tree to a tar stream) for layer commit + save |
| `src/image.c` | Modify | Native pull stores layers + manifest instead of flattening |
| `src/build.c` | Modify | Commit each RUN as a layer; drop final merge |
| `src/main.c` | Modify | Resolution order (native store before podman); fill `store_layers` from `native_chain`; `save`/`load` subcommands |
| `CMakeLists.txt`, `Makefile` | Modify | Build the new sources |
| `tests/test_store_native.c` | Create | Manifest R/W, digest, chain ordering (top-first), dedup |
| `tests/test_tarwrite.c` | Create | ustar: dir/file/symlink/whiteout, long names, round-trip vs `untar_mem` |
| `tests/e2e_native_store.sh` | Create | Pull+run a multi-layer image; build keeps steps; save->`podman load` round-trip |
| `tests/Makefile`, `tests/run_tests.sh` | Modify | Wire new tests |

## Migration

Each phase leaves the build green and `make test` passing; the old
flattened cache (`/tmp/podvm/images`) is left to expire (tmpfs) and is not
migrated.

### Phase 1: Store skeleton + run path (est. 4-6h)
1. `src/tarwrite.c` + `test_tarwrite` (pack/unpack round-trip vs `untar_mem`).
2. `src/store_native.c`: `native_commit_diff`, `native_manifest_put`,
   `native_chain`, `native_config`, `native_has` + `test_store_native`.
3. `main.c`: resolution order inserts native store; reuse `setup_userns_overlay`.
4. A hand-planted manifest (test fixture) runs via the chain path.
5. `make test` -- no behavior change for existing images.

### Phase 2: Pull keeps layers (est. 4-6h)
1. `image.c` native-client pull stores blobs + unpack + manifest.
2. `image_resolve` finds it via `native_has`.
3. e2e: pull alpine, `podvm images` shows layer count > 1, `podvm run` works.
4. `make test`.

### Phase 3: Build commits layers (est. 4-6h)
1. `build.c`: per-RUN `native_commit_diff` + manifest append; drop merge.
2. e2e: build a 2-RUN Dockerfile, assert layer count = base + 2; failing
   RUN aborts; whiteouts preserved (deleted file stays deleted).
3. `make test`.

### Phase 4: save / load interop (est. 4-6h)
1. `native_save` (docker-archive) + `native_load`.
2. `podvm save` / `podvm load` subcommands.
3. e2e: `podvm save` a built image -> `podman load` it -> `podman run` it.
4. `make test`.

### Phase 5: GC (optional, est. 3-4h)
1. `podvm system prune`: drop blobs/layers not referenced by any manifest.
2. `make test`.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Overlay order reversed (top-first vs bottom-first) | Med | High (wrong rootfs) | Unit-test `native_chain` against a known 3-layer chain; e2e boots a multi-layer image |
| Whiteouts lost across commit (deleted files reappear) | Med | High | `tarwrite` preserves char 0:0 devices; build e2e asserts a deleted file stays deleted |
| New `tarwrite` bugs (long names, symlinks, modes) | Med | Med | Table-tested; round-trip against the existing `untar_mem` reader |
| Persistent store fills the disk (no GC in v1) | Med | Med | Phase 5 GC; document; `podvm images` shows layer sizes |
| Dual backend (store.c + store_native) drift | Low | Med | Keep both behind one resolve interface; share `StoreLayer` shape |
| Old flattened cache incompatible | Low | Low | New store root; old `/tmp` cache expires |

## Open Questions

- Should the native layered store become the *primary* pull path (replacing
  the ADR-024 podman-hijack), or stay the podman-absent fallback + build
  output in v1? (v1 keeps ADR-024 order; convergence is a follow-up.)
- Store blobs compressed (gzip) to save disk at CPU cost on unpack?
  (v1: uncompressed; revisit with GC.)
- One manifest per `repo:tag` (overwrite on re-tag) vs tag history?
  (v1: single manifest per ref.)
- Should `podvm save` default to docker-archive or oci-archive?
  (podman's default is docker-archive; match it.)

## References

- ADR-027 (this store), ADR-005 (OCI compat), ADR-020/026 (layer commits /
  write-back), ADR-022 (9p), ADR-024 (podman storage read path +
  `setup_userns_overlay`), ADR-026 (host-side write-back).
- `src/store.h` (`StoreLayer`, `store_chain`), `src/image.h`
  (`ImageConfig`, `ImageRef`), `src/main.c` (`setup_userns_overlay`,
  `store_layers`), `src/image.c` (`untar_mem`).
- Mission logs: `2026-10-08_podman-storage.md`,
  `2026-10-09_rm-and-run-flags.md` (build merge-commit).
