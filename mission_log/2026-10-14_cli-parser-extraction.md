---
name: cli-parser-extraction
description: extract the run-CLI getopt parser into src/cli.{c,h} and pin the docker compat grammar with a C unit test (test_cli) -- plus pflag --bool=VALUE parity and honest unknown-option errors
project: podvm
previous: 2026-10-14_initramfs-auto-resolve.md
---

# Mission Log: The CLI Parser Extraction

---

## The Ask

The user: "no need to test it e2e, but unit test the cli parsing" --
and, in the same breath, "is there a read-to-use simple lib to parse
those?"

## The Lib Question (answered before coding)

No usable one, and not worth it:

| Candidate | Why not |
|-----------|---------|
| getopt_long (status quo) | Already docker-shaped: `--opt=value`, attached shorts (`-eK=V`, `-m512`), grouping (`-im 512`), `--`, and the `+` prefix = docker's stop-at-image rule. Zero deps. |
| argp (glibc) | GNU help/semantics baked in; `ARGP_IN_ORDER` can mimic docker but you fight the conventions. |
| argtable3 (vendored) | Generates usage; its positional model still can't express "everything after IMAGE is the container's command". Buys aesthetics, not semantics. |
| cxxopts / CLI11 | C++; podvm is C. |
| pflag (Go) | Literally docker's parser -- unusable from C. |

The docker-specific rules are ~10 lines *around* getopt. Kept
getopt_long; the unit test pins it as the spec.

## What We Did

### 1. Extraction (the actual work)

The parser was static state + a getopt loop inside `main()`:
`config`, the `-p/-v/--tmpfs` lists, `publish_add`, `volume_add`,
`tmpfs_add`, `env_add`, `env_add_file`, `usage()`, the `OPT_*` enum,
and the loop itself. Moved verbatim into:

- `src/cli.h` -- `PodvmConfig`/`PodvmPublish`/`PodvmVolume` types,
  extern state, `cli_parse_run()` returning `CLI_PARSE_{OK,HELP,ERROR}`,
  `cli_image_ref()` (the typed ref, kept because `config.rootfs_dir`
  gets overwritten with the resolved path).
- `src/cli.c` -- the parser. `cli_parse_run` resets all state first
  (glibc: `optind = 0` fully reinitializes getopt), so it is
  re-callable -- the unit test parses ~25 times in one process.

`main.c` keeps QEMU/launch/build as *consumers* of `config`.
`CMakeLists.txt` + the root `Makefile` prereq list both gained
`cli.c` -- the Phase-2 lesson (incomplete prereq list = silently
stale binary) applied from memory.

### 2. Compat fixes found by the earlier probe matrix

- **`--rm=true` etc. now work.** pflag accepts `=VALUE` on boolean
  long flags (strconv.ParseBool set: 1,t,T,TRUE,true,True / 0,f,...).
  getopt_long rejects those outright (no_argument), so
  `bool_long_with_value()` handles them: applies the value, names the
  flag in pflag-style errors (`invalid argument "banana" for "--rm"
  flag: want true or false`).
- **Unknown long options name the token**, not a garbage byte: for
  long options glibc sets `optopt` to the option's `val` (1000+),
  which `%c` turned into noise. Now: `podvm: unknown option: --tty`.
- `-t` still rejected (honest: podvm has no TTY); `-it` fails on `t`
  with a clean message.

### 3. `tests/test_cli.c` (new, wired into Makefile + run_tests.sh)

Links the **real** `src/cli.c` -- no mock. (Memory of
`test_host_args.c`: a hand-copied mock that drifted, still accepting
`-it` after the real parser stopped. Replaced in-tree by
`cli_matrix.sh` at the binary level; test_cli is its unit-level
sibling.) 6 groups, ~35 checks:

- `run` keyword consumed, and keyword-less form identical
- flag grammar: `-eFOO=bar`, `--env=FOO=bar`, `-m512`, `-im 512`,
  multi `-e` csv accumulation, comma-in-value escaping (`X=a\,b`)
- `--bool=VALUE` true/false/1/False + rejections
- post-image passthrough (`img --rm` -> `--rm` is the COMMAND),
  `--` terminator
- cross-flag validations (-d/-n/-rm/-i matrix, -p vs --network=none,
  -e/-u/-w/-n/--hostname/--tmpfs/-v/-p specs, -h)
- state reset across parses (nothing leaks between calls)

## What We Learned

### Technical

- **getopt's `optopt` is a trap for long options.** For a long option
  error glibc leaves `optopt` at the option's assigned `val`
  (OPT_RM=1000 -> `%c` prints byte 0xe8). The argv token
  (`argv[optind-1]`) is the only reliable source for the message.
  Short-option errors still use `optopt` (correctly).
- **`optind = 0` (not 1) fully reinitializes glibc getopt** -- needed
  for re-parsing in one process. `1` only resets the public counter.
- **kv_escape escapes spaces too** -- a unit test's expectation of
  `args=-c,echo hi` was wrong; the wire form is `-c,echo\ hi`. The
  test caught *itself*; the parser was right.
- **Sequential `sed -i 'N,Md'` is a line-number trap**: each deletion
  shifts the numbers for the next. Two deletions landed in the wrong
  blocks (clipped `cmd_rmi`'s loop header). Restored from backup and
  redid it as one python pass bottom-up with content-verified
  boundaries. The mission-log-worthy version: one edit pass, verify
  each anchor by content, never by stale line number.

### Process

- The lib question was answerable in two minutes from the code
  (getopt `+` already encodes docker's rule) -- asking it *before*
  writing tests avoided pinning the wrong parser.
- Unit-testing the parser directly (vs cli_matrix.sh's binary
  probing) is what made the `--rm=true` work safe to add: the whole
  matrix re-runs in milliseconds with no VM, no podman, no network.

## What We Decided

- **No CLI-parsing library.** getopt_long stays; docker grammar lives
  in `cli.c`'s contract (header comment) and `test_cli.c`.
- **pflag bool parity**: `--rm=true|false|1|0|t|f|TRUE|False`
  accepted on the four boolean long flags (interactive, detach, rm,
  read-only); anything else `=value` errors naming the flag.
- **Unknown long options are hard errors** naming the token (no
  ignore-and-warn; drop-in compat means scripts fail loudly and
  identically to docker).

## Rules Added

- **Never delete by line number in two passes.** One
  content-anchored edit (or a bottom-up single pass) per file.
- **Unit tests link the real module; never a copy of it.** Already a
  rule in spirit (test_host_args.c's fate); now also stated for the
  CLI layer: `test_cli` links `src/cli.c`.

## What We'd Do Differently

- Extract to `cli.c` the moment the second CLI bug in a week appeared
  (run keyword, then --rm=true) -- the extraction was overdue and
  made both fixes cheap. Next CLI change should start by asking
  "does this live in cli.c yet?"

## Files Changed

| File | Purpose |
|------|---------|
| `src/cli.h` (new) | Parse state types + `cli_parse_run()` contract |
| `src/cli.c` (new) | The run parser (moved from main.c) + `--bool=VALUE` + honest errors |
| `src/main.c` | -340 lines: consumed cli.h instead of owning the parser |
| `CMakeLists.txt`, `Makefile` | Build cli.c (prereq list included) |
| `tests/test_cli.c` (new) | ~35 checks pinning the docker grammar |
| `tests/Makefile`, `tests/run_tests.sh` | Wire test_cli |

## Test Results

```
test_cli: all checks passed          (new)
CLI parsing                          PASS (new, run_tests)
CLI matrix (real binary)             PASS
...
PASS: 37   WARN: 0   FAIL: 0
```

Binary probes:

```
run --rm=true  /nonexistent-x  ->  directory not found   (parsed OK)
run --rm=banana /nonexistent-x ->  invalid argument "banana" for "--rm" flag...
--tty=true     /nonexistent-x  ->  unknown option: --tty
run -im 512    /nonexistent-x  ->  directory not found   (grouped shorts OK)
```

## Next Steps

- ADR-028 guest-side overlay (unchanged; acceptance: mermaid-ascii
  Dockerfile unmodified).
- Compat candidates the parser now makes cheap to add: `-t/--tty`
  (needs a decision: fake tty over serial? reject-as-now?), `--entrypoint`, `--label` (store in config blob), `--platform`
  (accept-only-linux/amd64), `-P` (publish all EXPOSEd).
- `podvm build`'s hand-rolled flag loop (`cmd_build`) is the same
  pattern the extraction just fixed -- worth the same treatment when
  build flags grow (--build-arg, --target, --no-cache).

---

## Session 2: -t/--tty via serial, vsock PTY deferred (2026-10-14)

### The Question

Fake-over-serial or hijack vsock for docker's `-t`?

### The Decisive Evidence (from the tree, not opinion)

1. **The serial console is already a real tty.** The entrypoint's
   fds 0/1/2 are the kernel console (ttyS0) -- a genuine guest line
   discipline: isatty() is true, echo works, Ctrl-C becomes SIGINT.
   "Fake" is only half-right: a real tty on a UART instead of a PTY.
2. **A guest PTY is feasible whenever wanted:** init.c already mounts
   devpts (mount_pseudofs_at), and both host and synthetic kernels
   have UNIX98_PTYS.

### Decision

- **Accept -t/--tty now, as parse-level honesty** (a tty genuinely
  exists; recorded in config.tty for a future channel to act on).
  docker `-it` muscle memory parses.
- **vsock-backed PTY is the end-state, built on demand** -- when
  window-size forwarding (SIGWINCH -> TIOCSWINSZ), stream separation
  (dmesg out of the user's terminal), or `docker attach` need it.
  It must be a THIRD allocated port (vsockalloc), never a hijack of
  the config (5000) or exit-report channels.
- Not speculative: ~250 lines (guest relay + host relay + port
  alloc) is cheap but not free; the UART tty covers isatty/echo/
  signals today.

### Fossil flipped

`cli_matrix.sh` pinned `-it` as an expected "phantom" failure -- a
relic of the old drifted mock parser. Now a positive assertion (optstring
matrix includes -t; `--tty=banana` rejected; -t alone != -i).

### Test Results

```
test_cli: all checks passed   (-it, -t, --tty=true, --tty=banana)
CLI matrix                    PASS (phantom replaced with -y)
PASS: 37   FAIL: 0
```
