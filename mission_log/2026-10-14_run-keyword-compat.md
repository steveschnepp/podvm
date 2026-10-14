---
name: run-keyword-compat
description: `podvm run --rm hello-world` tried to pull an image named "run" -- the explicit run keyword (docker/podman drop-in compat) was never consumed; fixed, tested, logged
project: podvm
---

# Mission Log: The `run` Keyword Compat Fix

---

## What We Did

The stated next step from the previous session was the user's smoke
test: `build/podvm run --rm hello-world` "doesn't work". Reproduced
on the first try:

```
Error: short-name "run" did not resolve to an alias ...
podvm: manifest fetch failed for https://registry-1.docker.io/v2/library/run/manifests/latest
```

It tried to pull an image literally named `run`. Diagnosis in
`src/main.c` `main()`: podvm's run path is the *default* command (no
subcommand), and getopt_long runs with `"+"` (stop at the first
non-option, so entrypoint args are not stolen). `run` at argv[1] is a
non-option -- getopt stopped immediately, `optind==1`, and `run`
became the image positional, with `--rm hello-world` passed through
as the container's command.

Fix (drop-in compat is the project's defining spec, so match docker's
precedence -- the keyword wins over any image named `run`):

- `main()`: before option parsing, if argv[1] is `run`, shift it away
  (`podvm run FLAGS IMG CMD` now parses identically to
  `podvm FLAGS IMG CMD`).
- `usage()`: documents the `run` form.

Verified end to end with the exact failing invocation:

```
$ ./build/podvm run --rm -r out/initramfs.cpio hello-world
... Hello from Docker! ...        rc=0
```

(The intermediate failure -- `initramfs not found` at the default
`/var/lib/podvm/kernel/` path -- is the known fixture requirement,
not a bug: pass `-r out/initramfs.cpio` or install it. Same trap the
layered-store log already documented.)

Regression coverage: `tests/e2e_runflags.sh` gained a `run`-keyword
section -- invokes `podvm run -r ... -e PODVM_E2E_KW=yes IMG /bin/sh
-c ...` and asserts the flag plumbing behaves exactly as in the
keyword-less form.

## What We Learned

### Technical

- **The `"+"` in getopt_long is load-bearing twice.** It protects
  entrypoint args from being parsed as podvm flags, but it also means
  ANY leading non-option (including a docker keyword) terminates
  option parsing -- so keywords must be consumed before getopt runs,
  not after.
- **podvm's flag grammar is already docker-shaped** (flags before the
  image, everything after the image is the container command); only
  the keyword was missing. The fix was three lines, not a rework.

### Process

- The user's one-line repro ("run --rm hello-world doesn't work")
  localized to arg parsing on the first invocation -- a cheap repro is
  worth a design doc.

## What We Decided

- **The explicit `run` keyword is accepted and consumed, keyword-wins
  (docker semantics).** An image literally named `run` needs
  `docker.io/library/run` or a path prefix -- identical to docker's
  own resolution, which is the point of drop-in compat.

## What We'd Do Differently

- The smoke test (`podvm run --rm hello-world`) would have been run
  the moment the `run`-flags session landed -- `--rm` shipped in the
  10-09 session and nobody spelled it docker-style until the user did
  two days later. A compat project should keep a tiny "canonical
  docker invocations" checklist (run/pull/build/save/load in docker
  spelling) exercised by the suite, not just flag-by-flag parity.

## Rules Added

- **Drop-in CLI compat is the tiebreaker.** When podvm's CLI grammar
  and docker/podman's disagree, docker's behavior wins (here: keyword
  precedence over image-name resolution).

## Files Changed

| File | Purpose |
|------|---------|
| `src/main.c` | Consume the `run` keyword before getopt; usage line |
| `tests/e2e_runflags.sh` | New: `run`-keyword invocation parses like the default form |
| `mission_log/2026-10-14_run-keyword-compat.md` | This log |

## Test Results

```
E2E run flags (podman parity)  PASS   (new run-keyword section)
...
PASS: 35   WARN: 0   FAIL: 0
```

## Next Steps

- ADR-028 (guest-side overlay) still unimplemented -- fixes cross-uid
  chown; acceptance test is the mermaid-ascii Dockerfile unmodified.
- Same compat lens on other subcommands: any docker keyword podvm
  doesn't have (e.g. `exec`, `cp`, `ps`) currently falls into the
  default run path and produces a confusing "pull <keyword>" error;
  deciding per keyword (implement vs. explicit "unsupported" error)
  is open.
