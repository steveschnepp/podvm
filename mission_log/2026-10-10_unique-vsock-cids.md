---
name: unique-vsock-cids
description: replace the hardcoded vsock guest-CID (3) and exit port (5001) with per-VM flock allocation, unlocking concurrent VMs -- and the exit port had to move too
project: podvm
---

# Mission Log: Unique vsock CID and Exit Port per VM

---

## What We Did

Picked up the top item carried across the last two logs: the vsock guest
CID was hardcoded to `3` (`GUEST_CID` in `main.c`) and the exit-report
port to `5001` (`VSOCK_EXIT_PORT`), so only ONE vsock VM could exist at a
time -- an orphaned QEMU holding CID 3 made every later boot fail its
`vhost-vsock` attach.

The obvious fix (allocate a unique CID) turned out to be only half the
job. Mapping the two vsock directions first:

- **Host -> guest config**: host dials `<cid>:5000`. Needs the guest's
  CID, which the host must know. This is the hardcoded-3 that breaks.
- **Guest -> host exit code**: guest dials `VMADDR_CID_HOST(2):5001`;
  host listens on `VMADDR_CID_ANY:5001`. Does NOT need the guest CID,
  but the port is a **host-global namespace** -- a second podvm process
  binding 5001 gets `EADDRINUSE`, its helper dies, and that run silently
  loses its exit code (or, with reuse, cross-delivers another VM's code).

So making the CID unique would have made concurrency *possible but
incorrect*: two VMs would boot, then collide on port 5001. Both resources
had to be allocated per VM.

### The allocator (`src/vsockalloc.c/.h`, new)

Non-blocking `flock` over a per-number file in `/tmp/podvm/vsock-locks`
(`cid-<N>`, `port-<N>`) -- the same inode-is-identity pattern as the
container-name lock from the `rm-and-run-flags` session 4. First free
number from a base wins; the fd is **deliberately not CLOEXEC**, so a
forked QEMU inherits it and holds the CID exactly as long as the VM
lives -- including under `--detach`, where QEMU outlives the podvm
process. Honors `PODVM_VSOCK_LOCKDIR` so tests point it at a temp dir.

- CID base 3 (0 = hypervisor, 2 = host are reserved); port base 5001.
- Range of 256 each; exhaustion degrades loudly (CID -> cmdline-only
  config, the existing no-vsock path; port -> no exit report).

### Wiring

- `main.c`: `GUEST_CID`/`GUEST_CID_STR`/`VSOCK_EXIT_PORT` constants
  replaced by `vsock_cid`/`vsock_exit_port` globals. `detect_vsock()`
  claims the CID (single call site, both run and build paths);
  `start_exit_helper()` claims the port lazily (so `--detach`, which
  never starts a helper, claims none and sends no token); the QEMU
  device arg became a runtime `snprintf` instead of a compile-time
  string concat; `send_config_vsock()` dials the allocated CID.
- `build_config_tokens()` emits a new `podvm.exitport=N` token, so the
  guest dials the port the host actually bound.
- `guest/init.c`: `Config.exit_port` (default `VSOCK_EXIT_PORT`),
  `podvm.exitport=` parsed in `parse_config()`, `report_exit()` now takes
  the port. `podvm run` already returns the guest's reported code
  (`guest_code >= 0 ? guest_code : rc`), so a lost exit channel shows up
  as QEMU's 0 instead of the real code -- which is what makes the
  concurrency e2e able to detect it.

### Drive-by: flush the host banner

`start_qemu()`'s startup banner (`podvm: vsock: yes (cid N)`) is
block-buffered when stdout is redirected to a file, so it lands in the
log only after the process flushes -- *after* QEMU's serial output has
interleaved for pages. Added `fflush(stdout)` after the banner so host
diagnostics reach a redirected log before the guest floods it. This is
what the concurrency e2e reads to compare the two CIDs.

### A test-logic dead end

The unit test's first draft asserted `vsock_alloc_port() == 5006`
immediately after `5003`, expecting it to skip the held `5005`. But
`5004` is still free and comes first, so it returned `5004` and the
assert aborted. The CID case happened to be correct because I had filled
up to the number just before the held one. Fixed by allocating `5004`
too, so the *next* call has no choice but to step over `5005`.

---

## What We Learned

### Technical

- **A vsock "unique CID" fix is incomplete without the exit port.** The
  two vsock channels have different addressing (host dials the guest's
  CID; the guest dials the host's fixed CID 2), and the exit port lives
  in the host-global port namespace. Fixing one without the other trades
  a boot failure for a silent exit-code loss -- worse, because it looks
  like success.
- **The lock fd IS the lifetime mechanism, again.** Same trick as the
  container-name lock: not-CLOEXEC so the forked QEMU inherits it. Under
  `--detach` the podvm process exits immediately but the CID stays
  reserved exactly as long as QEMU runs, with no bookkeeping.
- **`flock` files persist but the lock does not.** A fresh podvm gets
  `cid-3` even though `cid-3` the *file* already exists from prior runs;
  only a live holder blocks it. This is what makes "never delete the lock
  file" safe.
- **stdout to a file is block-buffered.** Host `printf` diagnostics sat
  in the buffer while the guest's serial (a separate process writing the
  same fd) streamed for pages. Any e2e that greps host startup output
  needs an explicit flush, or it reads the banner at an arbitrary offset.

### Process

- **Trace both directions of a channel before sizing the fix.** The CID
  was the reported bug; the port was the latent one sitting one layer
  down. The grep-for-CID pass surfaced `VSOCK_EXIT_PORT` bound to a fixed
  number in the same helper, which is what turned "allocate a CID" into
  "allocate a CID *and* a port."
- **A failed background-VM e2e leaks the VM.** My first run failed an
  assertion and left the backgrounded run A alive -- still holding CID 3
  and port 5001 -- which is why the next manual run silently got `cid 4`.
  The trap only cleaned `$WORK`, not the process. Fixed by killing the
  process group in the `EXIT` trap, not just on the success path.
- **The e2e assertion must be satisfiable only by the fix.** Exit 42 vs
  QEMU's 0 is the discriminator: without a private exit port, B's helper
  fails to bind, `guest_code` stays -1, and podvm returns 0. So `RC==42`
  proves the port was B's own, and the distinct-CID comparison proves the
  device was too.

---

## What We Decided

- **Both the CID and the exit port are allocated per VM** via the same
  flock allocator; the port travels to the guest as `podvm.exitport=`.
- **CID is claimed in `detect_vsock()`** (one call site covers run and
  build); **the port is claimed lazily in `start_exit_helper()`** so
  `--detach` (no helper) claims and advertises none, preserving today's
  "guest tries 5001, nobody listens, best-effort" behavior for detached
  runs.
- **Exhaustion degrades loudly, not fatally** -- consistent with the
  existing best-effort vsock posture (cmdline config is the fallback).
  256 concurrent VMs is far outside this PoC's envelope.
- **Lock files under `/tmp/podvm/vsock-locks`**, not XDG_STATE_HOME,
  because vsock resources are host-global (like the image cache), not
  per-container. Cross-user collision is theoretically possible but
  `/dev/vhost-vsock` is privilege-gated; noted, not solved.
- **Build's sequential step VMs reuse one CID and one port** -- they never
  overlap, so per-step allocation would only churn the range.

---

## Rules Added

- **A "unique vsock resource" fix must cover every vsock endpoint**, not
  just the one that was reported: trace host->guest and guest->host
  separately, and remember the host-side port namespace is global.
- **A lock file's fd is the resource's lifetime; make it not-CLOEXEC**
  when a forked child (QEMU) should outlive the allocating process.
- **Background-VM e2e: kill the process group in the `EXIT` trap**, not
  only on the success path, or a failed assertion leaks a VM that still
  holds the shared resource.
- **Flush host stdout before forking a process that writes the same fd**,
  or redirected logs interleave and greppable diagnostics land at an
  arbitrary offset.
- **When an assert can be off-by-N, fill every slot below the held
  number first** so the "skip" case is actually exercised.

---

## What We'd Do Differently

- **The exit-port half was invisible until I grepped for the CID's
  sibling.** A quick "list every vsock constant and port bind" pass at
  the start would have framed this as a two-resource problem before I
  wrote any code. The CID was the headline; the port was the trap.
- **The unit test's off-by-one was avoidable** by reasoning about the
  free-list position explicitly (which numbers are held at each step)
  instead of assuming the held number was adjacent.
- **The e2e's trap should have owned process cleanup from the first
  draft.** The leaked VM not only failed the run, it poisoned the *next*
  run's CID, which reads as a scary "why cid 4?" until you realize a
  background process is still alive.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/vsockalloc.c` / `.h` | New: flock allocator for a unique guest CID + exit port (`PODVM_VSOCK_LOCKDIR` override) |
| `src/main.c` | `vsock_cid`/`vsock_exit_port` globals; runtime device arg; `detect_vsock` claims CID; `start_exit_helper` claims port; `podvm.exitport` token; banner `fflush` |
| `guest/init.c` | `Config.exit_port` (default 5001), `podvm.exitport=` parse, `report_exit(code, port)` |
| `tests/test_vsockalloc.c` | New: base values, consecutive allocation, skip-held-number |
| `tests/e2e_concurrency.sh` | New: run A stays up while run B boots + reports exit 42 (distinct CID + port) |
| `CMakeLists.txt`, `Makefile` | Build `vsockalloc.c` |
| `tests/Makefile`, `tests/run_tests.sh` | Wire in test_vsockalloc + the concurrency e2e |

---

## Test Results

```
Vsock alloc                   PASS   (new)
E2E concurrency (distinct CID+port) PASS   (new: CIDs 3,4; B reported exit 42)
...
PASS: 33   WARN: 0   FAIL: 0
```

(Up from 31 PASS before this work: +1 unit row, +1 concurrency e2e row.)

---

## Next Steps

1. Unchanged from previous logs, now that the CID/port collision is
   fixed: build-driver unit tests (`cmd_build` chain assembly, whiteouts),
   a gcov target, OCI blob export from build, ADR-023 slirp-token cleanup.
2. Cross-user vsock lock isolation (a per-uid subdir under the lock dir)
   if podvm is ever run by multiple users on one host.
3. Big rocks unchanged: layer compaction (ADR-020 revisit), the
   performance phase (ADR-019/009), FreeBSD (ADR-021).
