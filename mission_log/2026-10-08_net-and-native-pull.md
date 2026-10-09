---
name: net-and-native-pull
description: fix the never-tested native registry pull (bearer token truncation) and implement slirp networking so DNS+HTTPS works from inside the guest -- no static link needed
project: podvm
---

- Previous: `2026-10-08_podman-storage.md`

# Mission Log: Achieving Pull (Native Client + Guest Networking)

---

## What We Did

The ask was "think on how we can achieve pull". Two sessions earlier the
native OCI registry client had been written but **never exercised against a
live registry** (network was unavailable then). This session split "pull"
into the three things it had become, and closed two of them.

### Phase 1: The native registry pull was silently broken

Network was live now (`registry-1.docker.io/v2/` -> 401 as expected,
`auth.docker.io/token` -> 200). A probe binary calling `image_pull("busybox",
"latest")` directly (bypassing podman-first resolution) failed with
`HTTP 401` on the manifest fetch.

Inspection of the auth chain looked correct, so I instrumented
`registry_get` in place. The tell: `DBG token len=1023` and the retry still
401. The token fetched fine (2700 chars) but was **truncated**.

Root cause: two `snprintf` truncations, same class of bug, two locations:

| Buffer | Was | Effect |
|---|---|---|
| `char token[1024]` in `registry_get` | 1024 | Docker Hub JWT (~2.7KB) clipped to invalid JWT -> 401 |
| `char auth[512]` in `http_get` | 512 | `Authorization: Bearer <token>` header clipped mid-token -> 401 |

Manual curl with the same full token: **200**. So the token and logic were
right; the plumbing around them clipped it. Both buffers -> 4096, and
`registry_token` now *detects* truncation (`w >= n`) and prints "auth token
too long" instead of silently accepting a clipped token.

Verified end-to-end: busybox:latest pulled through the native client
(manifest -> bearer auth -> platform manifest -> layers -> gunzip -> untar)
into a real rootfs that boots in the VM.

### Phase 2: The static-link reframing

The DNS+HTTPS e2e had been deferred twice because a *static* test binary
would not link in the guest (missing `gssapi_krb5`/`krb5`/`sasl2` static
libs). The user's observation: **use a real image (debian/alpine) -- it
ships its own libc + loader, so a dynamic binary just runs after chroot.**
No static link needed at all.

That reframing exposed the actual blocker: **the guest had no network
device.** No `-netdev`, no `virtio-net-pci`. The static link was only ever
the *second* problem.

### Phase 3: Implement slirp (ADR-023, already accepted)

- **QEMU args** (`src/main.c`): `-netdev user,id=net0` +
  `virtio-net-pci,netdev=net0`. In-process, no daemon, no root, no host
  state -- same rule that made 9p work.
- **Module bundling** (`guest/build-initramfs.sh`): added `virtio_net`
  plus its transitive deps `net_failover` and `failover` (per
  `modules.dep`). Transitive bundling again.
- **Guest load list** (`guest/init.c`): `failover.ko`, `net_failover.ko`,
  `virtio_net.ko` appended in dependency order.

But the device appearing (`00:03.0: [1af4:1000]`, class 0x020000) and all
10 modules loading (`/proc/modules` listed `virtio_net`) still left
`/proc/net/dev` showing eth0 **down with no IP**. Nothing configured the
interface: slirp serves DHCP, but there is no DHCP client in the initramfs
and init never touched eth0.

### Phase 4: Guest interface config

`setup_network()` in `guest/init.c`: static config via ioctl using slirp's
well-known constants -- 10.0.2.15/24, default route via 10.0.2.2, and
`/etc/resolv.conf` -> 10.0.2.3. Best-effort: if eth0 is absent (no
virtio-net, synthetic kernel without netdev) the guest just boots without
network; isolation and boot are unaffected.

Result: `podvm-init: eth0 up 10.0.2.15 via 10.0.2.2`, and alpine's dynamic
`busybox wget` completed a real HTTPS request from inside the VM.

### Phase 5: Bugs found and fixed along the way

Each fixed in its own commit (commit-always, fix-forward):

1. **getopt permutation** (`c47fcb6`): `podvm img /bin/sh -c "..."` stole
   the entrypoint's `-c` as podvm's own `--cpus`, taking the command string
   as its value (`atoi`=0 -> "Invalid CPU topology"). `+` in the optstring
   stops option parsing at the first positional.
2. **pseudofs lost at chroot** (`39c8ed4`): `mount_pseudofs()` mounted
   proc/sys/dev at the *initramfs* root, then `chroot(cfg.root)` hid them.
   After chroot, `/proc` resolved to the container's empty `/rootfs/proc`,
   so any tool reading `/proc/net` or `/proc/self` (ifconfig, curl, ps)
   failed ENOENT. Refactored `mount_pseudofs_at(root)` and call it again
   inside the new root.
3. **`-` sentinel became a literal arg** (`39c8ed4`): host sends
   `podvm.args=-` for "no args", but the guest passed `-` through, so
   `ifconfig` ran as `ifconfig -`. Fixed by trimming the token
   (`kv_copy_value`) before comparing. My first attempt compared the raw
   `kv_find` pointer, which still held the *rest of the string*
   (`- podvm.env=...`), so the `strcmp` failed -- needed the token extracted
   first.
4. **No interface config** (`15593a1`): see Phase 4.

### Phase 6: The e2e test

`tests/e2e_net.sh` (`dc81d8a`): boots a VM with slirp, brings up eth0, runs
alpine's dynamic `busybox wget` against an endpoint returning 200
(`auth.docker.io/token`). Asserts eth0 at 10.0.2.15, no DNS/route/TLS
errors, and `rc=0`. Gated on an alpine image being present.

A `401` from `registry-1.docker.io/v2/` would *also* prove the stack works
(unauthenticated `/v2/` correctly 401s); the test hits a 200 endpoint for
an unambiguous signal.

---

## What We Learned

### Technical

- **Docker Hub bearer JWTs are ~2.7KB.** Any buffer holding one must be
  >= 4KB. `snprintf` truncation here is silent and produces a *confusing*
  downstream error (401 "unauthorized") rather than an obvious overflow.
- **`snprintf` return-value checking is not optional** for security- or
  auth-sensitive copies. Detect `w >= n` and fail loudly.
- **A real container image ships its own libc + loader.** A dynamic binary
  from the image runs after chroot with zero extra work -- so test-network
  problems need a static link only if you insist on injecting a foreign
  binary into a bare rootfs. Test against the image instead.
- **Rootless podman mount paths are invisible from the host namespace**
  (re-confirmed): probing `$d/bin/curl` from the host after
  `podman image mount` returns nothing. Inspect *inside* a `podman unshare`
  wrapper, one image at a time (leaked loop variables across iterations
  caused a confusing wrong-answer detour).
- **`chroot` hides previously-mounted pseudofs.** Anything mounted at the
  initramfs root (`/proc`, `/sys`, `/dev`) is unreachable after
  `chroot(cfg.root)`. Container inits must re-mount inside the new root.
- **An interface that exists is not an interface that works.** `/proc/net/
  dev` listing eth0 only means the driver bound; without an address + route
  it is down and useless. Slirp's DHCP is useless without a DHCP client.
- **`getopt` permutes by default.** A CLI that takes both its own flags and
  an arbitrary entrypoint command must use `+` (or `--`) or it will steal
  the command's flags.

### Process

- **The user's reframing ("use a real image") was the unlock.** Two sessions
  were spent fighting a static link that a real image makes unnecessary.
  When a test fights the toolchain, question whether the test's shape is
  right before fighting harder.
- **Commit-always + fix-forward kept a 6-bug session bisectable.** Each bug
  got its own green commit; no rewrites. The commit list reads as a
  debugging narrative.
- **Instrument in place, revert the instrumentation.** Adding `DBG` prints
  to `registry_get` found the truncation in one run, far faster than
  theorizing about the auth flow.

---

## What We Decided

- **Native registry client stays as the fallback** (podman storage is
  primary per ADR-024); the truncation fix matters for podman-less hosts.
- **Slirp networking implemented as ADR-023 specified** -- in-process, no
  daemon, no root, guest config via static constants. Bridge/tap remains
  the performance-phase default.
- **Static slirp config in guest init** rather than bundling a DHCP client
  (no new deps; slirp's constants are stable). Revisit when bridge
  networking needs per-container addresses via the `podvm.*=` channel.
- **`-` is the empty-args sentinel** and is trimmed, not passed through.

---

## Rules Added

- **Auth/JWT buffers >= 4KB, and check `snprintf` returns.** Silent
  truncation of a token looks like an auth failure, not a buffer bug.
- **Re-mount pseudofs after `chroot`.** `/proc` `/sys` `/dev` mounted
  before chroot are unreachable after it.
- **Test networking against a real image**, not a static binary injected
  into a bare rootfs -- the image ships its own libc + loader.
- **Inspect rootless podman mounts only from inside `podman unshare`,** one
  image at a time.
- **CLI takes entrypoint flags -> use `+` in the getopt optstring.**

---

## What We'd Do Differently

- **The native pull should have had a live-registry smoke test from day
  one.** It sat untested for two sessions and hid a two-line buffer bug
  that made it completely non-functional. Unit tests of the pure
  sub-functions (JSON, gzip, tar) gave false confidence that the whole
  path worked.
- **My first `-` sentinel fix was wrong** (compared the untrimmed
  `kv_find` pointer). Writing the failing-case test first would have caught
  it; instead the e2e caught it a build later.
- **The args-with-spaces limitation is still open**: the space-delimited
  `podvm.args=` token cannot carry an argument containing a space (found
  when a multi-word `ifconfig ... | grep` entrypoint was truncated). Not
  blocking the current tests (comma-joined args work), but real commands
  with spaced args will need a quoting/escaping scheme on the config
  channel.

---

## Files Changed

| File | Purpose |
|------|---------|
| `src/image.c` | Bearer token buffers 1024/512 -> 4096; truncation detection |
| `src/main.c` | `+` in getopt optstring; `-netdev user` + `virtio-net-pci` |
| `guest/init.c` | pseudofs re-mount post-chroot; `-` sentinel; `setup_network()`; resolv.conf |
| `guest/build-initramfs.sh` | Bundle virtio_net + net_failover + failover |
| `tests/e2e_net.sh` | New: DNS+HTTPS out of the guest via alpine dynamic wget |
| `tests/run_tests.sh` | e2e_net wiring (gated on alpine image) |

---

## Test Results

```
... prior suite ...
E2E boot (9p + vsock)         PASS
E2E podman image              PASS
E2E net (DNS+HTTPS)           PASS

PASS: 19   WARN: 1 (musl-gcc)   FAIL: 0
```

Manual proofs: `busybox:latest` pulled through the native client and booted;
alpine dynamic `wget` completed HTTPS (`rc=0`, HTTP 200) from inside the
guest with `eth0 up 10.0.2.15 via 10.0.2.2`.

---

## Next Steps

Refined after verifying the current state (tree clean, all three e2e
tests green: 19 PASS / 1 WARN / 0 FAIL). Ranked by value.

1. **Image config blob (Env/Entrypoint/Cmd) -- the recommended next
   step.** Verified broken today: `podvm alpine:latest` with *no* command
   sends an empty entrypoint (`podvm.entrypoint=`) and the guest rejects
   it ("podvm.entrypoint not set"). So podvm can only run images whose
   command the user types, which undercuts the OCI promise (ADR-005) that
   an image carries its own default command. Now cheap: the storage
   backend already parses manifests, and the config blob sits in podman
   storage big-data (`images.json` -> `big-data-names` ->
   `manifest-sha256:*` -> JSON with Env/Entrypoint/Cmd/WorkingDir).
   Unlocks: `podvm <image>` just runs the image's default command; env
   and working-dir honored, not just CLI flags.
2. **sha256 layer-digest verification** (`image.c` TODO). Real integrity
   gap; do it before the pull path is used against untrusted registries.
3. **zstd layer support** (`image.c` TODO). Registries increasingly
   default to zstd; will bite eventually, not immediately.
4. **`-i` / `--interactive` is a dead flag.** Parsed and stored
   (`config.interactive`) but never read -- QEMU already attaches stdio
   via `-serial mon:stdio -nographic`. Either wire it to something real
   or drop it from the CLI.
5. **Native-pull e2e coverage.** The native registry client works (fixed
   this session) but is not exercised by the suite -- only podman storage
   is. Add an e2e that pulls via the native path and boots.
6. **Overlay write-back.** The podman-storage upper layer is a throwaway
   tmpfs, so in-guest writes vanish with the VM. Needs ADR-020's guest
   overlay module design to commit changes back.
7. **Args-with-spaces on the config channel.** Left as-is per decision;
   the space-delimited `podvm.*=` format cannot carry an argument with a
   space (a format constraint, not a parser bug -- no library fixes it).
   Revisit if real entrypoints need spaced args; fix is to the encoding
   (escape or length-prefix over vsock), keeping `strparse.c` as the
   cmdline-fallback decoder.
8. **ADR-023 cleanup.** The slirp constants (10.0.2.15/2.3) are
   hardcoded in `init.c`; when bridge networking lands, route
   per-container config through the existing `podvm.*=` cmdline/vsock
   channel.
9. **Bigger design items (deliberate future work):** ADR-021 FreeBSD
   platform, ADR-020 guest overlay module.
