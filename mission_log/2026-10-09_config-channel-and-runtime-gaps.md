---
name: config-channel-and-runtime-gaps
description: close the remaining runtime gaps -- escape the config channel, table-test the config fold, resolve image users by name, fix -i, and add overlay write-back -- two ADRs and two real bugs found by a pty test
project: podvm
---

# Mission Log: Closing the Runtime Gaps

---

## What We Did

Picked up the queue from `2026-10-08_next-steps-batch.md`, which ended with
a ranked list of remaining work. Worked through five items in order, one
commit (or small commit group) each, so all eight commits bisect cleanly.

Final state: **26 PASS / 1 WARN (musl-gcc) / 0 FAIL**, up from 22/1/0. Two
ADRs, three new unit-test binaries, two new e2e tests, ~1500 lines across
20 files.

### 1. Escape the config channel (ADR-025)

The `podvm.*=` channel is a space-delimited token list, and `args`/`env`
are comma-joined within a token. So any value containing a space was
truncated at the token boundary and any value containing a comma was split
in two. Before this session that only affected user-typed args; now it
also broke the image-default-command feature, because an image whose `Cmd`
is `["sh","-c","echo hi"]` could not run with no CLI command.

Backslash is now the escape char (`\\`, `\ `, `\,`). `kv_escape()`
produces the form; `kv_find()`/`kv_copy_value()` became escape-aware (they
stop at an *unescaped* space); `csv_parse()` splits on *unescaped* commas
and unescapes each field; `kv_unescape()` resolves single-valued fields on
the guest. On the host, `build_config_tokens()` escapes every value before
it goes to either the cmdline or vsock, escaping `args`/`env` element-wise
so the separators stay separators. Both channels share one encoder, so they
cannot diverge.

Verified end to end: `podvm alpine /bin/echo "hello space world"` reaches
the guest as one argument.

### 2. Table-test the config fold

`apply_image_config()` held the docker-semantics resolution (Entrypoint vs
Cmd vs a user command, env merging, cwd/user precedence) inline in
`main.c` against the global config, so it was only reachable by booting a
VM. Extracted the pure part into `imgcfg_fold(ic, base, out)` in a new
`imgcfg.c`: no globals, no I/O, takes a base `RunSpec` (the user's CLI
command/args/cwd/user) plus the image config and produces a resolved
`RunSpec`. `main.c` is now just glue.

`test_imgcfg.c` covers 12 cases including the full entrypoint/cmd/user
matrix, env merging and HOME defaulting, cwd/user precedence, and a NULL
image config. **This refactor introduced a regression that a later e2e
caught** -- see "What We Learned".

### 3. Resolve image users by name

The guest parsed `podvm.user` as numeric `uid:gid` only, so an image whose
`User` is a name (`nobody`, `app`, `appuser`) silently ran as root. Added
`userdb.c`: a hand-rolled `/etc/passwd` + `/etc/group` parser covering the
docker spec matrix (numeric `uid:gid`, bare uid, named user, `name:gid`,
`name:group`, `uid:group`). Resolution happens **after chroot** so the
files read are the *container's*, matching docker. An unresolvable name
warns and falls back to root rather than refusing to boot.

Hand-rolled rather than `getpwnam`/`getgrnam` because the guest links
statically (musl when present, glibc otherwise) and NSS under static
linking is fragile across libcs -- same reasoning as the hand-rolled sha256
last session.

Verified end to end: an alpine image with `USER appuser` (uid 1234) runs
`/usr/bin/id` as `uid=1234 gid=1234` in the guest.

### 4. Make `-i` real, with a pty test that found two bugs

Wrote `tests/e2e_interactive.py`: boots a VM with `-i` under a pty, runs
`/bin/cat`, and sends a payload containing a raw `Ctrl-A` (0x01). With `-i`
the guest must receive it and cat must echo the payload back intact. Sync
is explicit (wait for the guest's `exec /bin/cat` trace, then write), the
pty is raw so the terminal cannot self-echo, and the process group is
killed so QEMU does not linger holding the vsock CID.

**It immediately found two real defects** the earlier argv-shim-only check
had missed -- see "What We Learned". Both fixed:
- `-i` never worked: `-serial stdio -nographic` conflicts because
  `-nographic` already wires the default serial to stdio. Use
  `-display none` and let `-serial` own the backend.
- A `printf("%s", NULL)` regression from item 2 rendered the literal string
  `"(null)"`, which became a real argument, so `podvm alpine /bin/cat` ran
  `cat (null)` and the guest panicked.

### 5. Overlay write-back (ADR-026)

The podman-storage path mounted the image as an overlay whose upper layer
was a throwaway tmpfs, so every in-guest write vanished when the VM
exited. `podvm -n <name>` now places the overlay upper + work in a
persistent per-container state dir (`$XDG_STATE_HOME/podvm/containers/<name>`,
or `~/.local/state/...`) instead. Writes land in that host dir and are
visible to the next run using the same name. Without `-n` the upper stays
on tmpfs, so the throwaway default is preserved exactly.

The overlay is mounted in the QEMU child's userns, where the invoking uid
maps to root, so files created in the persistent upper are owned by the
invoking user outside -- no root-owned state left behind.

This is host-side on purpose. ADR-020 proposed a guest kernel module, but
that assumed virtiofs-served per-layer mount tags and a podvm-controlled
synthetic kernel. The built system serves a host-side overlay over 9p using
the *host's* kernel, so a guest module has nothing to assemble and cannot be
compiled in. ADR-026 records the divergence and keeps ADR-020 alive for
`podvm build` layer commits.

---

## What We Learned

### Technical

- **`-nographic` is not a synonym for `-display none -serial stdio`.**
  `-nographic` implicitly wires the *default* serial to stdio, so pairing it
  with an explicit `-serial stdio` makes QEMU refuse to start ("cannot use
  stdio by multiple character devices"). If you want to control the serial
  backend yourself, use `-display none` and own `-serial` entirely.
- **`snprintf("%s", NULL)` renders the literal string `"(null)"` on glibc.**
  It does not crash and it does not produce an empty string -- it produces
  six characters that then flow onward as real data. Any `%s` on a possibly
  NULL pointer needs a guard or a `""` fallback.
- **The kernel's own cmdline parser will warn about escaped values.** With
  spaces escaped as `\ `, the *kernel* still splits `-append` on the literal
  space bytes, so it reports "Unknown kernel command line parameters" for
  the fragments. This is cosmetic: `/proc/cmdline` preserves the full
  escaped string and the guest's `kv_find` parses it correctly, and vsock
  bypasses the kernel entirely. Worth knowing so it is not mistaken for a
  real config failure.
- **A container's writable layer is just "don't throw the upper away."**
  Overlayfs already writes every change to the upper as it happens, so
  persistence is a directory-location decision, not a syncing mechanism.
  The kernel module ADR-020 proposed would add VFS-hook overhead to solve a
  "when to commit" problem a persistent upper does not have.
- **`pgrep -f <pattern>` matches the shell running it** if the pattern
  appears in that shell's own command line. Cost me a false "orphan QEMU"
  alarm. Use `ps -eo cmd | grep -c '^qemu-system'` (anchored on the binary
  path) or check PPID when the pattern could be self-referential.
- **`build-initramfs.sh` writes the cpio to `$1` and progress to stdout.**
  Redirecting stdout into the same path corrupts the archive; the kernel
  then fails to find `/init` and panics with "VFS: Cannot open root fs".

### Process

- **A pty-driven test earns its keep by catching what argv inspection
  cannot.** The `-i` flag had been "verified" last session only by inspecting
  the QEMU argv through a shim on `PATH`. That confirmed the flag changed the
  args; it could not confirm the VM actually booted. The moment a real VM
  booted under `-i`, two defects surfaced. The lesson: for anything touching
  device/backend wiring, an argv assertion is necessary but not sufficient --
  boot the thing.
- **`git checkout <file>` to undo debug prints will also eat uncommitted
  feature work in that file.** Happened again this session (recovered by
  re-applying recorded edits). The habit to keep: `cp` the file to `/tmp`
  before instrumenting, or commit first.
- **Backticks in a `git commit -m "..."` message are shell-interpreted.**
  `-i` inside backticks became a command substitution and mangled the message.
  Write the message to a file and use `git commit -F <file>`.
- **Table-driven tests are the fastest way to find a refactor regression.**
  The `(null)` bug in item 2 was a one-line guard dropped during the
  extraction. It survived 12 unit cases because none covered "command with
  no args" -- and it took an e2e boot to surface. Add the missing table row,
  not just the fix.

---

## What We Decided

- **Backslash escaping over base64/percent-encoding** for the config
  channel: it keeps `/proc/cmdline` human-readable (the main debugging
  window) and only costs bytes for the characters that need it. Length-prefixed
  vsock framing is the clean long-term answer but does not help the cmdline
  fallback, which must stay text; deferred until the 4096-byte cmdline limit
  actually binds (ADR-025).
- **Hand-rolled passwd/group parsing over `getpwnam`**, to keep the static
  guest free of NSS fragility across musl/glibc -- consistent with the
  hand-rolled sha256.
- **An unresolvable `podvm.user` name warns and runs as root** rather than
  refusing to boot, matching podvm's best-effort posture elsewhere (e.g. no
  eth0, no vsock).
- **Write-back is opt-in via `--name`**, so unnamed runs keep the throwaway
  default and existing scripts are unaffected. Sharing state by name is the
  convention every container user expects (ADR-026).
- **ADR-020 is narrowed, not superseded.** Its guest-module design remains
  the plan for `podvm build` layer commits; ADR-026 supersedes it only for
  the "`podvm run` needs a persistent writable layer" case, and records that
  the module's original premise no longer describes the built system.

---

## Rules Added

- **Do not use `-nographic` when you need to control the serial backend.**
  Use `-display none` plus explicit `-serial`. `-nographic` silently wires
  the default serial to stdio and will conflict.
- **Never pass a possibly-NULL pointer to `snprintf("%s", ...)`.** Guard it
  or substitute `""`; glibc renders `(null)` and it propagates as real data.
- **A device/backend flag needs a boot test, not just an argv assertion.**
  If it changes QEMU args, boot the VM and check the guest actually came up.
- **Escape-aware parsing must be applied at every level of a nested
  format.** Here: token boundaries (space) *and* list separators (comma)
  both needed escape handling, or fixing one level silently broke the other.
- **Anchor `pgrep`/`grep` on the binary path when the pattern could appear
  in your own command line**, or you will get a false positive on the shell
  running the check.
- **Redirect a build script's stdout away from the file it writes to `$1`.**
  They are usually different outputs; conflating them corrupts the artifact.

---

## What We'd Do Differently

- **The `(null)` regression should have been caught by the unit tests, not
  an e2e.** `test_imgcfg.c` had 12 cases but none exercised "user command
  with no args." When extracting a function, enumerate the input-space
  branches the old code guarded and make each a table row *before* deleting
  the old guards.
- **The `-i` bug was avoidable last session** if I had booted a VM once
  instead of trusting the argv shim. The shim was quick and convincing, which
  is exactly why it was insufficient.
- **Write-back concurrency is unguarded.** Two live runs sharing a `--name`
  would share an overlay upper, which overlayfs does not support for two
  writers. The convention (one live container per name) matches docker, but
  podvm does not detect the conflict yet -- a flock on the state dir would be
  a cheap guard.
- **`userdb.c` duplicates small parsers** (field extraction, digit check) that
  could live in `strparse.c`. Fine at this size; worth consolidating if a
  third caller appears.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/strparse.c` | Escape/unescape (`kv_escape`, `kv_unescape`); escape-aware `kv_find`/`kv_copy_value`/`csv_parse` |
| `src/strparse.h` | New escape API |
| `src/imgcfg.c` | New: pure `imgcfg_fold()` (docker command/env/cwd/user resolution) |
| `src/imgcfg.h` | New: `RunSpec` + fold API |
| `src/userdb.c` | New: passwd/group parser for `podvm.user` by name |
| `src/userdb.h` | New: `userdb_resolve()` API |
| `src/main.c` | Shared `build_config_tokens()` encoder; `-i` uses `-display none`; `--name` + persistent overlay upper; `imgcfg_fold` glue |
| `guest/init.c` | Unescape single-valued fields; resolve `podvm.user` via `userdb_resolve()` after chroot |
| `guest/Makefile` | Link `userdb.c` |
| `CMakeLists.txt` | Build `imgcfg.c` |
| `tests/test_config.c` | Escape round-trip + escape-aware token tests |
| `tests/test_imgcfg.c` | New: 12 table cases for the config fold |
| `tests/test_userdb.c` | New: docker user-spec matrix + degenerate args |
| `tests/e2e_interactive.py` | New: pty harness proving `-i` delivers Ctrl-A |
| `tests/e2e_writeback.sh` | New: named-container write persistence |
| `tests/Makefile` | New test targets + zstd/imgcfg/userdb links |
| `tests/run_tests.sh` | Wire in imgcfg, userdb, interactive, write-back |
| `adrs/podvm_adr025_escape-config-channel.md` | New: escaping decision |
| `adrs/podvm_adr026_host-side-writeback.md` | New: host-side write-back, ADR-020 narrowed |

---

## Test Results

```
Config parsing                PASS   (now covers escape round-trips)
Host args                     PASS
Vsock basics                  PASS
Image pipeline                PASS
SHA-256                       PASS
Image config fold             PASS   (new: 12 table cases)
User resolution               PASS   (new: docker spec matrix)
Host build                    PASS
Guest build                   PASS
Initramfs build               PASS
QEMU available                PASS
KVM available                 PASS
9p modules available          PASS
musl-gcc available            WARN   (pre-existing; not installed)
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
E2E interactive (-i)          PASS   (new)
E2E write-back (--name)       PASS   (new)

PASS: 26   WARN: 1   FAIL: 0
```

Manual proofs: a named container wrote `/wb.txt` and a second run of the
same name read it back while an un-named run did not; an image with
`USER appuser` ran as `uid=1234 gid=1234`; `podvm alpine /bin/cat` under
`-i` echoed a payload containing a raw `Ctrl-A` intact.

---

## Next Steps

Ranked by value. Items 1-2 from the previous log are done (args-with-spaces,
write-back); this is the refreshed remainder.

1. **Guard write-back concurrency.** Two live runs sharing a `--name` share
   an overlay upper, which overlayfs rejects for two writers. A `flock` on
   the state dir would fail fast with a clear message instead of undefined
   behavior. Cheap, and the only correctness hole left by item 5.
2. **`podvm rm` / state GC.** Write-back state under
   `$XDG_STATE_HOME/podvm/containers/` grows without bound and nothing
   removes it. A `--rm` flag (delete the state dir on exit) and/or a `rm`
   subcommand would make the feature operable.
3. **Compact long-lived state into an OCI layer.** The natural complement to
   write-back, and the part of ADR-020 aimed at `podvm build`: turn a
   container's accumulated upper into a real layer so it can be reused as an
   image. Revisit ADR-020's guest-module design first, re-evaluated against
   the 9p + host-kernel reality.
4. **`podvm build`.** Still the largest unbuilt piece of the OCI story. Needs
   the layer-commit path (item 3) first.
5. **The performance phase (ADR-019 virtiofs, ADR-009 bridge/tap).** Deliberately
   untouched this session; it changes measured behavior across the board and
   deserves its own focused session with before/after numbers.
6. **FreeBSD platform (ADR-021).** Large, independent, no urgency.
