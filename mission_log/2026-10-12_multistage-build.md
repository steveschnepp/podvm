---
name: multistage-build
description: multi-stage Dockerfile support (FROM..AS, COPY --from), multi-source COPY, WORKDIR-relative COPY dst -- and the dogfood that forced it: building mermaid-ascii inside podvm
project: podvm
---

# Mission Log: Multi-Stage Builds

---

## What We Did

`podvm build` rejected a second `FROM` outright ("multi-stage builds are
not supported"). That ceiling came down, along with two smaller
Dockerfile-compat gaps a real-world Dockerfile exposed immediately.

### The trigger

The user handed over the `mermaid-ascii` Dockerfile (Go, two stages) as
a dogfood E2E: "run it with podvm". It exercises every new piece at
once -- `FROM ... AS builder`, multi-source `COPY go.mod go.sum ./`,
`COPY . .`, and two `COPY --from=builder` (one file, one directory).

### Multi-stage design

Each `FROM` now starts a **stage**: its own base resolution, its own
committed-layer chain, its own metadata (env/cwd/user/cmd), stored in a
heap-allocated `BuildStage` array (stage-sized arrays are far too big
for the stack). Semantics matched against docker:

- `FROM <prior stage>` -- the named stage's whole chain becomes the new
  base, and its final config is inherited.
- `COPY --from=<stage>` -- the source is a path inside that stage's
  **merged rootfs**, not the build context.
- `COPY --from=<image>` -- same, resolved through podman/native store.
- The **last stage is the image**; intermediate ones just stop.

The interesting bit was `COPY --from` needing a merged view of a layer
chain *on the host*, where there is no overlay mount (rootless). The
guest merges layers via 9p at RUN time; for a copy we need the answer
before any VM boots. So `build_copy_from_chain()` walks the chain
bottom-to-top itself: whiteout (char 0:0) removes the path in the
merged tree, a directory merges entry-wise, a file replaces wholesale.
Only the requested subtree is materialized (a `/build/mermaid-ascii`
copy must not drag the 200 MB golang tree into a temp dir).

### Bugs the dogfood flushed out

1. **`COPY go.mod go.sum ./`** (multi-source) silently copied only the
   first source -- the parser took token 1 as src and token 2 as dst and
   dropped the rest. Now: everything but the LAST token is a source, and
   with >1 source the dst is treated as a directory (docker rules).
2. **Relative COPY dst** (`./`) was rejected outright. Docker resolves
   it against WORKDIR; now so do we.
3. **`COPY --from` of a directory** hit a bare `mkdir()` in the merge
   walker that didn't create parents (`/tmp/.../build/templates` with
   no `/tmp/.../build`) -- `mkdir_p()` fixed it.

### The rootless-9p wall (not fixed, documented)

`RUN chown -R mermaid:mermaid /app` fails in the guest with
`Invalid argument`. Chown to the file's current uid works; cross-uid
does not. Cause: the rootfs is served by QEMU 9p
`security_model=none` as the **unprivileged host user**, so guest-root's
cross-uid chown is a host-side chown nobody has permission for. Any
Dockerfile that drops privileges via chown hits this. Worked around in
the dogfood (`chown ... || true`; files stay root-owned but
world-readable, `USER mermaid` still works). The real fix is a
guest-side overlay upper (tmpfs) so ownership changes are guest-local --
an ADR-sized piece of work, deliberately not started here.

### Bonus flags

`podvm build -m MB -c CPUS` now sizes the RUN-step VMs. A Go build in
128 MB / 1 CPU is not happening; `-m 3072 -c 4` is.

## The Payoff

`podvm build` compiled mermaid-ascii end-to-end (go mod download needed
the slirp network inside the step VM -- it worked), then `podvm run`
with a `-v` bind mount rendered the podvm README's own mermaid
architecture diagram, in the podvm VM, with the binary podvm had just
built. Full circle in one pipeline.

## Verified

- `tests/test_build`: `FROM..AS`, `COPY --from` (stage + image),
  multi-source COPY, error cases (`AS` without name, `--chown`,
  empty `--from`).
- `tests/e2e_build.sh`: new multi-stage section -- artifact crosses via
  `COPY --from`, builder-only files and builder whiteouts do NOT leak
  into the final stage (checked via exit code; marker strings would
  echo in the serial log's cmdline trace).
- Full e2e build suite passes; the original single-stage tests
  (whiteouts, config inheritance, failing-RUN cleanup, rmi) still hold.

## Carried Forward

- Guest-side overlay upper: fixes cross-uid chown AND makes RUN-step
  layer commits cheaper (host currently commits the step's upper dir).
- `podvm build` has no layer cache -- every `RUN` is a fresh VM
  (~seconds each); a cache keyed on the step's input chain would make
  the mermaid-ascii rebuild loop much nicer.
- `RUN` exec form and `ARG` are still unsupported (rejected, not
  silently ignored).
