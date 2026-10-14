---
name: initramfs-auto-resolve
description: -r out/initramfs.cpio was tribal knowledge for every uninstalled dev run -- initramfs now auto-resolves (next-to-binary, then installed), make initramfs lands it in build/, MAX_PATH -> PATH_MAX everywhere
project: podvm
previous: 2026-10-14_run-keyword-compat.md
---

# Mission Log: Initramfs Auto-Resolve + PATH_MAX

---

## What We Did

Fixing `podvm run --rm hello-world` (previous log) revealed a second
compat gap: after the `run`-keyword fix, the command STILL needed
`-r out/initramfs.cpio`, because the compiled-in default
`/var/lib/podvm/kernel/initramfs.cpio` only exists after
`sudo make install` -- and every one of the ~20 e2e invocations passes
`-r` explicitly, so the suite can never catch the uninstalled case.

### Initramfs resolution (user's design, refined in discussion)

The user first considered a non-root XDG-ish location, then landed the
better framing: `/var/lib/podvm/kernel` is *software payload* of a
system-wide install -- perfectly right when installed. The dev tree
just needs to carry its own artifact. Final precedence:

1. `-r` (explicit always wins)
2. **next to the running binary** -- `dirname(/proc/self/exe)`,
   so the dev tree's `build/podvm` finds `build/initramfs.cpio`;
   "the binary you run owns the initramfs you get"
3. `/var/lib/podvm/kernel/initramfs.cpio` (installed copy)

Implementation: `config.initramfs` defaults to `""`; new
`resolve_initramfs()` fills it at QEMU-launch time (the single choke
point both `run` and `build`'s RUN-step VMs go through). The
not-found error now lists all three remedies (`make initramfs`,
`make install`, `-r`). `make initramfs` also copies
`guest/initramfs.cpio` into `build/`, so the dev tree is
self-contained -- no sudo in the dev loop.

Verified: `build/podvm run --rm hello-world` (the user's original
zero-flag command) resolves `build/initramfs.cpio`, boots, prints
the docker banner, rc=0. `-r` precedence checked; a binary copied to
`/tmp` (next-to-binary misses, /var/lib absent) produces the new
error message.

### MAX_PATH -> PATH_MAX (user's call)

While there, the user flagged the project-local `#define MAX_PATH
1024` (in `src/main.c` and `src/image.h`) as shadowing the standard:
use `<limits.h>`'s `PATH_MAX` everywhere. Renamed all 50 occurrences
across `src/main.c`, `src/image.h`, `src/imgcfg.h`, `src/imgcfg.c`;
deleted both local defines; added `#include <limits.h>` to `image.h`
(the shared header) and `main.c`. Buffers grow 1024 -> 4096 (glibc),
strictly safer for long paths.

## What We Learned

### Technical

- **A hermetic test suite has a blind spot exactly where users feel
  most pain.** Every e2e passes `-r "$WORK/initramfs.cpio"` -- correct
  for isolation, but it means the default-resolution path (the one a
  new user hits first) is never executed by CI. The zero-flag smoke
  test must be run by a human (or a dedicated "installed-layout" test)
  now and then.
- **Bumping a path constant fires a wave of `-Wformat-truncation`
  warnings** (23 -> 35): gcc's heuristic re-computes src/dst size
  ratios for every snprintf into a PATH_MAX buffer. All pre-existing
  class, none new code. Worth remembering so the delta doesn't scare
  the next bisect.
- `/proc/self/exe` is the reliable way to find "next to the binary" --
  argv[0] lies (PATH lookups, symlinks, exec from another cwd).

### Process

- The user's instinct chain here was textbook: "why -r?" -> "use a
  non-root location" -> "oh, /var/lib IS right for installed" ->
  "then dev just needs it in build/". Refining the framing beat
  accepting the first plausible option.

## What We Decided

- Initramfs precedence: `-r` > next-to-binary > `/var/lib/podvm`.
- `make initramfs` copies the artifact into `build/` (dev tree
  self-contained; `make clean` already removes build/ and
  guest/initramfs.cpio).
- `PATH_MAX` from `<limits.h>` replaces the project-local `MAX_PATH`
  everywhere; no local shadow define remains.

## Rules Added

- **No project-local defines of standard `<limits.h>` names.**
  `PATH_MAX` (and kin) come from the header; a local 1024-sized
  shadow silently disagrees with the libc truth.

## What We'd Do Differently

- Land the zero-flag smoke test (`build/podvm run --rm hello-world`,
  no `-r`, no env) as an explicit -- even if manual -- checklist item
  on any CLI/session change. Two compat bugs in one user command were
  found only because the user ran it verbatim.

## Files Changed

| File | Purpose |
|------|---------|
| `src/main.c` | `resolve_initramfs()` (next-to-binary, then installed); empty default; error text; usage; MAX_PATH -> PATH_MAX |
| `src/image.h` | Drop local MAX_PATH; `#include <limits.h>`; PATH_MAX |
| `src/imgcfg.h`, `src/imgcfg.c` | PATH_MAX rename |
| `Makefile` | `make initramfs` copies artifact to `build/initramfs.cpio` |

## Test Results

```
$ ./build/podvm run --rm hello-world      # no -r, no env
podvm:   initramfs: /home/snide/podvm/build/initramfs.cpio
... Hello from Docker! ...                rc=0

PASS: 35   WARN: 0   FAIL: 0
```

## Next Steps

- ADR-028 guest-side overlay still unimplemented (chown/USER
  ownership; acceptance: mermaid-ascii Dockerfile unmodified).
- Possible: an "installed-layout" test that runs the binary from a
  staged prefix (binary + initramfs side by side) to keep
  `resolve_initramfs` honest in CI.
