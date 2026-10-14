---
name: warning-root-cause-sweep
description: CI build broke on an old zstd (transitive-include assumption); fixing it led to a full sweep of all 39 -Wall/-Wextra warnings, each root-caused (real truncation bugs) rather than silenced
project: podvm
previous: 2026-10-14_cli-parser-extraction.md
---

# Mission Log: The Warning Root-Cause Sweep

---

## The Ask

Two things landed in one session:

1. A pasted CI build failure (GitHub Actions, `/home/runner/work/podvm/podvm`)
   -- `ZSTD_getErrorCode` implicitly declared, `ZSTD_error_dstSize_tooSmall`
   undeclared in `src/image.c:128`.
2. After that was diagnosed, the user: "let's fix all those warnings. those
   are usually valid and indicate looming bugs -- and fix the root cause, not
   silence the warning."

That second instruction is the spine of this log. It ruled out `-w`,
`-Wno-format-truncation`, casting returns to `(void)`, and "just enlarge every
buffer." Every one of the 39 warnings had to be traced to a real defect.

---

## What We Did

### 1. The CI build break (item 1)

`ZSTD_getErrorCode()` and the `ZSTD_error_*` enum live in **`zstd_errors.h`**,
not `zstd.h`. It compiled locally only because the dev box's zstd (1.5.7) has
`#include "zstd_errors.h"` *inside* `zstd.h` (line 18). The CI runner's older
zstd (Ubuntu 22.04 / 1.4.x) does not. The transitive include was the bug, not
the code.

Fix: an explicit `#include <zstd_errors.h>` in `src/image.c`. `zstd_errors.h`
has shipped since ~1.3.4, so it is safe on every supported version. One line.
Committed separately (`f3ab760`) so the CI unblock is cherry-pickable.

### 2. The warning inventory

A clean rebuild (`rm -rf build && cmake -DCMAKE_BUILD_TYPE=Debug && make -j4`)
produced **39 warnings** under the project's own `-Wall -Wextra -O2`
(`CMakeLists.txt:8`):

```
33  -Wformat-truncation
 5  -Wrestrict
 1  -Wformat-zero-length
```

(The CI log's two `-Wunused-result` on `fscanf`/`read` did NOT reproduce locally
-- that build predates the cli.c extraction; the current tree does not warn on
those. Left alone.)

### 3. The mechanism that made this tractable

The key gcc behavior, verified with a standalone repro before touching code:

> `-Wformat-truncation` (level 1, on by `-Wall`) fires **only when snprintf's
> return value is discarded**. Check the return -- and gcc stops warning.

That is not a silencing trick: checking the return *is* the root-cause fix,
because it forces you to decide what truncation means at each site. The 5
`-Wrestrict` warnings needed a different answer (see below).

### 4. File-by-file, each root-caused

**`main.c` -- `build_config_tokens()` / `build_cmdline()` (the load-bearing
one).** This assembles the `podvm.args=... podvm.env=...` token string the
*guest parses*, from `entry`+`args`+`env`+`cwd`+`user` that can total ~24KB
into a 4096 buffer. Truncation here means the guest reads `podvm.args` as
`podvm.env` -- a real correctness bug, not a cosmetic one. Rather than enlarge
buffers (which only postpones the limit), converted both to return
`int` (0 / -1-truncated), added a `tok_append()` varargs helper that tracks
`used` and sets a `trunc` flag, and made truncation **fail hard**. Propagated
to all 5 call sites:
- `start_qemu()` and the `--detach` path **abort** (cmdline is the only
  channel when detached; no fallback).
- the two vsock senders **fall back to the cmdline** with a warning (that is
  already the documented fallback).
- `cmd_build`'s step-VM vsock send warns and uses the cmdline.

**`main.c` -- `envraw`/`env` sizing.** `envraw` was `MAX_CMDLINE` (4096) but
had to hold `config.env` (4096) *plus* the `,HOME=/root` suffix -- so the HOME
append silently truncated the tail. Sized both to `sizeof(config.env) + 16`.
Real wire overflow is still caught by `tok_append`.

**`imgcfg.h` / `imgcfg.c` -- the 16K-into-4K copy-back.** `RunSpec.args/env`
were `IMGCFG_ARGS_MAX/ENV_MAX = PATH_MAX*4` (16384), but the destination
`config.args/env` (cli.h) are 4096. So `imgcfg_fold` produced up to 16KB that
was **silently truncated at the copy-back in `apply_image_config()`**. The
wire (`MAX_CMDLINE`) is 4096 anyway, so the 16K was dead headroom. Sized both
to 4096 (with a comment explaining the invariant), and rewrote `PUSH_TO_ARG`
to bounds-check *before* writing (via an `args_full` flag) and drop a whole
trailing field -- so an over-long command is dropped whole with a warning,
never emitted as a comma-corrupt partial field. (The first attempt still
warned because gcc cannot correlate a runtime `_avail` with the snprintf
bound; switching to explicit `memcpy` of the pre-measured length cleared it.)

**Path joins (5 files).** The pervasive pattern: `snprintf(dst, n, "%s/%s",
dir, entry)` with the return discarded. A truncated path is a *wrong* path.
Two categories:
- **build/run paths -> error out.** `path_append()` helper (main.c) for the
  overlay `/u`,`/w` dirs; explicit checks on the overlay `opts` string,
  `resolve_initramfs()` candidate, `reg_parse()` `library/` prefix,
  `container.c` lock paths, `store.c` layer diff paths, the COPY dst, the
  `c%03d` layer dir, the `.podvm-step.sh` path, the QEMU `-fsdev` volume
  args.
- **directory walks -> skip the entry.** `store_native.c` gained
  `join_entry()`; its 7 sites `continue` on overflow, because `rm_rf()` /
  `unlink()` / `stat()` on a truncated path would touch an innocent file.
  `tar.c`'s `extract()` (security-sensitive: never extract to a wrong path)
  and `pack_walk()` likewise skip the entry, matching the existing
  `safe_path()` failure convention.

**`-Wrestrict` (5, main.c `cmd_build`).** `snprintf(st->envlist, ...,
src->envlist)` where `st` = `&stages[nstages]` and `src` =
`&stages[src_stage<nstages]` -- provably distinct elements, but both index
the same `stages` array so gcc cannot prove non-aliasing. These are identical
fixed-size fields, so a full `memcpy` is the exact copy (no truncation
possible) and aliasing-safe by construction. Same treatment for two
`resolve_image_chain` field copies (`lay[l].diff`, `nl[l].digest`) where gcc
lost the bound through the `store_chain()` call and estimated 65535/135679
bytes into a 4096/72 buffer.

**`-Wformat-zero-length` (1).** `cli.c` used `snprintf(esc, ..., "")` as a
kv_escape fallback; the other 6 sites use `esc[0] = '\0'`. Unified on the
latter.

### Result

```
$ rm -rf build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j4
...
warning count: 0          (was 39)

$ bash tests/run_tests.sh
PASS: 37   WARN: 0   FAIL: 0
All tests passed!
```

Two commits:
- `f3ab760` fix: include zstd_errors.h explicitly for the CI toolchain
- `e854bc6` fix: eliminate all format-truncation/restrict warnings at the root

---

## What We Learned

### Technical

- **gcc's `-Wformat-truncation` keys off the *unused return value*.** Check
  snprintf's return and it stops warning. This makes "check + handle" the
  only fix that both satisfies gcc and is honest -- there is no cheap way to
  silence it that isn't also a real fix. (Level-1 warnings ignore return
  checks; that is the whole lever.)
- **A transitive `#include` is a portability bug waiting for an older
  toolchain.** `zstd.h` pulling `zstd_errors.h` is a 1.5.x convenience, not a
  contract. The CI runner (1.4.x) proved it. Include what you use.
- **Buffer-size mismatches across a copy-back are silent truncation.**
  `RunSpec` at 16K feeding `config` at 4K looked like generosity; it was a
  place where up to 12KB of the user's command could vanish without a word.
  When two structs hold the *same* logical value, their buffers must agree.
- **gcc's truncation byte estimates are sometimes fiction** (65535, 135679
  bytes "into a region of size 72"). That signals gcc *lost the bound* --
  usually through a call boundary (`store_chain()`) or an extern array
  (`volumes[]`). The fix is to make the bound explicit (memcpy of a known
  field, or an explicit check), not to assume the warning is spurious.
  Some of these *were* false alarms (the QEMU `-fsdev` args fit), but the
  check is still right -- it protects against a future larger `host[]`.
- **`-Wrestrict` on `snprintf(dst, ..., src)` means "prove `dst != src`."**
  When two pointers index the same array at provably different indices but
  gcc can't see it, `memcpy` of a same-or-smaller fixed field is the correct
  equivalent, not a cast.

### Process

- **Verify the fix mechanism in a standalone repro before applying it 39
  times.** The 20-line `wt.c` test (return-checked vs return-discarded
  snprintf) settled the strategy in one command and avoided guessing.
- **The user's "fix the root cause, not silence" instruction is what made
  this valuable.** Every silenced warning here would have hidden a real bug:
  a misparsed guest config, a wrong overlay mount, a wrong `rm_rf` target, a
  truncated COPY destination. The instruction converted a chore into four
  genuine bug fixes.
- **Clean rebuilds are mandatory when counting warnings.** An incremental
  build reported "0 warnings" multiple times simply because the edited
  translation units hadn't recompiled; only `rm -rf build` gave the true
  count. Nearly fooled twice.

---

## What We Decided

- **Truncation of the guest config token string is a hard error**, not a
  warning and not a buffer enlargement. The guest cannot run correctly on a
  partial config.
- **Directory-walk path joins skip the entry on overflow; build/run path
  joins fail the operation.** The distinction is whether a wrong path is
  recoverable (skip) or corrupts state (fail).
- **`RunSpec.args/env` are 4096**, matching `config.args/env` and
  `MAX_CMDLINE`. The 16K sizing was removed as dead headroom that hid a
  truncation.
- **`memcpy` for fixed-size field copies** where the source provably fits;
  **explicit return checks** where it might not.

---

## Rules Added

- **No reliance on transitive includes.** Include the header that declares
  the symbol you call, even if another header currently pulls it in. The CI
  toolchain is usually older than the dev box.
- **Two structs holding the same logical value must use the same buffer
  size.** A larger producer feeding a smaller consumer is a silent
  truncation bug, not generosity.
- **When two buffers must agree on a size, name the invariant in a comment
  at the definition** (as done for `IMGCFG_ARGS_MAX`) so the next edit
  doesn't unbalance them.
- **A clean rebuild (`rm -rf build`) before claiming a warning/build count.**
  Incremental builds hide both.

---

## What We'd Do Differently

- The `IMGCFG_ARGS_MAX = PATH_MAX*4` mismatch should have been caught when
  `apply_image_config()` first copied `out.args` (16K) into `config.args`
  (4K). A compile-time `_Static_assert(sizeof out.args == sizeof
  config.args)` would have flagged it at birth. Worth adding for the other
  cross-struct string copies.
- I verified the gcc return-check mechanic *after* fixing the first two
  sites by inspection. Doing the repro first (as I eventually did) would
  have set the strategy before touching any file. Do the repro first, always.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/image.c` | explicit `zstd_errors.h` include; `reg_parse` rejects over-long `library/` prefix |
| `src/main.c` | `tok_append` + truncation-failing `build_config_tokens`/`build_cmdline`; `envraw`/`env` sizing; `path_append`; overlay opts/initramfs/COPY/ldir/spath/QEMU-vol checks; `memcpy` field copies (also kills 5 `-Wrestrict`) |
| `src/imgcfg.h` | `IMGCFG_ARGS_MAX/ENV_MAX` 16K -> 4096 (match `config.*`, the real wire limit) |
| `src/imgcfg.c` | `PUSH_TO_ARG` bounds-checks before write; `args_full` drops a whole field; warn on drop |
| `src/cli.c` | `esc[0] = '\0'` instead of zero-length snprintf |
| `src/store.c` | layer diff path join checks truncation, returns -1 |
| `src/store_native.c` | new `join_entry()`; 7 walk sites skip on overflow; commit-tar path check |
| `src/container.c` | lock path + `/locks` dir joins return `ENAMETOOLONG` on overflow |
| `src/tar.c` | `extract()`/`pack_walk()` skip an entry whose path would truncate |

## Test Results

```
Build (clean, -Wall -Wextra -O2):  0 warnings   (was 39)
PASS: 37   WARN: 0   FAIL: 0
```

Binary behavior spot-checked implicitly by the suite (all e2e still green,
including the cmdline-token assertions in `e2e_runflags.sh` and
`e2e_imgcfg.sh`, which is what guards the `build_config_tokens` refactor).

---

## Next Steps

- Continue the queued items: ADR-028 guest-side overlay, the zero-flag
  smoke-test checklist item, and the `cmd_build` flag-loop extraction.
- Consider `_Static_assert` on the other cross-struct string-copy size
  invariants (`config.*` <-> `RunSpec`, `StoreLayer.diff` <-> `NativeLayer.diff`).
- A CI matrix entry that builds against an *older* zstd (1.4.x) would have
  caught the transitive-include bug locally; the single-line include now
  guards it, but the matrix makes the class of bug visible.
