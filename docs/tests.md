# Tests

> Test suite for podvm POC.

---

## Running Tests

```bash
./tests/run_tests.sh
```

Unit tests only (fast, no VM):

```bash
make -C tests all && make -C tests test
```

## Test Structure

```
tests/
├── Makefile              # Builds every test_*.c binary
├── run_tests.sh          # Test runner: unit -> build -> deps -> kernel -> e2e
├── cli_matrix.sh         # Drives the REAL binary's CLI (no VM)
├── test_config.c         # strparse: kv/csv parsing + ADR-025 escaping
├── test_vsock.c          # Vsock socket/address plumbing
├── test_kernel_config.c  # kernel/config validation (run from repo root)
├── test_image.c          # OCI manifest/config parse, zstd, includes json.c
├── test_sha256.c         # FIPS 180-4 known-answer tests
├── test_imgcfg.c         # imgcfg_fold: docker command/env/cwd/user semantics
├── test_userdb.c         # /etc/passwd+group parser, docker user-spec matrix
├── test_container.c      # name validation, state dirs, rm -rf, flock guard
├── test_build.c          # Dockerfile parser
├── test_json.c           # json.c DOM parser contract
├── test_store.c          # podman storage reader (fixture tree via XDG_DATA_HOME)
└── e2e_*.sh / .py        # End-to-end tests; most boot a real VM
```

## Unit Tests

### test_config.c
- `kv_find()` / `kv_copy_value()` - key=value lookup, escape-aware
- `csv_parse()` - comma-separated values, unescape-aware
- `kv_escape()` / `kv_unescape()` - ADR-025 round-trips

### test_vsock.c
- Socket creation, address construction, server/client communication

### test_kernel_config.c
- Required options present, forbidden options absent, security options on.
- Run from the repo root (it reads `kernel/config` relatively).

### test_image.c
- OCI manifest + config blob parse, digest verification, zstd/gzip layers.
- Compiles `../src/image.c` directly so static helpers are reachable.

### test_sha256.c
- Known-answer vectors for the hand-rolled streaming SHA-256.

### test_imgcfg.c
- `imgcfg_fold()`: Entrypoint vs Cmd vs user command, env merge order,
  cwd/user precedence. Extracted from `main.c` so it is testable without a VM.

### test_userdb.c
- `userdb_resolve()`: numeric uid:gid, bare uid, named user, name:gid,
  name:group, uid:group, plus unresolvable-name fallback.

### test_container.c
- `container_name_valid()`, `container_state_dir()`, `container_rm_rf()`
  (including a mode-000 directory and a symlink), `container_lock()` /
  `container_locked()`.

### test_build.c
- Dockerfile parser: FROM/RUN/COPY/ENV, shell and JSON forms, error cases.

### test_json.c
- `json_parse()`: object/array access, escape + `\uXXXX` decoding,
  numbers/bool/null, rejection of malformed input.

### test_store.c
- `store_find()` / `store_chain()` / `store_image_config()` against a
  temp fixture tree (`XDG_DATA_HOME`), so no podman or VM is needed.
  Pins the `localhost/<name>:<tag>` matching and the big-data base64
  filename encoding.

## CLI Tests (no VM)

### cli_matrix.sh
Drives the real `build/podvm` binary. Every case uses a nonexistent
path positional so it fails at image resolution *after* getopt, which
makes a dropped getopt letter show up as `invalid option`. Also covers
`--help`, missing option arguments, per-flag host-side validation,
detach rules, and subcommand argument errors.

## End-to-End Tests

Most of these boot a real VM (KVM required) and assert inside the guest.

| Script | What it proves |
|--------|----------------|
| `e2e_boot.sh` | 9p mount -> chroot -> exec -> clean exit |
| `e2e_podman.sh` | podman-storage resolution path |
| `e2e_net.sh` | DNS + HTTPS out of the guest |
| `e2e_imgcfg.sh` | the image's own Cmd/Env with no CLI command |
| `e2e_native.sh` | podvm's own registry client (PODVM_RESOLVE=native) |
| `e2e_interactive.py` | `-i` delivers a raw Ctrl-A to the guest (pty) |
| `e2e_writeback.sh` | `--name` persists the writable layer across runs |
| `e2e_rm.sh` | `rm`/`--rm` and the live-container flock guard |
| `e2e_runflags.sh` | podman-parity flags reach the guest (-e/-u/-w/-p/...) |
| `e2e_build.sh` | Dockerfile -> runnable cached image (RUN = real VMs) |
| `e2e_detach.sh` | `-d` + `stop` + `logs` lifecycle |
| `e2e_images.sh` | `images`/`rmi` with the shared/exclusive lock |

`e2e_images.sh` deletes cached alpine on purpose and must stay last.

## Test Results

Current status:
- 30 PASS
- 1 WARN (missing musl-gcc, pre-existing)
- 0 FAIL

## Adding Tests

1. Pure logic: put it in a `src/*.c` module with its own `test_*.c`.
2. Anything touching QEMU/guest/devices: an `e2e_*.sh` that boots a VM.
3. Wire it into `tests/Makefile` (`TESTS`) and `tests/run_tests.sh`.
