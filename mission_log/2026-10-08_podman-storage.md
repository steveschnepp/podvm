---
name: podman-storage
description: consume podman's rootless storage directly (layers in place) -- QEMU child mounts the overlay chain in its own userns; podman doubles as the puller
project: podvm
---

# Mission Log: Podman Storage as the Image Backend

---

## What We Did

Continued the same day's thread (after the 9p pivot). The user's asks, in
sequence: use podman to generate/consume images, no `podman save` export,
"just use .local/share/containers", "hijack podman to pull", and finally
the framing decision: *"the main point of podvm is isolation of runtime,
not the isolation of mounting mechanics"* + *"run directly qemu in userns"*.

### Phase 1: Feasibility (before any C)

- Confirmed podman 5.4.2 rootless on the box with real images
  (`debian:bookworm` single-layer; `munin-dev`/`munin-orig` 5-layer).
- **The blocker:** `podman image mount` requires `podman unshare`, and
  the mount is invisible from the host namespace (`findmnt` showed ext4,
  not overlay). QEMU on the host cannot `-fsdev` a dir that only exists
  inside a userns.
- **The inversion:** instead of dragging the mount out, run QEMU *in*.
  Verified `mount(2)` of a stacked overlay over `overlay/<id>/diff` dirs
  inside `unshare(CLONE_NEWUSER|CLONE_NEWNS)`: 2-layer OK, 3-layer OK,
  5-layer OK (the munin chain, including a pure-metadata layer).
- **Red herring recorded:** the `mount(8)` *binary* fails inside the
  userns ("wrong fs type..."), but the raw syscall succeeds. Any future
  shell-level experiment must not trust `mount(8)` output here. The C
  path uses the syscall.
- **Also recorded:** `EPERM` from overlay mount in a shell means you
  forgot the `unshare` wrapper (spent two rounds bisecting my own
  missing `podman unshare`); `ELOOP` means duplicate lowerdirs in the
  chain (overlayfs rejects a layer listed twice -- podman's parent
  chain never does this, but hand-built chains can).
- Whiteouts: no `.wh.` files, no char devices, no xattrs found in the
  diff dirs; rootless storage relies on the kernel overlay semantics
  over the plain diff dirs, so podvm needs no whiteout handling at all
  -- the kernel does it. One more thing the overlay mount gave us for
  free.

### Phase 2: Implementation (6 commits, `301dab4..1c450ef`)

1. **`json.c/h` extraction** (`301dab4`): the DOM parser moved out of
   `image.c` so `store.c` can share it. Mechanical; no behavior change.
2. **`store.c/h`** (`87c5ef0`): reads `overlay-images/images.json`
   (name -> top layer id) and `overlay-layers/layers.json` (parent
   chain). Ref normalization mirrors podman: bare `debian` ->
   `docker.io/library/debian:latest` and `localhost/debian:latest`;
   `debian:bookworm` -> `docker.io/library/debian:bookworm`.
   `store_podman_pull()` forks `podman pull` when the image is missing
   -- the requested hijack; credentials/mirrors/zstd become podman's
   problem.
3. **`main.c`** (`403223f`): resolution order path -> podman storage ->
   podvm's own registry client (kept as fallback). In storage mode the
   QEMU child runs `setup_userns_overlay()` after fork, before exec:
   `unshare(CLONE_NEWUSER|CLONE_NEWNS)`, deny setgroups, uid_map
   `0 <uid> 1` (order-sensitive), gid_map, make-rprivate `/`, tmpfs for
   upper/work under `/tmp/podvm-ovl.<pid>`, overlay mount of the chain
   at the rootfs path, then exec QEMU inside the ns. ~100 lines.
4. **`csv_parse` bug fix** (`412edb1`): the first podman e2e run
   printed `E2E-PODMAN-OK podvm.env=HOME=/root` -- the args parser
   treated space as a separator and consumed the next cmdline token as
   an arg. My first fix was wrong (removed space as a terminator
   entirely, value ran to end-of-string); the correct fix stops the
   whole parse at a space. Regression test in `test_config.c`. Note the
   guest init links `strparse.c`, so the initramfs must be *rebuilt*
   (not just the host binary) to see the fix -- burned one debug cycle.
5. **test infra** (`d07bef2`, `68eb347`): `test_image` now compiles
   `src/json.c` too; `tests/e2e_podman.sh` added, wired into
   `run_tests.sh` gated on podman + at least one local image.
6. **build wiring** (`1c450ef`): `json.c`/`store.c` into the CMake
   target. Landed last by mistake -- `403223f` does not link without
   it; noted in the commit message as a bisect caveat.

### Phase 3: Verification

- `podvm debian:bookworm /bin/echo E2E-PODMAN-OK` -> clean run,
  single-layer path.
- `tests/e2e_podman.sh` picked `localhost/munin-dev:latest` -> the
  **5-layer** image, so the multi-layer chain is what the committed
  test actually exercises.
- Suite: **17 PASS / 1 WARN (musl-gcc) / 0 FAIL**.
- ADR-024 written with the mount-mechanics findings; README to follow.

---

## What We Learned

### Technical

- **Rootless podman storage is a userns-only view.** Its `merged/`
  paths do not exist in the host mount namespace. Any tool consuming
  it (podvm today, others tomorrow) must either run inside a userns or
  re-mount the diffs itself.
- **`overlay/<id>/diff` + `lower` files:** podman chains overlays of
  overlays, but the kernel accepts the raw stacked lowerdir list just
  fine -- reading `layers.json` parent links is sufficient; the `lower`
  files can be ignored.
- **`mount(8)` vs `mount(2)` inside userns** (see red herring above).
- **uid_map dance is order-locked:** write `setgroups=deny` before
  `gid_map`, or `gid_map` fails with EPERM and you will blame the wrong
  thing.
- **Guest-side parsing bugs need guest rebuilds.** The initramfs embeds
  `init`, which statically links `strparse.c`; rebuilding only the host
  binary cannot fix a guest parsing bug.

### Process

- **Feasibility-first paid off again** (same as the 9p pivot): every
  mount-mechanics unknown was resolved with shell/Python probes before
  a line of C, so the implementation commit series went in almost
  straight-line.
- **The e2e test found a real bug in the first run** -- and my first
  fix was wrong, caught only by rerunning the e2e, not by the unit
  test alone (the unit test was added *with* the fix, so it validated
  my misunderstanding until the real e2e disagreed). Write the
  failing-scenario test *before* the fix when possible.
- **User framing as a design rule** ("isolation of runtime, not of
  mounting mechanics") is worth repeating in the ADR; it settles a
  whole class of future objections about borrowing podman's userns.

---

## What We Decided

- **Podman storage is the primary image backend**; podvm's registry
  client becomes the fallback (ADR-024). podman doubles as the puller.
- **QEMU runs inside the staging userns** (self-contained in the
  binary, ~100 lines) rather than a podman-container wrapper or a
  persistent helper. Wrapper revisit criteria in ADR-024.
- **csv_parse: space terminates, comma continues.** Cmdline tokens are
  space-separated; args are comma-joined by the host.
- The overlay+userns staging is accepted overhead for now; optimize
  only if it shows up in profiles (ADR-022's stance).

---

## Rules Added

- **Never trust `mount(8)` inside a userns; test with the syscall.**
- **uid_map before gid_map is wrong; setgroups=deny first, then
  uid_map, then gid_map.**
- **Guest-linked shared code changes require a guest rebuild + new
  initramfs**, not just a host rebuild.
- **Feasibility probes before implementation** for anything involving
  namespaces, mounts, or device access.

---

## What We'd Do Differently

- **CMake wiring should have been in the first commit that needed
  it** (`403223f`), not a cleanup commit after five others. The
  dependency-first discipline from the previous session slipped here.
- **The csv_parse fix should have been test-first.** I wrote the fix,
  then the test that matched the fix -- and the fix was wrong. The e2e
  caught it; a test written from the *observed failure* would have
  caught it before the wrong fix landed (the wrong fix is in history,
  corrected by `412edb1`'s final form).
- **`storage_root()` should also honor `CONTAINERS_STORAGE_CONF`** for
  configured storage paths; today it assumes the XDG default. Fine for
  the reference box, a known gap for configured setups.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/json.c` / `json.h` | JSON DOM parser, extracted from image.c |
| `src/store.c` / `store.h` | Podman storage backend: find image, chain layers, podman-pull hijack |
| `src/main.c` | Resolution order; `setup_userns_overlay()` in the QEMU child |
| `src/strparse.c` | csv_parse token-boundary fix |
| `tests/test_config.c` | Regression test for the args/env token bug |
| `tests/Makefile` | test_image links src/json.c |
| `tests/e2e_podman.sh` | New: real podman image through the whole VM path |
| `tests/run_tests.sh` | e2e_podman wiring (gated on podman + images) |
| `CMakeLists.txt` | json.c, store.c in the target |
| `adrs/podvm_adr024_podman-storage-backend.md` | The decision + mount-mechanics evidence |

---

## Test Results

```
... previous suite ...
E2E boot (9p + vsock)         PASS
E2E podman image              PASS   (localhost/munin-dev:latest, 5 layers)

PASS: 17   WARN: 1   FAIL: 0
```

Manual: `podvm -r guest/initramfs.cpio debian:bookworm /bin/echo
E2E-PODMAN-OK` -> serial shows `E2E-PODMAN-OK`, clean exit via
panic=-1/-no-reboot.

---

## Next Steps

1. **`CONTAINERS_STORAGE_CONF` support** in `store.c` (known gap).
2. **Image config blob (Env/Entrypoint/Cmd):** with storage access,
   the manifest/config JSON is in the storage big-data; honoring the
   image's default command becomes *easier* via podman (still open from
   the vsock-fix session).
3. **Write access semantics:** the overlay upper is a throwaway tmpfs
   -- writes work in-guest but vanish with the VM. Decide the commit
   story (ADR-020's guest overlay module is the designed destination).
4. **DNS/HTTPS e2e** (deferred twice now): static-curl link in the
   guest needs the full dep set; consider shipping a tiny Go/Rust
   static client or trimming curl features instead.
5. **Network plumbing inside the userns** when ADR-023's slirp lands:
   verify `-netdev user` works in the same child (expected: yes, no
   privileges needed).
6. **ADR-021 (FreeBSD):** the storage backend is Linux/podman-specific;
   the FreeBSD platform keeps the registry client path. Note in the
   FreeBSD work when it resumes.
