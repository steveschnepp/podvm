---
name: adr028-acceptance-and-delete-regression
description: ADR-028 acceptance passes (chown without || true, USER app writes to /app); found and fixed the regression I introduced -- subuid-owned store trees broke rm -rf, so rmi/prune/rm/traps all needed a subuid-mapped delete
project: podvm
---

# Mission Log: ADR-028 Acceptance + the Delete Regression

---

## The Ask

The user approved the two remaining follow-ups after the store
round-trip, on the standing instruction "correctness matters." Those
were: (1) prove ADR-028's real acceptance -- the mermaid-ascii Dockerfile
shape that broke on 10-12 now works unmodified -- and (2) close the
save/load-ownership corner.

What this actually turned into: writing the acceptance test surfaced a
**regression I had introduced** in the previous session's ownership fix,
which I then had to fix properly before the acceptance could pass.

---

## What We Did

### 1. The regression, found by writing a test

The ownership round-trip made store/state trees hold **subuid-owned**
files. That broke `rm -rf`: unlink needs write on the *parent*, and a
subuid-owned 0755 directory grants the invoking user none. So every
recursive delete of a store/state tree failed -- `rmi`, `system prune`,
`--rm`, build cleanup, and the e2e traps.

This is the exact "host fs represents container uids" cost ADR-028
flagged when it chose path B. I had accepted it in the decision but not
*handled* it. It only surfaced now because before the round-trip, ingest
discarded ownership so store files were user-owned and removable.

Diagnosed empirically (the session's standing discipline): built a
subuid-owned tree in a range-ns, then `rm -rf` as the user -- confirmed
`Permission denied` on the parent-dir unlink. Verified the fix direction
the same way: a delete inside a subuid-mapped userns removes it cleanly.

### 2. subuid_rm_rf: the one correct delete

`subuid_rm_rf()` in subuid.c runs the delete **inside a subuid-mapped
userns**, where the process is ns-root over the range and so owns every
subuid-owned file -- the same thing rootless podman does. A plain fast
path handles user-owned trees (save/load staging, build temp) with no
fork, escalating to the userns only on the first EPERM. The chmod
pre-pass moved here too (overlayfs leaves its work dir mode 000, and
chmod needs ownership).

Three delete implementations collapsed into it: `container_rm_rf`,
`store_native`'s `rm_rf`, and `build.c`'s `rm_rf`. The nftw callbacks
that lived in container.c moved to subuid.c with it.

### 3. The e2e traps are shell -- they cannot call C

The 8 image-building e2e tests trap on `rm -rf "$WORK"`, which is the
*shell's* rm, not podvm's. So even with the C fix, the traps left
subuid-owned litter in /tmp. Solution: `tests/rf.c` -- the same
`subuid_rm_rf` exposed as a binary. Traps became
`"$SCRIPT_DIR/rf" "$WORK" 2>/dev/null || rm -rf "$WORK"` -- subuid-aware,
with a plain-rm fallback if rf is absent.

### 4. ADR-028 acceptance, written and passing

`e2e_adr028.sh`: the exact shape that broke on 10-12, with **no
`chown || true` workaround**. It must (a) build -- the chown step
completing is itself proof cross-uid chown no longer aborts -- and (b)
run as `USER app` (uid 1000), which must write to the chowned `/app`.

Two false starts, both my bugs, both caught by running it:
- **Base-image mismatch:** the test's `grep "alpine|debian"` picked
  `debian:bookworm-slim`, but the Dockerfile uses alpine/busybox user
  tools (`adduser -D`), which debian rejects. My manual probe had
  hardcoded alpine so it passed; the test needed to *require* alpine.
- **Heredoc escaping:** `UID=\\$(id -u)` in an unquoted heredoc yielded a
  literal `\$(id -u)` in the Dockerfile, so the guest printed it verbatim
  instead of the uid. Single backslash is correct; the manual probe used
  a quoted heredoc and so never hit it.

Fixed both; the test now passes and leaves zero litter.

### 5. A build-wiring miss (again)

Delegating the three delete paths to `subuid_rm_rf` meant `container.c`
and `build.c` gained a subuid.c dependency -- but the test targets that
link them did not. `make all` halted at `test_container` (undefined
`subuid_rm_rf`), so every later binary was missing and 9 unit tests
failed from a single link error. Added subuid.c to
`test_container`/`test_cli`/`test_build`. Third time this class of bug
appeared this session; the Phase-2 lesson keeps earning its keep.

### Result

```
E2E-ADR028 PASS: cross-uid chown built without || true, USER app (1000) wrote to /app

PASS: 41   WARN: 0   FAIL: 0
```

All four store-lifecycle e2e (`images`, `saveload`, `rm`, `writeback`)
pass -- the product delete paths handle subuid trees. Traps leave zero
litter.

Commits:
- `00af7e8` fix: musl guest build finds UAPI headers in the multiarch dir
- `3043b8e` fix: deleting subuid-owned trees works (rmi/prune/rm/build-cleanup)

(The musl multiarch fix rode along in the same working tree; it is a
distinct concern so it got its own commit. See the note below on the
botched first commit.)

---

## What We Learned

### Technical

- **A fix that makes files "correctly owned" can break the tools that
  manage them.** Ownership is not just metadata to display -- it decides
  who may unlink. Subuid-owned dirs are invisible to `rm`, and the blast
  radius is every recursive delete in the codebase. When changing what a
  tree *owns*, audit what *deletes* it.
- **The userns delete is the only correct one, and it is cheap enough:**
  fast path first (no fork for user-owned trees), escalate on EPERM. The
  common case pays nothing.
- **Test traps are shell, not C** -- a product fix does not automatically
  fix the harness. Exposing the product helper as a tiny binary (`rf`) is
  better than duplicating the userns dance in shell.
- **Manual probes and automated tests must exercise the same code.** Both
  false starts (base image, heredoc escaping) were cases where my manual
  probe hardcoded what the test parameterized, so the probe passed and
  the test failed. When a probe works but the test does not, diff them.

### Process

- **Write the acceptance test even (especially) when you think the
  feature works.** It is what surfaced the delete regression -- a bug I
  had introduced and would otherwise have shipped.
- **A `git add <one file> && git commit` after a broad earlier `git add`
  sweeps in everything staged.** I accidentally committed the whole
  subuid-delete change under a "musl fix" message. Caught it by reading
  `git show --stat` and asking why HEAD contained `subuid_rm_rf`; fixed
  with a `--soft` reset and two clean commits. The lesson: commit the
  narrow thing *before* staging the broad thing, or check `--stat` right
  after.
- **Backticks in a `git commit -m "..."` message trigger bash command
  substitution** -- `|| true` inside backticks broke the commit
  invocation. Use a quoted heredoc (`git commit -F - <<'MSG'`) for any
  message with shell-significant characters.

---

## What We Decided

- **All recursive deletes of store/state/staging trees go through
  `subuid_rm_rf`.** No path keeps its own nftw/readdir delete -- that is
  how the three implementations collapsed into one.
- **e2e traps use `tests/rf` with a plain-`rm` fallback**, so a missing
  helper degrades rather than fails the trap.
- **ADR-028 is now accepted:** the shape that forced it (cross-uid chown
  before `USER`) builds and runs unmodified. The `|| true` workaround in
  `out/mermaid-ascii/Dockerfile` is no longer needed.

---

## Rules Added

- **When a change alters what files a tree owns, audit every path that
  deletes that tree.** Ownership changes ripple into delete permissions.
- **Product fixes do not fix the test harness.** If a test's trap/teardown
  does the same operation the fix covered, expose the helper to it.
- **After any `git commit`, glance at `git show --stat`** -- especially if
  you staged more broadly earlier in the session.

---

## What We'd Do Differently

- The delete regression was *predictable*: the previous session's log
  even flagged "host filesystems hold subuid-owned files" as a cost. I
  should have handled `rm` at that moment rather than a session later
  when a test caught it. When a decision has a named downside, fix the
  downside in the same change, not later.
- Both e2e false starts shared a root cause: probe and test diverged.
  Next time, write the test *first* and let the manual probe be the test
  run -- one code path, no divergence.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/subuid.c`, `src/subuid.h` | `subuid_rm_rf()` -- subuid-aware recursive delete (fast path + userns escalation) |
| `src/container.c` | `container_rm_rf` delegates to `subuid_rm_rf`; its nftw callbacks moved out |
| `src/store_native.c` | local `rm_rf` delegates (covers staging + GC'd layer diffs) |
| `src/build.c` | local `rm_rf` delegates (build staging is subuid-written) |
| `tests/rf.c` (new) | the helper as a binary, for e2e traps |
| `tests/e2e_adr028.sh` (new) | ADR-028 acceptance: chown with no workaround + USER app writes |
| `tests/Makefile` | build `rf`; link subuid.c into test_container/test_cli/test_build |
| 8 `tests/e2e_*.sh` | traps use `rf` with a plain-rm fallback |
| `.gitignore` | ignore the `rf` binary |
| `guest/Makefile` | (separate commit) musl multiarch -idirafter fix |

## Test Results

```
E2E-ADR028 PASS: cross-uid chown built without || true, USER app (1000) wrote to /app
...
PASS: 41   WARN: 0   FAIL: 0
```

Store-lifecycle e2e (`images`, `saveload`, `rm`, `writeback`) all pass;
e2e traps leave zero /tmp litter.

---

## Next Steps

- ADR-028 is accepted end to end. The `|| true` workaround in
  `out/mermaid-ascii/Dockerfile` can be dropped; a full mermaid-ascii
  build (Go, multi-stage, network) is the real-world dogfood but needs
  `-m 3072 -c 4` and network, so it stays a manual exercise.
- The subuid allocation is read fresh on every `subuid_rm_rf`/translator
  call (one /etc/subuid parse each). Fine at current call volume; if a
  hot path ever appears, cache it.
- Ownership now survives ingest/run/commit/save/load AND cleanup works.
  The remaining untested corner is podman-authored non-trivial ownership
  through `podvm load` -- the path is right but no e2e drives it.
