---
name: next-steps-batch
description: close out the five ranked next steps -- image config blob, sha256 digest verification, zstd layers, the dead -i flag, and native-pull e2e coverage
project: podvm
previous: 2026-10-08_net-and-native-pull.md
---

# Mission Log: Clearing the Ranked Next-Steps Queue

---

## What We Did

Picked up where `2026-10-08_net-and-native-pull.md` left off. That log ended
with a ranked "Next Steps" list; this session worked through the top five in
order, committing each fix separately so the twelve commits bisect cleanly.

Final state: **22 PASS / 1 WARN (musl-gcc) / 0 FAIL**, up from 19/1/0. Two new
e2e tests, one new unit test binary, ~1130 lines added across 15 files.

### 1. Image config blob -- Env / Entrypoint / Cmd / WorkingDir / User

The headline item. Before this, `podvm <image>` with no command failed with
"entrypoint not set", so podvm could only run images whose command the user
typed by hand -- undercutting the OCI promise (ADR-005) that an image carries
its own default command.

Split into four commits so each is independently meaningful:

- **`image.c`/`image.h`**: `ImageConfig` struct + `image_config_parse()`.
  Parses the OCI config blob's `config` object. Unit-tested against a real
  Debian bookworm blob and a full entrypoint/cmd/workingdir/user blob, plus
  rejection cases (a manifest, garbage, empty, NULL).
- **`guest/init.c`**: the guest now honors `podvm.env=`. The host had been
  sending it on both the cmdline and vsock channels since forever, but
  `parse_config` never read it -- `HOME=/root` and any image `Env` were
  silently dropped before exec. Parsed as a comma-separated `K=V` list and
  `setenv`'d just before exec.
- **`store.c`/`store.h`**: `store_image_config()` reads the config blob out of
  podman storage. See "What We Learned" for the on-disk layout.
- **`main.c`**: `apply_image_config()` folds the image config into the run
  config, with docker semantics. Plus `image_config_load()` in `image.c` so a
  native-pull cache hit still carries the default command (`image_pull()`
  now persists the config blob as `.podvm-config.json` next to the rootfs).

Verified against real storage: alpine -> `entrypoint=/bin/sh`,
debian:bookworm -> `bash`, munin-dev -> `./dev_scripts/all` with `cwd=/app`,
all with the image's `PATH` prepended to `HOME=/root`.

**A pre-existing bug this surfaced**: `store_find` only tried the
`localhost/<name>` form for refs with *no colon*. So a locally-built image
referenced as `myimg:v1` (podman stores it as `localhost/myimg:v1`) never
matched, and fell through to the native registry pull, which then tried
docker.io and failed. Found by the new e2e, which builds a tagged throwaway
image.

### 2. sha256 layer-digest verification

`image.c` had carried a `TODO` for this since the registry client was written.
The manifest names every blob by its sha256, so a truncated or tampered
download was extracted as if authentic.

Hand-rolled a compact streaming SHA-256 (`src/sha256.c`) rather than link
libcrypto -- the project's dependency budget is curl + zlib, and integrity
checking shouldn't change that. Streaming, not one-shot, so a layer blob can
be hashed without a second full-size buffer.

`digest_matches()` returns 1 (no judgement) for a missing digest or a
non-sha256 algorithm, so it never blocks on something it cannot verify. A
layer mismatch aborts the pull with both digests printed; a config-blob
mismatch only skips caching the config, since the layers are what matter for
the rootfs.

### 3. zstd layer support

Registries increasingly publish `...tar+zstd` layers; the pull handled only
gzip and raw tar, so it would have treated a zstd layer as raw tar and failed
to untar. Added `zstd_decompress_mem()` mirroring `gunzip_mem`: ask zstd for
the frame's declared size when present, else grow geometrically, retry on
`dstSize_tooSmall`.

Detection is by **magic bytes** (`0x28 0xB5 0x2F 0xFD`), not the manifest
media type, so a mislabeled layer still decompresses. Build links libzstd via
pkg-config with a direct-probe fallback, and *warns* (not fails) if absent,
since gzip remains the common case.

### 4. The `-i` / `--interactive` dead flag

It was parsed and stored but never read. Gave it real meaning rather than
deleting it: QEMU's default `-serial mon:stdio` multiplexes the QEMU monitor
onto stdio, so `Ctrl-A C` drops into the monitor and `Ctrl-A` never reaches
the guest. `-i` now selects `-serial stdio`, giving the guest stdin
exclusively -- right for an interactive shell. Default stays `mon:stdio` so
the debugging escape hatch survives.

### 5. Native-pull e2e coverage

The same blind spot that hid the bearer-token truncation bug for two
sessions: podman storage is checked first, so any image podman already had
never exercised the native path. Added `PODVM_RESOLVE=auto|podman|native` to
select the backend (reshaping the resolution block into two explicit steps),
and `tests/e2e_native.sh`, which clears podvm's cache, forces the native
backend, and boots busybox with no command -- proving manifest fetch ->
bearer auth -> platform manifest -> blob -> sha256 verify -> gunzip -> untar
-> 9p -> chroot -> exec, plus the config blob, since the image's own Cmd is
what runs.

---

## What We Learned

### Technical

- **podman's config blob lives at `overlay-images/<id>/=<base64("sha256:<id>")>`.**
  containers/storage keeps "big data" under the image dir, filename-encoded as
  `=` + standard base64 of the data name. For images podman pulled, the image
  ID *is* the config digest, so the primary path is directly constructible. A
  fallback reads the `manifest` entry and follows `config.digest` for images
  whose ID isn't the config digest. Note this is *not* a `big-data/`
  subdirectory -- files sit directly in the image dir.
- **A base64 loop that advances by 3 and tests `in[i]` over-reads.** With
  `len % 3 != 0` the final stride steps *past* the NUL terminator into
  uninitialized stack memory. Saw it as a spurious `GA==` suffix that made
  every big-data path miss. Bound by `strlen` instead. This cost a debugging
  detour because the corruption looked like a wrong *path*, not a bad encode.
- **`getenv`-style knobs are the clean way to make an unreachable code path
  testable.** The native pull was untestable precisely *because* it was
  correctly deprioritized behind podman storage. Forcing it needed an
  explicit override, not a reorder.
- **`-serial mon:stdio` vs `-serial stdio` is a real usability difference.**
  The monitor mux is great for debugging a wedged guest and terrible for an
  interactive shell, because `Ctrl-A` (readline's beginning-of-line) is
  swallowed as the monitor escape.
- **zstd frames advertise their decompressed size** (`ZSTD_getFrameContentSize`),
  so the output buffer can be allocated once when it's known, and only grown
  speculatively when it isn't.
- **libzstd has no CMake Find module.** `pkg_check_modules` works; a direct
  `find_library(zstd)` is the fallback.

### Process

- **Commit-always + fix-forward again paid off**, and this session had a
  self-inflicted case study: I used `git checkout src/store.c src/main.c` to
  undo temporary debug instrumentation, which also reverted the *uncommitted
  feature work* in those files. Recovered by re-applying from the recorded
  edits, but the lesson is below.
- **Instrumenting by hand-editing debug `fprintf`s into the source, then
  reverting with `git checkout`, is a trap.** Twice this session the debug
  output was itself corrupted (base64 of the debug format strings appearing in
  the output), which sent me chasing a phantom buffer overflow. The actual bug
  was the base64 over-read, found only after removing the instrumentation and
  reading the code.
- **A test harness can have the bug you think you found in the library.**
  Chased a phantom "config blob not persisted" issue that turned out to be
  `printf("... %d ... %d\n", image_config_load(...), c.ncmd)` -- unspecified
  evaluation order meant `c.ncmd` was read *before* the load ran. Assigning to
  a temporary first fixed the harness; the library had been correct all along.

---

## What We Decided

- **Docker command semantics** for the config fold: `podvm img` runs
  Entrypoint + Cmd; `podvm img foo` runs Entrypoint + foo (the user's command
  replaces Cmd, but an image Entrypoint still leads). A user command always
  wins over Cmd.
- **Hand-rolled sha256 over libcrypto**, to keep the dependency budget at
  curl + zlib. ~150 lines, fully covered by known-answer tests.
- **zstd is a warn-not-fail build dependency.** gzip is still the common case;
  missing libzstd shouldn't break the build.
- **`-i` is wired, not deleted.** Deleting a flag docker users type out of
  habit (`-it`) is unfriendly; giving it a real meaning is better than a
  documented no-op.
- **`PODVM_RESOLVE` exists as a testing knob and a user preference.** Default
  behavior is unchanged; the knob makes the native path reachable on purpose.

---

## Rules Added

- **A loop that strides by N and tests `in[i]` will over-read when
  `len % N != 0`.** Bound by `strlen` (or an explicit length), not by the
  sentinel you're stepping past.
- **Don't revert debug instrumentation with `git checkout <file>`** if the file
  also holds uncommitted feature work. Commit first, or restore from a backup
  copy outside the tree.
- **When debugging produces corrupted-looking output, suspect the
  instrumentation before the code.** Base64 of my own debug format strings
  appearing in a path was the tell.
- **Assign a function's result to a temporary before printing it alongside
  other outputs.** `printf("%d %d", f(x), y)` has unspecified order.
- **Detect a compressed stream by magic bytes, not the declared media type.**
  Manifests mislabel; the frame header doesn't.
- **An unreachable-by-default code path needs an explicit override to be
  testable.** Don't reorder production logic to make it reachable.

---

## What We'd Do Differently

- **Verify the base64 encoder against a known vector *before* wiring it into a
  path lookup.** I wrote it, wired it in, then spent a debugging cycle on the
  resulting garbage path. A three-line known-answer check (`"abc"` ->
  `YWJj`) would have caught the over-read immediately.
- **Don't `git checkout` a file to undo debug prints.** The recovery cost more
  than the discipline would have. A backup copy outside the tree (`cp` to
  `/tmp`) is the habit to keep.
- **The `-i` change deserves a guest-side test.** It's currently verified only
  by inspecting the QEMU argv via a shim on `PATH`. A test that asserts the
  guest receives a keystroke the monitor would otherwise swallow would be
  stronger, but needs a pty-driven harness that didn't exist yet.
- **`apply_image_config` could be a pure function with a unit test.** It's
  `static` in `main.c` and currently only exercised through real boots. The
  command-resolution matrix (entrypoint vs cmd vs user command) is exactly the
  kind of logic that wants table-driven tests.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/image.c` | Config-blob parse + cached load; sha256 digest verification; zstd layers; config blob persisted on pull |
| `src/image.h` | `ImageConfig` type + config API |
| `src/main.c` | `apply_image_config()`; `PODVM_RESOLVE` knob; `podvm.cwd`/`podvm.user` on both channels; `-i` wired to `-serial stdio` |
| `src/store.c` | `store_lookup()` (returns image id); `store_image_config()`; base64 encoder; `localhost/<name>:<tag>` matching |
| `src/store.h` | `store_image_config()` decl |
| `src/sha256.c` | New: compact streaming SHA-256 |
| `src/sha256.h` | New: SHA-256 API |
| `guest/init.c` | Honor `podvm.env=`; dedicated env token buffer |
| `tests/e2e_imgcfg.sh` | New: image's own Cmd + Env with no CLI command |
| `tests/e2e_native.sh` | New: native registry pull end to end |
| `tests/test_sha256.c` | New: FIPS 180-4 known-answer tests |
| `tests/test_image.c` | Config-parse + zstd round-trip tests |
| `tests/run_tests.sh` | Wire in sha256, image-config, and native e2e |
| `tests/Makefile` | sha256 + zstd link flags, new test target |
| `CMakeLists.txt` | sha256.c; libzstd discovery |

---

## Test Results

```
Config parsing                PASS
Host args                     PASS
Vsock basics                  PASS
Image pipeline                PASS
SHA-256                       PASS
Host build                    PASS
Guest build                   PASS
Initramfs build               PASS
QEMU available                PASS
KVM available                 PASS
9p modules available          PASS
musl-gcc available            WARN  (pre-existing; not installed)
Vsock enabled                 PASS
9p enabled                    PASS
Balloon enabled               PASS
Uncompressed kernel           PASS
No BPF                        PASS
No modules                    PASS
E2E boot (9p + vsock)         PASS
E2E podman image              PASS
E2E net (DNS+HTTPS)           PASS
E2E image config (default cmd) PASS
E2E native pull               PASS

PASS: 22   WARN: 1   FAIL: 0
```

Manual proofs: `busybox:latest` pulled through the native client
(digest-verified) and booted; a throwaway alpine image with
`CMD=/usr/bin/env` and a marker `Env` ran its own default command with the
env var reaching the entrypoint, no command typed.

---

## Next Steps

Remaining from the previous log, untouched this session, still ranked by
value:

1. **Args-with-spaces on the config channel.** The space-delimited
   `podvm.*=` format cannot carry an argument containing a space. This now
   bites harder: an image whose `Cmd` is `["/bin/sh","-c","echo hi"]` cannot
   run with no CLI command, because the `-c` script travels over the same
   channel. The new e2e works around it with a space-free `CMD`. Fix is to
   the encoding (escape or length-prefix over vsock), keeping `strparse.c` as
   the cmdline-fallback decoder.
2. **Overlay write-back (ADR-020).** The podman-storage upper layer is a
   throwaway tmpfs, so in-guest writes vanish with the VM.
3. **`apply_image_config` unit tests.** Extract from `main.c` and cover the
   entrypoint/cmd/user-command matrix with table-driven tests (see "What We'd
   Differently").
4. **A pty-driven test for `-i`.** Prove a keystroke the monitor would
   otherwise swallow reaches the guest.
5. **`podvm.user` by name.** The guest parses `uid:gid` only; an image whose
   `User` is `nobody` or `app` is silently ignored (runs as root). Needs a
   passwd lookup inside the guest.
6. **ADR-023 cleanup.** The slirp constants (10.0.2.15/2.3) are hardcoded in
   `init.c`; when bridge networking lands, route per-container config through
   the `podvm.*=` channel.
7. **Bigger design items:** ADR-021 FreeBSD platform, ADR-020 guest overlay
   module.
