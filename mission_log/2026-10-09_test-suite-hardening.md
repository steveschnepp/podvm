---
name: test-suite-hardening
description: audit what the 33-test suite actually covers, then close the gaps -- real-binary CLI matrix, store/json unit tests, harness WARN-masking fix -- and find a stale cache path that made the "native pull" e2e fake its own success
project: podvm
previous: 2026-10-09_rm-and-run-flags.md
---

# Mission Log: Test Suite Hardening

---

## What We Did

Started from a coverage audit (mapping every flag, subcommand and
src module against every test) instead of writing tests blind. The
audit's ranked findings, worked in order, one commit:

1. **`test_host_args.c` was a mock of a parser that no longer exists.**
   It hand-copied main.c's option loop and had drifted: it still knew
   `-it` (which the real getopt never accepted), and its `args[256]`
   buffer had been 4096 in the real binary for two sessions. Deleted
   it. Replaced with `tests/cli_matrix.sh`: 30 cases driving the REAL
   `build/podvm`, no VM, 0.26s. The design trick: every case uses a
   nonexistent *path* positional, so the run fails at image resolution
   ("directory not found") -- AFTER getopt, before podman/network/VM.
   A dropped getopt letter therefore shows up as "invalid option"
   instead of being invisible. The matrix case passes every short flag
   in the optstring in one invocation. Also covers `--help`, missing
   option args, per-flag host-side validation (-w/-n/-e/--network/-p/
   -v, detach rules), and subcommand argument errors (rm/stop/logs/rmi/
   build/images).
2. **`test_integration.c` was dead weight**: it ran the same
   cmake/guest/initramfs builds and the same qemu/kvm greps that
   run_tests.sh already does, plus a virtiofsd check for a feature
   podvm does not use. Deleted; run_tests.sh is the integration layer.
3. **`test_kernel_config` was built but never run** -- run_tests.sh
   re-checked kernel/config with six ad-hoc greps. The binary checks a
   superset (vsock, 9p, balloon, uncompressed, no-BPF, no-modules plus
   ~40 more). Swapped the six greps for one run of the binary.
4. **New unit tests for the two untested src modules:**
   - `test_json.c`: the DOM parser contract -- shape access, escape and
     `\uXXXX` (incl. surrogate pair) decoding, numbers/bool/null,
     rejection of malformed input, and the pinned fact that trailing
     garbage after the first value is tolerated (store.c relies on it).
   - `test_store.c`: `store_find`/`store_chain`/`store_image_config`
     against a temp fixture tree via `XDG_DATA_HOME` -- no podman, no
     VM, no network, so the podman-storage reader is finally testable
     on any host. Pins two historical bugs: the `localhost/<name>:<tag>`
     candidate matching, and the big-data base64 filename encoding
     (cross-checked with an independent base64 in the test, so a
     regression in store.c's encoder cannot hide).
5. **Harness fixes in `run_tests.sh`:**
   - The WARN heuristic could mask real failures: any non-zero exit
     whose output contained "No such file|not found|Permission denied"
     was reported WARN, and an output containing the word WARN won
     over exit status. Now: exit status decides PASS vs FAIL; a WARN
     token in otherwise-successful output downgrades PASS -> WARN.
   - Orphan-QEMU pre-check before the e2e block (the vsock CID is
     hardcoded to 3; one leaked QEMU turns every later boot into a
     mysterious flake). Prints a loud warning, never fails the suite.
   - Documented the e2e ordering coupling where it bites:
     `e2e_images.sh` deletes cached alpine and must stay last.
6. **Rewrote `docs/tests.md`** -- it described a 14-test suite that no
   longer existed (test_host_args, test_integration, virtiofsd WARN).

### The suite found its own bug

The first full-suite run after the rewiring failed `E2E native pull`
(rc=124). Investigation: the guest booted and exec'd busybox's own
default Cmd (`sh`) and sat at a prompt until the timeout. Root cause
was IN THE TEST: `e2e_native.sh` cleared its cache with
`rm -rf /tmp/podvm/images/docker.io/library/busybox` -- the pre-flatten
nested path. Since session 5 flattened the cache to
`library+busybox/latest`, the rm was a no-op, the run resolved from
cache ("using cached image busybox:latest"), no pull happened, and the
pull-completion assertion failed. The e2e had been one cache-wipe away
from silently testing nothing for a day. Fixed the path; solo re-run:
PASS with a real digest-verified pull.

---

## What We Learned

### Technical

- **A test that clears a fixture must use the same path layout the
  code writes.** After the cache flatten, `e2e_native.sh`'s rm was a
  silent no-op and the test degraded to "cache hit + hang". Any e2e
  that pre-clears state should assert the clear took effect (or assert
  on a marker only the intended path can produce).
- **The real-binary CLI trick**: fail at the first post-getopt
  validation with a nonexistent *path* positional -- hermetic (no
  network, unlike a bogus image ref which triggers a registry pull)
  and ordering-proof (resolution happens after getopt).
- **`store.c` was unit-testable all along** -- it reads storage through
  `XDG_DATA_HOME`, so a temp fixture tree replaces podman entirely. A
  300-line module had survived three sessions with only VM-backed
  coverage.
- **`json_parse` is a "value or NULL" parser with no trailing-garbage
  check.** Fine for its callers; now pinned by a test so nobody
  "fixes" it into strictness and breaks store.c's manifest fallback.

### Process

- **Audit before writing.** Mapping source modules and CLI surface to
  existing tests first turned "write a full test suite" into five
  ranked gaps; three of the five turned out to be deletions or
  rewiring, not new tests.
- **A failing e2e during unrelated work is still a gift.** The native
  pull failure was not caused by this session's changes, but it was a
  real defect in the suite's own honesty -- worth its own fix.
- **The WARN-masking heuristic dated from before the e2es had proper
  SKIP paths.** Every check that legitimately warns now echoes WARN
  explicitly and exits 0, so the downgrade rule had no remaining
  legitimate use.

## What We Decided

- No replacement for test_host_args' mock: the matrix test tests more
  (the real binary) and asserts less fiction (no phantom `-it`).
- `test_integration` stays deleted: run_tests.sh is the integration
  layer; a C binary re-shelling cmake added a second source of truth.
- store/json unit tests compile the real sources (same style as
  test_image), no mocks -- consistent with the container/imgcfg
  pattern.
- The full suite ran clean after the changes: **30 PASS / 1 WARN
  (musl-gcc, pre-existing) / 0 FAIL** (row count moved from 33 because
  six kernel-config rows merged into one and three rows were removed/
  added -- coverage went up, not down).

## Rules Added

- **An e2e that pre-clears state must clear the layout the code
  actually writes**, and ideally prove the clear took effect.
- **CLI-surface tests drive the real binary**, failing at resolution
  with a nonexistent path -- never a copied parser, never a network
  ref.
- **Exit status decides PASS/FAIL**; WARN only ever comes from an
  explicit WARN token in successful output. Never downgrade a failure.
- **Test binaries that exist must be wired into run_tests.sh** (the
  kernel_config binary was built but unrun for several sessions).

## What We'd Do Differently

- **The cache flatten in session 5 should have updated e2e_native.sh's
  rm path in the same commit.** Layout changes need a grep for the old
  path across tests/.
- **The `-it` drift proves mock-copy tests rot silently.** The rule
  "unit tests link the real code" was already written after container.c
  landed; test_host_args should have been converted then, not today.
- **The full suite was only run at the END.** Intermediate validation
  was unit-only; running the e2e-heavy suite once mid-session would
  have surfaced the native-pull failure earlier (it was pre-existing).

## Files Changed

| File | Purpose |
|------|---------|
| `tests/cli_matrix.sh` | New: 30 real-binary CLI cases, hermetic, no VM |
| `tests/test_json.c` | New: json.c parser contract |
| `tests/test_store.c` | New: store.c against an XDG_DATA_HOME fixture tree |
| `tests/test_host_args.c` | Deleted: drifted mock of main.c's parser |
| `tests/test_integration.c` | Deleted: redundant with run_tests.sh |
| `tests/run_tests.sh` | PASS/FAIL from exit status; cli_matrix + json + store wired; kernel-config binary replaces greps; orphan-QEMU warning; ordering note |
| `tests/Makefile` | TESTS list + build rules updated |
| `tests/e2e_native.sh` | Cache clear uses the flattened `library+busybox` path |
| `docs/tests.md` | Rewritten to match the suite as it is |

## Test Results

```
Config parsing                PASS
Vsock basics                  PASS
Image pipeline                PASS
SHA-256                       PASS
Image config fold             PASS
User resolution               PASS
Container state               PASS
Build parser                  PASS
JSON parser                   PASS   (new)
Podman store                  PASS   (new)
Host build                    PASS
CLI matrix (real binary)      PASS   (new, replaces Host args)
Guest build                   PASS
Initramfs build               PASS
QEMU available                PASS
KVM available                 PASS
9p modules available          PASS
musl-gcc available            WARN   (pre-existing; not installed)
Kernel config                 PASS   (binary, replaces 6 greps)
E2E boot (9p + vsock)         PASS
E2E podman image              PASS
E2E net (DNS+HTTPS)           PASS
E2E image config (default cmd) PASS
E2E native pull               PASS   (after fixing its stale cache path)
E2E interactive (-i)          PASS
E2E write-back (--name)       PASS
E2E rm (rm + --rm)            PASS
E2E run flags (podman parity) PASS
E2E build (Dockerfile)        PASS
E2E detach (-d/stop/logs)     PASS
E2E images/rmi (cache)        PASS

PASS: 30   WARN: 1   FAIL: 0
```

## Next Steps

1. Build-driver unit tests (`cmd_build` chain assembly, whiteouts) --
   the one large src region still covered only by e2e.
2. Coverage tooling (gcov target) to check this audit against numbers.
3. Unchanged from previous sessions: unique vsock CIDs (one leaked QEMU
   still breaks all later boots), OCI blob export from build,
   multi-stage builds, `logs -f`, the performance phase (ADR-019/009),
   FreeBSD (ADR-021).

---

## Session 2: Clearing the musl WARN (2026-10-09)

Session 1 ended at 30 PASS / 1 WARN, the WARN being the pre-existing
`musl-gcc available` row. Closed it, and in doing so gave the guest
its first musl build.

### What We Did

1. **Installed musl-tools** (1.2.5-3.1~deb13u1, user-approved per the
   no-install rule). The guest Makefile picks up `musl-gcc`
   automatically (`guest/Makefile:4-5`).
2. **The musl build then failed on `linux/vm_sockets.h`.** musl-gcc
   deliberately omits `/usr/include` from its search path so glibc
   headers cannot contaminate a musl static build -- which also hides
   the kernel UAPI headers. Verified by printing the search path:
   only `/usr/include/x86_64-linux-musl` and gcc's own dir.
3. **Fixed with `-idirafter /usr/include`** (guarded on musl being
   present). `-idirafter` appends AFTER all standard search dirs, so
   musl's own headers still win and only what musl lacks -- the UAPI
   headers under `linux/`, `asm/` -- comes from `/usr/include`. This is
   the standard musl-plus-kernel-headers idiom; `-I/usr/include` would
   have been wrong (it searches early and pulls in glibc).
4. **Full suite re-run on the musl-built guest**: 31 PASS / 0 WARN /
   0 FAIL. The guest `init` is now statically linked + stripped,
   51365 bytes of text vs ~64KB for the glibc-static build.
5. **`.gitignore` tidy**: the test-binary list still named the two
   files deleted in session 1 and missed `test_json`/`test_store`.
   Collapsed to `tests/test_*` + `!tests/test_*.c`, verified with
   `git check-ignore`.

Commits: `9edb0a3` (guest musl build + .gitignore).

### What We Learned

#### Technical

- **musl-gcc's missing `/usr/include` is a feature, not a bug** -- the
  fix is `-idirafter`, never `-I`. The include-order difference is the
  whole point of musl's isolated search path.
- **A WARN-free suite is not the same as a tested suite.** The musl
  row read WARN for sessions because the guest had literally never
  been compiled with the compiler its Makefile prefers. Installing the
  tool and fixing the build tested a path no test had exercised.
- **`-idirafter` cost nothing at runtime**: the smaller musl binary
  booted in all twelve VM e2e paths unchanged -- the guest ABI the
  host depends on is the vsock/9p protocol, not the libc.

#### Process

- **Environment warnings deserve a real fix, not suppression.** The
  WARN row had been on the board for the whole project; the honest
  fix was installing the tool (with approval) and repairing what its
  absence had been masking -- not downgrading the check.

### What We Decided

- musl remains the guest's preferred compiler with a gcc fallback;
  the UAPI include is conditional on musl so the fallback path is
  untouched.
- `.gitignore` uses a pattern, not a hand-maintained list -- same
  reasoning as the mission-log README's "no index" rule.

### Rules Added

- **Adding a compiler to the host means rebuilding every artifact it
  can affect**, not just the one that triggered the change: the guest
  binary is part of the test surface for all e2e.
- **`-idirafter` for UAPI headers under musl**, never `-I/usr/include`.

### Files Changed

| File | Purpose |
|------|---------|
| `guest/Makefile` | `MUSL_UAPI = -idirafter /usr/include` when musl-gcc present |
| `.gitignore` | Test binaries as a pattern; sources negated |

### Test Results

```
musl-gcc available            PASS   (was WARN)
...
PASS: 31   WARN: 0   FAIL: 0
```

### Next Steps

1. Session 1's list unchanged: build-driver unit tests, gcov target,
   unique vsock CIDs, OCI blob export, performance phase, FreeBSD.

