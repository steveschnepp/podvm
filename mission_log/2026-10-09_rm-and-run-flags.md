---
name: rm-and-run-flags
description: podvm rm + --rm close the write-back lifecycle, then tier-1 (-e/-u/-w) and tier-2 (-v/-p/--network/--read-only) podman flag parity -- three sessions, one log
project: podvm
previous: 2026-10-09_config-channel-and-runtime-gaps.md
---

# Mission Log: Container-State Removal and Podman Flag Parity

---

## Session 1: `podvm rm` and `--rm` (2026-10-09)

Picked up item 2 from `2026-10-09_config-channel-and-runtime-gaps.md`:
write-back state (ADR-026) could be created but nothing could remove it,
so `$XDG_STATE_HOME/podvm/containers/` grew without bound.

### What We Did

1. **Extracted `src/container.c`** from `main.c`: name validation, state
   dir construction, and an `nftw`-based recursive delete. Same pattern
   as `imgcfg.c`/`userdb.c` -- pure logic out of `main.c` so the unit
   tests link the real code instead of a mock (the `test_host_args.c`
   mock-copy style is the old pattern).

2. **`podvm rm <name>...`**: deletes named containers' state. A missing
   name is an error (docker semantics: a zero exit would hide typos).
   Every name is attempted so one bulk invocation reports all problems;
   exit status is non-zero if any failed.

3. **`--rm`** (long-only; `-r` is taken by `--initramfs`): discards the
   named state when the run ends, however it ended. Never overrides the
   run's own exit status. No-op without `--name`.

4. **Hardened name validation**: `--name` previously only rejected `/`.
   It now also rejects `.`, `..`, and a leading `-` -- `--name ..` would
   have pointed the state dir at the containers root itself, and
   `podvm rm -x` would have read the name as an option.

5. **The new e2e caught a real bug on its first run.** `--rm` aborted
   with `EISDIR` and left the state behind. Root cause:
   overlayfs leaves its work dir (`w/work`) at **mode 000**; `nftw`
   cannot `opendir` it and glibc reports that as `FTW_DNR`, *not*
   `FTW_D` (this is true under `FTW_DEPTH` too). The delete pass then
   called `unlink()` on a directory. Fixed with a chmod pre-pass over
   the tree plus `rmdir`-not-`unlink` for `FTW_D`/`FTW_DNR` arrivals.
   Unit test gained a mode-000 regression case.

Final state: **28 PASS / 1 WARN (musl-gcc) / 0 FAIL**, up from 26/1/0.

### What We Learned

#### Technical

- **glibc `nftw` reports an unreadable directory as `FTW_DNR`, not
  `FTW_D`** -- and `FTW_DEPTH` does not change that. Any `nftw` walker
  must handle `FTW_DNR` in *every* callback or it silently mishandles
  the one directory type it cannot read.
- **The overlayfs work dir is mode 000 by design.** Shell `rm -rf`
  copes (it chmods first), so a hand check of `rm -rf <state dir>` passes
  while a custom walker fails. Podvm-only bug, invisible without a test
  that boots the real thing.
- **`rmdir()` needs write permission on the parent, not on the
  directory itself** -- so a mode-000 directory we own is deletable;
  only `unlink()` (wrong for dirs) fails, with `EISDIR`.
- **`docker rm` semantics matter even in a PoC**: missing name = error.
  Being strict costs nothing and catches typos in scripts.

#### Process

- **The e2e earned its keep immediately**, same as `e2e_interactive.py`
  last session: the unit test passed, the review passed, the VM boot
  failed. Boot the thing.
- **The regression got a unit-test row, not just a fix** -- the rule from
  last session, applied on the same day it was written.

### What We Decided

- Missing-name error (docker), not idempotent success.
- chmod pre-pass + depth-first delete over invoking `rm -rf`: no shell
  quoting, no external dependency, testable in-process.
- `cmd_rm` refuses to delete any path not containing
  `/podvm/containers/` -- defense in depth for a recursive delete, even
  though the path is built by podvm.
- `--rm` cleanup runs after QEMU was attempted (both the normal and the
  start-failure path), never before image resolution: the line is "this
  named container's run ended", not "this process ran".
- Top-level `make clean` now delegates `tests/` cleanup to the tests
  Makefile; the hand-maintained binary list there was already stale.

### Rules Added

- **Handle `FTW_DNR` in every `nftw` callback.** Unreadable dirs arrive
  as `FTW_DNR` even with `FTW_DEPTH`; defaulting them to unlink aborts
  with `EISDIR`.
- **Assume container state contains mode-000 directories** (overlayfs
  work dirs). Anything that walks or copies a container's state must
  chmod first or tolerate unreadable dirs.
- **Recursive deletes: never follow symlinks (`FTW_PHYS`), and verify
  the constructed path root before deleting.**
- **Validate container names at every entry point** (`--name`, `rm`)
  against `.`, `..`, `/`, and leading `-` -- path building and option
  parsing both trust them.

### What We'd Do Differently

- **The mode-000 overlay work dir was already documented** in ADR-026
  work, so the unit test should have been written with that hostile
  shape from the start instead of being added after the e2e caught the
  bug. When a filesystem's quirks are known, encode them as test rows
  on day one.
- **Considered shelling out to `rm -rf`** and rejected it for no shell
  quoting and testability; the C walker cost ~40 lines and still had a
  bug the shell does not have. A fair price, but the first draft should
  have copied `rm`'s behavior (chmod-then-descend) rather than assuming
  every directory is readable.
- **The top-level `make clean` binary list went stale two sessions
  ago** and was only noticed now; delegating to `tests/Makefile` should
  have happened when `test_imgcfg` landed.

### Files Changed

| File | Purpose |
|------|---------|
| `src/container.c` | New: name validation, state dir, `nftw` rm -rf (chmod pre-pass) |
| `src/container.h` | New: `container_name_valid`, `container_state_dir`, `container_rm_rf` |
| `src/main.c` | `rm` subcommand, `--rm`, name validation, usage |
| `tests/test_container.c` | New: 16 name cases, state dir env matrix, rm -rf incl. mode-000 + symlink |
| `tests/e2e_rm.sh` | New: rm mechanics (no VM) + `--rm` lifecycle (real VMs) |
| `CMakeLists.txt`, `Makefile`, `tests/Makefile`, `tests/run_tests.sh`, `.gitignore` | Build/test wiring |
| `README.md` | Usage/options refreshed (was missing `-n` too) |
| `adrs/podvm_adr026_host-side-writeback.md` | "no podvm rm yet" note replaced |

### Test Results

```
Container state               PASS   (new)
E2E rm (rm + --rm)            PASS   (new)
...
PASS: 28   WARN: 1   FAIL: 0
```

### Next Steps

1. Guard write-back concurrency (`flock` on the state dir) -- `rm` could
   then refuse a live container. Still the only correctness hole.
2. Tier-1 podman flag parity: `-e/--env` (+`--env-file`), `-u/--user`,
   `-w/--workdir` -- plumbing exists, flag-only work.
3. Tier-2: `-v/--volume` (+`--tmpfs`), `-p/--publish`,
   `--network=none`, `--read-only`.
4. `podvm images`/`rmi`/cache GC (the state-GC twin for
   `/tmp/podvm/images`), layer compaction, `podvm build`, the
   performance phase (ADR-019/009), FreeBSD (ADR-021).

---

## Session 2: Tier-1 Podman Flag Parity (2026-10-09)

Implemented the three flags whose plumbing already existed: `-e/--env`
(+`--env-file`), `-u/--user`, `-w/--workdir`. One commit per flag, each
with its own e2e rows, so all three bisect cleanly.

### What We Did

1. **`-e/--env` + `--env-file` (`9f182d7`).** `config.env` and the
   escape channel (ADR-025) already existed, but nothing fed them from
   the CLI and `imgcfg_fold()` silently dropped `base.env` (it was
   always zeroed -- the field had no writer). Fixed the plumbing, and
   **flipped the fold's env order**: image vars first, then CLI vars.
   The guest `setenv`s in list order (later wins), so the old
   "base first" order would have made the image *beat* `-e` on a
   duplicated key -- exactly backwards from docker. Also started
   ensuring `HOME` in `build_config_tokens()` so the bare-rootfs path
   (where the fold never runs) matches the image path.
2. **`-u/--user` (`91222ed`).** Pure flag: `userdb.c` had resolved
   names against the container's `/etc/passwd` after chroot since the
   previous session; the CLI just could not ask for it. e2e proves
   numeric, named (`nobody`), CLI-over-image-`USER`, and
   image-`USER`-without-`-u` in both directions.
3. **`-w/--workdir` (`8d9b421`).** Flag plus one guest behavior change:
   docker creates a missing WORKDIR, so guest init now `mkdir_p`s the
   cwd and only warns (continuing in `/`) if that cannot work (e.g.
   read-only rootfs). Previously a nonexistent cwd silently landed the
   command in `/`.
4. **Drive-by fix (`e0df1fe`):** the guest's `resolv.conf` write passed
   a hardcoded 22 for a 21-byte literal (1-byte overread, flagged by
   the compiler once `init.c` was rebuilt). `sizeof - 1` now.

One e2e script grew with the session: `tests/e2e_runflags.sh`, wired
into `run_tests.sh` as "E2E run flags (-e/-u/-w)".

### What We Learned

#### Technical

- **A precedence rule no caller exercises is untested.**
  `imgcfg_fold()`'s `base.env` branch was dead code (nothing set
  `base.env`), and its order was wrong for the CLI case. The 12 table
  cases all passed while the rule they were written to protect would
  have failed the moment `-e` existed. Wire the input, then trust the
  order.
- **The guest's env application order is the real override mechanism.**
  `setenv(k, v, 1)` in list order means "last entry wins"; host-side
  ordering is therefore a semantic decision, not a cosmetic one.
- **Nothing tests the short-flag matrix of the real binary.**
  `test_host_args.c` parses a mock copy; the e2e scripts never pass
  `-m/-c/-k/-r`. Dropping `m:` from the optstring while adding `e:`
  would have shipped unnoticed -- caught only by re-reading the diff.

#### Process

- **The mission-log linter is stricter than the earlier logs assumed**
  (same-day `- Previous:` chains, required "What We'd Do Differently",
  frontmatter on every log). Because it judges the whole directory, one
  new log surfaced findings in five old ones; fixed retroactively in
  the same docs commit (additive one-line links + the missing 2024
  frontmatter). Run the linter on the directory, not the file.

### What We Decided

- CLI env is appended after image env in the fold (docker override
  semantics), with duplicate keys resolved by the guest's later-wins
  `setenv` -- rather than adding key-level deduplication, which can
  wait for a real need.
- `-w` requires an absolute path (rejecting relative early) and creates
  missing directories, matching docker's WORKDIR behavior.
- `--env-file` rejects lines without `=` (no host-env-inheritance
  shorthand yet; docker's bare-KEY form can come later).
- Short-flag regressions get a manual parse sanity run:
  `podvm -m -c -k -r -i -n -e -u -w <bogus image>` must fail at image
  resolution, never at getopt.

### Rules Added

- **When adding a flag, re-read the optstring diff by eye.** getopt
  short-option strings are untested surface in this repo; a dropped
  letter is invisible to the e2e suite.
- **Dead precedence branches are latent bugs:** if a fold/test case
  guards an ordering rule, ensure some caller exercises both sides.

### Files Changed

| File | Purpose |
|------|---------|
| `src/main.c` | `-e`/`--env-file`/`-u`/`-w` flags, `env_add`/`env_add_file`, HOME ensure in `build_config_tokens()` |
| `src/imgcfg.c` | Env order flipped: image first, CLI last |
| `guest/init.c` | cwd mkdir-if-missing + warn; resolv.conf overread fix |
| `tests/test_imgcfg.c` | Two new env-order cases (7b, 7c) |
| `tests/e2e_runflags.sh` | New: 10 boots covering all three flags |
| `tests/run_tests.sh`, `README.md` | Wiring + docs |

### Test Results

Per-flag e2e green at each commit (`E2E-RUNFLAGS PASS` after 10 VM
boots: env values with spaces/commas, PATH override, env-file,
numeric/named/override users, existing/created workdir, plus two
host-side rejections). Full-suite re-run queued after tier 2.

### Next Steps

1. Tier 2: `-p/--publish`, `--network=none`, `--read-only`,
   `-v/--volume` (+`--tmpfs`).
2. `flock` on the state dir (still the only correctness hole).
3. `podvm images`/`rmi`/cache GC, layer compaction, `podvm build`,
   performance phase, FreeBSD.

---

## Session 3: Tier-2 Podman Flag Parity (2026-10-09)

Four more flags, one commit each: `-p/--publish` (`e713f9e`),
`--network=none` (`4c51abd`), `--read-only` (`01bc85d`),
`-v/--volume` + `--tmpfs` (`e11c37e`). Full suite after: **29 PASS /
1 WARN (musl-gcc) / 0 FAIL**.

### What We Did

1. **`-p/--publish`**: `[hostip:]hostport:containerport` (tcp) becomes
   QEMU slirp `hostfwd` rules appended to the user netdev; host-side
   argv only. The e2e serves a one-shot HTTP response from the guest
   with busybox `nc` and curls it through the forward -- alpine's
   busybox has **no httpd applet**, which the first draft assumed and
   the first run disproved.
2. **`--network=none`**: no netdev attached; the guest learns via a
   new `podvm.net=none` token and skips the eth0 ioctl. `host`/`bridge`
   are rejected with a pointer to ADR-009 (tap belongs to the
   performance phase), and `-p` + `none` is caught as a contradiction.
3. **`--read-only`**: MS_RDONLY overlay mount (podman path) /
   `readonly=on` fsdev (native path). This one fought back -- see
   below.
4. **`-v/--volume` + `--tmpfs`**: each volume is served over its own 9p
   tag (`vol0`..) since the rootfs tag is taken; the guest mounts them
   at container paths after chroot. Both flags travel as new
   `podvm.vol=`/`podvm.tmpfs=` csv tokens, element-wise escaped so
   targets may contain spaces. `:ro` maps to `readonly=on` + `MS_RDONLY`.

### What We Learned

#### Technical

- **overlayfs in an unprivileged userns requires an upperdir.** The
  obvious read-only overlay -- `lowerdir=` only -- is init-userns-only
  and fails with EINVAL in the QEMU child's userns. Verified with a
  minimal `unshare -Urm` repro before reworking. The read-only overlay
  keeps an upper (throwaway tmpfs) and is mounted `MS_RDONLY` instead.
- **An empty branch in an if/else-if chain is a silent no-op** -- the
  first `--read-only` restructure left `upperdir=,workdir=` empty and
  the mount failed with a bare EINVAL. `strace -f -e mount` on the
  child showed the exact option string in one shot. When a mount errno
  is opaque, strace the syscall; don't reason from the source.
- **Test a container command against the image before baking it into
  an e2e.** `httpd` is not in alpine's busybox; `nc` is. One
  `podman run --rm` check would have saved a failed e2e cycle.
- **Background VM tests must kill the whole process group** (setsid +
  `kill -- -PGID`), or QEMU outlives the test holding the vsock CID.
- **`-p` hostfwd accepts connections before the guest is ready**
  (QEMU holds them), so polling from t=0 works; no readiness sleep is
  needed on the host side.

#### Process

- Every tier-2 feature landed as its own commit with its e2e rows, so
  the whole day bisects: 8 flag commits, 1 docs commit per session,
  1 drive-by guest fix.

### What We Decided

- `-p`: tcp only, numeric ports, hostip defaults to 127.0.0.1 (docker's
  default bind); udp and IPv6 hostips deferred.
- `--network`: only `slirp` and `none` exist; pretending `host` works
  would be a lie until ADR-009's tap/bridge.
- `--read-only` creates no state dir even with `-n` (throwaway tmpfs
  upper + ro mount), so `--rm` has nothing to clean.
- Volumes: bind mounts of existing host *directories* only (no named
  volumes, no file mounts); `:` and `,` rejected in paths because
  QEMU's fsdev parsing and the guest's `tag:target:ro` split both
  depend on them. Guest mount failures warn but do not abort (podvm's
  best-effort posture; docker would refuse to start).
- `--tmpfs` takes a bare absolute path (no mode= options yet).

### Rules Added

- **An overlayfs mount in an unprivileged userns needs an upperdir**;
  read-only overlays must be "upper + MS_RDONLY", not "lower-only".
- **When a mount() fails with an opaque errno, strace the child's
  syscall** rather than re-reading the option-building code.
- **Probe applets/tools inside the actual image** before encoding them
  in an e2e (`command -v` under `podman run`).
- **Tests that background a VM: setsid + process-group kill**, never a
  plain kill of the launcher.

### Files Changed

| File | Purpose |
|------|---------|
| `src/main.c` | `-p`, `--network`, `--read-only`, `-v`, `--tmpfs`; per-volume fsdevs; hostfwd netdev; new config tokens |
| `guest/init.c` | `podvm.net=`, `podvm.vol=`, `podvm.tmpfs=` parsing; volume/tmpfs mounts after chroot |
| `tests/e2e_runflags.sh` | Grows to 16 boots + 12 host-side rejection checks |
| `tests/run_tests.sh`, `README.md` | Wiring (label now "podman parity") + docs |

### Test Results

```
E2E run flags (podman parity) PASS   (16 boots: all 8 flags)
...
PASS: 29   WARN: 1   FAIL: 0
```

### Next Steps

1. `flock` on the write-back state dir (still the only correctness
   hole); `rm` could then refuse a live container.
2. `podvm images`/`rmi`/cache GC for `/tmp/podvm/images` (the state-GC
   twin).
3. `--hostname` is now nearly free -- guest init already honors
   `podvm.hostname=`; only the host flag is missing.
4. Layer compaction (ADR-020 revisit), `podvm build`, the performance
   phase (ADR-019/009), FreeBSD (ADR-021).

---

## Session 4: The flock Guard (2026-10-09)

Closed the last known correctness hole: two live `--name` runs shared
one overlay upper, which overlayfs does not support for two writers.
Two commits: primitives (`faefac5`), wiring (`f452182`). Suite after:
**29 PASS / 1 WARN / 0 FAIL**.

### What We Did

1. **`container_lock(name)`** -- exclusive, non-blocking flock on
   `$XDG_STATE_HOME/podvm/locks/<name>`, a *sibling* of `containers/`
   that is never deleted: the lock file's inode is the name's identity,
   so `rm`/`--rm` deleting the state dir cannot race a starting run
   into locking a fresh file. The returned fd is deliberately not
   CLOEXEC -- the QEMU child inherits it, so the lock's lifetime is
   the VM's, even if the launcher is SIGKILLed.
2. **Run path**: a named run on the overlay path (podman storage, not
   `--read-only`) locks before booting; a second run fails fast with a
   diagnosis and no second VM starts. Unnamed runs and `--read-only`
   (throwaway tmpfs upper) share nothing and skip the lock.
3. **`rm` holds the lock across the whole deletion** -- refusing a live
   name, and closing the TOCTOU window that a probe-then-delete would
   have left open (a run starting between probe and delete would boot
   a VM whose state dir is being removed underneath it).
4. **`e2e_rm.sh` part 3** proves it with real VMs: run A stays alive;
   a second run of the same name is refused without booting; `rm` is
   refused while live; after killing A's process group, both work
   again. The lock-release wait uses `flock(1)` as a host probe.

### What We Learned

#### Technical

- **`flock` is per open-file-description, not per process**: a second
  `container_lock()` in the same process conflicts, so the whole guard
  is unit-testable without forks.
- **Lock the identity, outside the deletable tree.** The first design
  instinct is a lock file inside the state dir; that re-opens the
  race the guard exists to close, because deletion changes the inode.
- **Probe-then-act is TOCTOU.** "Check the lock, release it, then
  delete" leaves a window; holding the lock across the action closes
  it for free.
- **An inherited fd is a lifetime mechanism.** The lock lives exactly
  as long as the VM with no bookkeeping, no pidfile, no signal
  handler -- the kernel enforces it.

#### Process

- "Correctness is premium" paid for itself immediately: this was the
  item that had survived four mission logs as "not yet guarded".

### What We Decided

- Lock files are never deleted (0-byte files accumulating per name
  ever used is the accepted cost; a future `podvm system` GC can sweep
  unreferenced ones).
- Fast-fail, not blocking wait: docker's semantics are "name in use",
  not queueing.
- `rm` of a name that never ran may leave its 0-byte lock file behind
  (harmless; noted, not fixed).
- `container_locked()` (non-creating probe) stays as the tested
  building block for a future `podvm ps`.

### Rules Added

- **Any resource shared by name gets a lock file outside the tree that
  is deleted, and the file is never deleted** -- the inode is the
  identity.
- **Hold the lock across the action, not across the check.**
  Probe-then-act is TOCTOU.
- **flock is per-OFD**: same-process second opens conflict; tests do
  not need forks to prove exclusion.

### Files Changed

| File | Purpose |
|------|---------|
| `src/container.c`/`.h` | `container_lock`, `container_locked`, shared `state_base()` |
| `src/main.c` | run-path lock before boot; `rm` locks across deletion |
| `tests/test_container.c` | Lock unit tests (free/held/conflict/EINVAL/location) |
| `tests/e2e_rm.sh` | Part 3: live-name guard with real VMs |
| `adrs/podvm_adr026_host-side-writeback.md`, `README.md` | "not yet guarded" replaced with the guard's design |

### Test Results

```
Container state               PASS   (lock cases added)
E2E rm (rm + --rm)            PASS   (part 3: run-vs-run + rm-vs-run)
...
PASS: 29   WARN: 1   FAIL: 0
```

### Next Steps

1. `podvm images`/`rmi`/cache GC for `/tmp/podvm/images` (the state-GC
   twin; the pattern is now proven twice).
2. `--hostname` (guest already honors `podvm.hostname=`).
3. ADR-023 cleanup: slirp constants hardcoded in `init.c` +
   `GUEST_IP_STR` in `main.c` -- a `podvm.net=ip:gw:dns` token before
   bridge networking lands.
4. Stale test/doc debt: `test_host_args.c` mocks a CLI that drifted
   (`-it`); `e2e_imgcfg.sh` header predates ADR-025.
5. Big rocks unchanged: layer compaction + `podvm build` (ADR-020
   revisit), the performance phase (ADR-019/009), `-d`/`stop`/`logs`,
   FreeBSD.

---

## Session 5: images/rmi, --hostname, build, and the Detached Lifecycle (2026-10-09)

Four work items in one sitting, one commit each (plus one correctness
commit that fell out of the design work):

1. **`podvm images` / `rmi` + `--hostname`** (`87a370a`). The cache
   (`/tmp/podvm/images`) got a listing, removal with docker
   semantics, and a shared/exclusive flock guard mirroring the
   container-name one -- a run serving a cached rootfs holds SHARED,
   `rmi` takes EXCLUSIVE, so a live container's image cannot be
   deleted under it. Lock files live in `<cache>-locks/`, never
   deleted (inode = identity). Cache layout flattened
   (`docker.io/library+alpine/latest`) because the nested repo path
   made a listing walk ambiguous -- "library" + tag "alpine" vs repo
   "library/alpine" + tag "latest" were structurally identical. Old
   cache entries invalidated (a /tmp cache re-pulls). `--hostname`
   needed only the host flag and the token; guest init already
   honored `podvm.hostname=`.
2. **Channel comma fix** (`f6879b2`), forced into the open by build's
   `ENV`: `podvm -e X=a,b` reached the guest as `X=a`. See "What We
   Learned" -- this one was worth its own commit.
3. **`podvm build`** (`e9f690a` parser, `e0703eb` driver). Dockerfile
   -> podvm-native image in the cache, runnable by tag. Each RUN is a
   real VM: chain = [committed layers, FROM...] as lowers with a
   fresh named-container upper; the upper IS the commit (rename into
   the build dir, same filesystem). Whiteouts travel as overlay's own
   char 0:0 devices between steps; final assembly is a host-side
   userns overlay + `cp --no-preserve=ownership` so the kernel does
   the merging. COPY/metadata are host-side; the final
   `.podvm-config.json` is the OCI shape the run path already reads.
   A failing RUN aborts the build with no image and no stray state.
   RUN scripts ride in a file in the step's upper
   (`/.podvm-step.sh`) -- no channel length/separator limits.
4. **`-d/--detach` + `stop` + `logs`** (`b25c2a3`). Daemon-less
   lifecycle: `-d` (requires `--name`; rejects `--rm`/`-i`) hands the
   VM over -- serial to `<state>/log`, pidfile written, QEMU holding
   the inherited locks. `stop` SIGTERMs the pidfile pid after
   verifying /proc cmdline is THIS container's QEMU, waits for the
   name lock (the ground truth for "the upper is free"), SIGKILLs
   after 10s. `logs` streams the serial file.

Suite after: **33 PASS / 1 WARN / 0 FAIL** (up from 29).

### What We Learned

#### Technical

- **A test assertion must never be satisfiable by the code under
  test's own output.** The old `-e CSV=a,b,c` e2e "passed" because
  `grep CSV=a,b,c` matched the host's own `cmdline:` echo -- the same
  line that showed the value being *broken*. The real bug (commas
  split as field separators) lived for a session behind a green
  check. After the fix, the cmdline echo shows escaped values, so the
  assertion is sound by construction.
- **Escape csv fields where they are BUILT.** The encoder split the
  raw list on every comma and then escaped the pieces -- by which
  point a data comma was indistinguishable from a separator. Escaping
  moved to the argv join, `-e`, the imgcfg fold, and the volume lists
  (the only sites that know), and `csv_encode` became a
  split-on-unescaped-comma pass-through. Wire format unchanged for
  plain values; the guest needed no change.
- **Guest exit codes never reached the host.** QEMU's status is
  meaningless for a guest panic, so `podvm run cmd` could not tell a
  script that cmd failed -- exposed by build's "failing RUN must fail
  the build" test. The guest now runs the entrypoint as a child,
  waits, and reports the code over vsock to a host helper process
  (cid host:5001 -> pipe). true->0, false->1, `sh -c "exit 42"`->42.
- **`/proc/<pid>/cmdline` is NUL-separated argv.** `strstr` over it
  only ever matches within the first argument; `stop`'s "is this our
  QEMU" check looked at argv[0] alone and every stop looked stale.
  Map NULs to spaces before scanning.
- **`config.args[256]` was a time bomb**: any real `sh -c` script
  overflowed it, and the old join even wrote out of bounds past the
  buffer. The new overflow guard turned silent corruption into
  truncation, which surfaced the limit; buffers are now 4096 host and
  guest, and overflow is a hard error.
- **`pkill -f <pattern>` kills the shell running it** when the pattern
  appears in your own command line (the `pgrep -f` lesson, now with a
  dead shell). Anchor on the binary path.
- **A pipeline's `$?` is the last command's** -- a `podvm stop ... |
  head` "success" was head's exit code, and a real failure read as a
  flake until a debug fprintf ended the speculation.
- **A missing installed initramfs looks like a guest bug**: QEMU exits
  with zero serial output. `start_qemu` now validates kernel+initramfs
  up front; `build` gained `-r`/`-k` pass-through.
- **The vsock CID is hardcoded to 3**, so only ONE vsock VM can exist
  at a time -- an orphaned QEMU from a debug session made the next
  VM's `vhost-vsock` attach fail. Newly visible now that background
  VMs exist.
- **cp -a chown on unmapped uids fails with EINVAL** (root-owned files
  are nobody inside the build userns); `--no-preserve=ownership`
  matches the pull path, which never had CAP_CHOWN either.

#### Process

- **The fork/wait exit-code change touched every run**, so the
  behavior-sensitive e2es (boot, interactive, net) were re-run before
  committing, not just the new one.
- Designing build's `ENV` is what surfaced the comma bug; designing
  its failure path is what surfaced the exit-code gap. Features that
  stress shared plumbing find its oldest wounds -- write the
  stress-ful test first.

### What We Decided

- Build output lives in the same cache as pulls, at the path a run
  would look up -- "if you can build it, you can run it by its tag."
  Untagged `-t` names land in the docker.io namespace (v1
  compromise; podman's `localhost/` could come later).
- v1 build scope: one stage, shell-form RUN, no ADD/VOLUME/ARG;
  EXPOSE/LABEL accepted and ignored; native image format only (no OCI
  blob export, so `podman load` compat is future work).
- Detached runs reject `--rm` (no supervisor to clean up) and `-i`
  (no owner for stdin); a >2KB config warns (kernel cmdline limit,
  no vsock channel).
- `stop` verifies identity via /proc cmdline before signalling, and
  treats the name lock -- not the pidfile -- as the truth about
  whether the upper is free.

### Rules Added

- **Assertions must not match the code under test's own output**;
  prefer markers whose on-wire form differs from any debug echo.
- **Escape delimiter-bearing fields where they are built**, never in
  the encoder -- the encoder cannot tell separators from data.
- **An exit code needs an explicit channel**; never assume a VMM's
  status reflects the guest command's.
- **`/proc/<pid>/cmdline` is NUL-separated**: map to spaces before
  substring matching.
- **Anchor `pkill -f`/`pgrep -f`** on the binary path (self-match is
  fatal).
- **Read the status you mean**: a pipeline reports its last command.

### Files Changed

| File | Purpose |
|------|---------|
| `src/image.c`/`.h` | Cache list/path/lock; repo flattening (`+`) |
| `src/build.c`/`.h` | Dockerfile parser, COPY, env-set |
| `src/main.c` | `images`/`rmi`/`build`/`stop`/`logs`, `--hostname`, `-d`, exit helper, csv_encode contract, early kernel/initramfs validation |
| `src/container.c`/`.h` | `container_build_dir`, `container_lock_unlink` |
| `guest/init.c` | fork/wait entrypoint + vsock exit report; bigger arg/env buffers |
| `src/imgcfg.c` | Escaping at fold time; escape-aware user-args split |
| `tests/test_build.c`, `tests/test_imgcfg.c` | Parser + escaping cases |
| `tests/e2e_images.sh`, `e2e_build.sh`, `e2e_detach.sh` | New e2es (3 VM-heavy suites) |
| `tests/run_tests.sh`, `tests/Makefile`, `CMakeLists.txt`, `README.md` | Wiring + docs |

### Test Results

```
Build parser                  PASS   (new)
E2E run flags (podman parity) PASS   (+ --hostname)
E2E build (Dockerfile)        PASS   (new)
E2E detach (-d/stop/logs)     PASS   (new)
E2E images/rmi (cache)        PASS   (new)
...
PASS: 33   WARN: 1   FAIL: 0
```

### Next Steps

1. **Unique vsock CIDs per VM** (hardcoded 3 = one vsock VM at a
   time; an orphan blocks the next boot). Now the top correctness
   item.
2. **OCI blob export from build** (`podman load` compat) and a build
   cache (reuse committed layers across builds; multi-stage).
3. ADR-023 cleanup: per-container slirp constants via a token.
4. Stale debt: `test_host_args.c` mocks a drifted CLI;
   `e2e_imgcfg.sh` header predates ADR-025.
5. Detach polish: `logs -f`/`--tail`, restart policies.
6. Big rocks: layer compaction, the performance phase (ADR-019/009),
   FreeBSD (ADR-021).

---
