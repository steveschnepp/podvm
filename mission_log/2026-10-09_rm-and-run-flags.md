---
name: rm-and-run-flags
description: podvm rm + --rm close the write-back lifecycle, then tier-1 (-e/-u/-w) and tier-2 (-v/-p/--network/--read-only) podman flag parity -- three sessions, one log
project: podvm
---

- Previous: `2026-10-09_config-channel-and-runtime-gaps.md`

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
