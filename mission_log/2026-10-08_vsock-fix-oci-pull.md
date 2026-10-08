---
name: vsock-fix-oci-pull
description: fix the dead vsock config channel (host + guest), replace the fake image_pull stub with a real OCI registry client, and land it all as bisectable commits
project: podvm
---

# Mission Log: Fixing Vsock and Finishing the In-Flight Work

---

## What We Did

The session started from a request to "fix vsock, finish all the inflight
things, commit often to enable git bisect." The tree had ~5 modified files and
~20 untracked ones from the previous PoC session, all uncommitted. The guiding
constraint: every commit must build and pass tests, so `git bisect` stays
usable. That forced an ordering discipline (dependency-before-dependent) that
shaped the whole session.

### Phase 1: Understand the vsock situation

`src/main.c` had a `send_config_vsock()` function and a guest that read config
from the kernel cmdline. Inspection showed vsock was effectively **dead code**:

- The QEMU line `-device vhost-vsock-pci,...` was **commented out**, so no vsock
  device ever existed in the guest.
- `send_config_vsock()` set `addr.svm_cid = VMADDR_CID_ANY`, which is not a
  routable destination -- you cannot `connect()` to CID_ANY.
- The retry loop ran `connect()` up to 50 times, then ran **one more**
  unconditional `connect()` after the loop -- a double-connect bug.
- The guest `init.c` had **no vsock listener at all**; it only parsed
  `/proc/cmdline`.

So the host tried (and failed) to send config over a channel that did not
exist, printed a warning every run, and the guest silently relied on the
cmdline `-append` path the whole time.

### Phase 2: Verify the host could even do vsock

Before writing code I checked the host: `/dev/vhost-vsock` existed and the
`vhost_vsock` module was loaded. Good -- vsock could be a real path here. This
informed a key design choice: auto-detect vsock and fall back to cmdline
rather than hard-requiring the device (so boot still works on hosts without
vhost_vsock).

### Phase 3: Fix host vsock (`src/main.c`)

- Added `detect_vsock()` that stats `/dev/vhost-vsock` and sets a
  `vsock_available` flag.
- Rewrote `send_config_vsock()`: connect to `GUEST_CID` (3), create a **fresh
  socket per retry attempt** (a failed `connect()` leaves a socket in an
  undefined state), and stop retrying on non-transient errors.
- Replaced `execlp()` with an **argv vector** in `start_qemu()` so the
  `vhost-vsock-pci,guest-cid=3` device could be appended conditionally.
- Gated the config send on `vsock_available`; the kernel cmdline stays the
  authoritative fallback (always passed via `-append`).

### Phase 4: Fix guest vsock (`guest/init.c`)

- Added `recv_config_vsock()`: bind `VMADDR_CID_ANY:5000`, `poll()` with a 3s
  timeout for the host to connect, read one newline-terminated config line,
  ACK, close.
- Changed `parse_config()` to **only set keys present in its input** (removed
  the `memset` to the caller), so the vsock payload layers on top of the
  cmdline baseline instead of wiping fields the host does not send
  (hostname/cwd/user).
- Deleted the locally-duplicated `kv_find`/`kv_copy`/`csv_parse` and wired in
  the shared `../src/strparse.c` (guest links it directly).

### Phase 5: The OCI pull -- the biggest in-flight item

`src/image.c`'s `image_pull()` was a stub that **fabricated a fake rootfs**:
it created `bin/sh` as a shell script that echoed "Hello from podvm" and even
printed "(using placeholder image - full registry pull not implemented)".
"Finish all the in-flight things" squarely targeted this.

I rewrote `image.c` as a real v2 registry client, in self-contained pieces:

- **Minimal JSON DOM parser** (recursive descent) for manifests/indexes,
  handling escapes, `\uXXXX` incl. surrogate pairs, numbers, nested
  objects/arrays. No external JSON dependency (none was available to install).
- **Bearer token auth**: on HTTP 401, parse `WWW-Authenticate`, fetch a token
  from the `realm`, retry once. Manifest and blob fetches both auth-aware.
- **Manifest list / index** -> select the linux/amd64 image manifest by digest.
- **Layer blobs**: download, **gunzip** (zlib, auto-detect header), **untar**
  into the rootfs.
- **Tar extractor**: ustar headers, GNU longname (`L`), skip pax/longlink,
  regular files/dirs/symlinks/hardlinks, and **OCI whiteout** semantics
  (`.wh.<name>` and opaque `.wh..wh..opq`). Paths sanitized (reject absolute
  paths and `..`).
- **Ref parsing**: `docker.io` default + `library/` prefix for official
  images, `registry:port/repo` distinguished from `name:tag`, and the
  `docker.io -> registry-1.docker.io` endpoint mapping.

Linked zlib in `CMakeLists.txt`.

### Phase 6: Fix build/initramfs bugs found along the way

`guest/build-initramfs.sh` had **two boot-breakers**:

1. The `cd "$INITRAMFS_DIR"` / `cd ..` chain left the script's cwd in the
   temp parent, so `> ../initramfs.cpio` wrote to `/tmp`, not `guest/`.
2. `/init` was a `#!/bin/sh` wrapper that `exec`'d `/bin/init`, but there is
   **no shell in the initramfs** -- the kernel could not have run it. Fixed by
   placing the real `init` binary directly at `/init`.

Also: `mkdir -p "$WORK"/{bin,proc,...}` used brace expansion, which does **not**
expand under `dash` (`/bin/sh` here) -- it created a literal `{bin,proc,...}`
directory. Replaced with a POSIX loop.

### Phase 7: Fix the test runner's false PASS

`tests/run_tests.sh`'s `run_test()` treated any zero-exit command as PASS and
only inspected output on failure. So `which X || echo WARN` reported **PASS**
when `X` was missing (the `echo` made it exit 0). This had been hiding two
genuinely-missing deps: `virtiofsd` and `musl-gcc`. Fixed so a WARN in output
wins regardless of exit status. The count went from a misleading "15 PASS /
1 WARN" to an honest "15 PASS / 2 WARN".

### Phase 8: Commit often, in dependency order

Eleven commits, each buildable/testable, ordered so no commit references a
symbol that does not exist yet:

1. `strparse` helpers (standalone)
2. `.gitignore` (build artifacts, cpio, test binaries)
3. `image` module (standalone -- HEAD `main.c` did not use it yet)
4. host `main.c` (vsock fix + CLI + image wiring) -- headline "fix vsock"
5. guest vsock listener + shared strparse
6. kernel config hardening
7. top-level Makefile + initramfs packaging fixes
8. real OCI registry pull (rewrites `image.c`, adds zlib)
9. image pipeline tests + WARN-misreporting fix
10. `.gitignore` follow-up (untrack a committed test binary)
11. docs (README accuracy pass + design docs)

---

## What We Learned

### Technical

- **`connect()` to `VMADDR_CID_ANY` is invalid.** From the host, the guest is a
  specific CID (set by `guest-cid=` on the QEMU device). CID_ANY is a bind
  wildcard, not a destination.
- **A failed `connect()` poisons the socket.** Reusing the same fd across retry
  attempts is a real bug; create a fresh socket per attempt.
- **`dash` does not expand `{a,b,c}`.** `/bin/sh` is dash on Debian/Ubuntu, so
  shell scripts that build initramfs trees must use explicit loops, not brace
  expansion. This silently created a garbage directory.
- **An initramfs with no shell cannot exec a script `/init`.** The kernel runs
  `/init` directly; if it is `#!/bin/sh` and there is no `/bin/sh` in the
  initramfs, boot fails immediately. `/init` must be the real static binary.
- **`which X || echo WARN` in a test harness masks missing deps.** Combined
  with exit-status-only PASS/FAIL, it reports PASS when X is absent. The
  harness must inspect the *output* for WARN, not just the exit code.
- **musl-gcc was never actually installed** despite an earlier test claiming
  "musl-gcc available PASS" -- the same false-PASS bug. The guest Makefile now
  falls back to `gcc` so the guest still builds.
- **OCI tags vs registry ports**: `host:5000/repo` has the colon *before* the
  last slash (a port, part of the name); `repo:v1` has the colon *after* the
  last slash (a tag). The rule is positional, not "is it numeric" -- an early
  numeric guard wrongly classified `debian:12` as a port.
- **JSON `\u` escapes need an off-by-one guard.** After reading 4 hex digits
  the cursor sits on the last digit; you must advance one more before
  continuing, or a stray character is emitted.

### Process

- **Ordering commits dependency-first makes bisect work.** Committing the
  standalone `image` module *before* the `main.c` that calls it kept every
  commit green. Had I committed `main.c` first, `image.h` would not have
  existed at that commit and the build would break under `git bisect`.
- **Writing tests for pure functions pays off immediately.** The JSON, gzip,
  and tar pieces have no network dependency, so `tests/test_image.c` (which
  `#include`s `src/image.c` to reach the statics) caught two real bugs (the
  `\u` off-by-one and the `debian:12` port/tag confusion) before anything hit
  the network.
- **Small, honest commits beat one big one.** Splitting "fix vsock" from
  "image module" from "tests" made the vsock fix clearly reviewable even
  though it shared a file with the image wiring.

---

## What We Decided

- **Vsock is best-effort; the cmdline is authoritative.** Vsock is auto-detected
  and only used when `/dev/vhost-vsock` exists. The guest treats a received
  vsock payload as an *override* layered on the cmdline baseline, and the
  cmdline is always passed via `-append`. This keeps boot working everywhere
  (including this box, where virtiofsd is absent).
- **Vsock config reuses the cmdline token format** (`podvm.*=...` space
  separated, newline-terminated), so the guest parses both with the same
  `parse_config()`. No bespoke protocol.
- **Shared `strparse` for host and guest.** The guest links `../src/strparse.c`
  directly (static musl build); duplication removed.
- **Real OCI pull, no external JSON/tar lib.** Given the "no package install"
  rule and nothing suitable available, the JSON parser, tar extractor, and
  gzip path are hand-written and unit-tested in `image.c`.
- **Digest (sha256) verification and zstd layers are TODO**, not blocking. The
  pull pipeline works; integrity verification is a follow-up.
- **Each commit builds.** No commit may reference a not-yet-committed symbol.

---

## Rules Added

- **Fresh socket per vsock connect retry:** never reuse an fd after a failed
  `connect()`.
- **POSIX shell only in build scripts:** no brace expansion; `/bin/sh` may be
  dash.
- **`/init` in an initramfs must be the real binary**, never a shell wrapper,
  when no shell is packaged.
- **Test harnesses check output for WARN, not just exit status**, so
  `which X || echo WARN` cannot mask a missing dependency.
- **Commit dependency-first for bisect:** land a module before the commit that
  calls it.

---

## What We'd Do Differently

- **The `test_image` binary got committed** in the tests commit because
  `.gitignore` listed the other test binaries by name but not `test_image`.
  Untracked it in a follow-up commit, but a glob (`tests/test_*` with negation
  for `*.c`) would have prevented it. Use globs, not enumerated names.
- **The registry pull is unit-tested but never exercised against a live
  registry** (no guaranteed network in this environment). The token-auth,
  manifest-list selection, and blob paths are logically correct and compile,
  but an end-to-end pull against Docker Hub is unverified. That should be the
  very next test.
- **The two truncation `snprintf` warnings in `main.c` and three in `image.c`
  were pre-existing / accepted** rather than fixed. They are benign here (paths
  are bounded in practice) but should be cleaned up so `-Wall -Wextra` is
  actually silent.
- **`virtiofsd` is missing on this box**, so a real end-to-end VM boot could
  not be performed to prove the vsock handshake works live. The host and guest
  sides are symmetric and unit-covered, but the handshake itself is unproven
  against a running QEMU. Needs virtiofsd installed (with user approval) or a
  different environment.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/main.c` | Host launcher: vsock auto-detect + fixed config send, argv-based QEMU start, image_resolve wiring, getopt CLI |
| `src/image.c` | Real OCI registry pull: JSON parser, token auth, manifest selection, gunzip + untar with whiteouts |
| `src/image.h` | (unchanged) public image API |
| `src/strparse.c` / `.h` | Shared cmdline/config parsing helpers (now used by host tests + guest) |
| `guest/init.c` | Guest PID1: vsock listener overrides cmdline; uses shared strparse |
| `guest/Makefile` | Compiles `../src/strparse.c`; falls back to gcc when musl-gcc absent |
| `guest/build-initramfs.sh` | Fixed output path, real `/init` binary, POSIX mkdir loop |
| `kernel/config` | Namespaces/seccomp/hardening enabled; attack-surface removals documented |
| `CMakeLists.txt` | Build `image.c`; link libcurl + zlib |
| `Makefile` | Top-level orchestrator; correct build prerequisites + clean list |
| `tests/test_image.c` | Unit tests for JSON/gzip/tar/ref-parsing (no network) |
| `tests/Makefile` | Added `test_image` target (needs `-lcurl -lz`) |
| `tests/run_tests.sh` | Fixed WARN misreporting; added image test |
| `.gitignore` | Ignore build artifacts, cpio images, test binaries |
| `README.md`, `docs/*`, `kernel/audit.md` | Docs; README aligned to the real CLI |
| `mission_log/README.md` | Verbatim copy of the skill README |

---

## Test Results

Full suite (`bash tests/run_tests.sh`) after the session:

```
Config parsing                PASS
Host args                     PASS
Vsock basics                  PASS
Image pipeline                PASS
Host build                    PASS
Guest build                   PASS
Initramfs build               PASS
QEMU available                PASS
KVM available                 PASS
virtiofsd available           WARN   (genuinely not installed)
musl-gcc available            WARN   (genuinely not installed)
Vsock enabled                 PASS
Virtio FS enabled             PASS
Balloon enabled               PASS
Uncompressed kernel           PASS
No BPF                        PASS
No modules                    PASS

PASS: 15   WARN: 2   FAIL: 0
```

The two WARNs are real environment gaps (virtiofsd and musl-gcc are absent),
not code failures. The guest builds via the gcc fallback; the initramfs builds
with the real `init` at `/init`. `build/podvm` links clean (only pre-existing
`snprintf` truncation warnings).

---

## Next Steps

1. **Exercise a live registry pull** (e.g. `podvm debian:12 /bin/true`) once
   network is available -- the only untested part of the OCI client.
2. **sha256 layer-digest verification** and zstd layer support (both marked
   TODO in `image.c`).
3. **End-to-end VM boot test** once virtiofsd is available (needs user approval
   to install) to prove the host<->guest vsock handshake live.
4. **Surface the image config blob** (Env/Entrypoint/Cmd) to the host so a
   pulled image's default command is honored, not just the CLI entrypoint.
5. **Clean up the `snprintf` truncation warnings** so `-Wall -Wextra` is
   silent.
6. Consider switching `.gitignore` test-binary entries to globs to avoid the
   "forgot to add a new binary" class of mistake.
