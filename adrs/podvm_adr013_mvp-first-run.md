# ADR-013: MVP Scope - First Successful Run

**Date:** 2024-09-29

**Status:** Accepted

## Context

podvm has many architectural decisions documented (ADR-001 through ADR-012). The design is clear. What remains is defining the minimum viable product: the smallest increment of work that proves the architecture works end-to-end.

The MVP must demonstrate that a container image can be pulled from a registry, run inside a VM, and produce output. This validates the entire pipeline: image resolution, overlay assembly, VM launch, virtiofs mount, guest init, and process execution.

Without a working end-to-end flow, further development risks building components that do not integrate correctly. The MVP de-risks the architecture by proving each layer works together.

## Decision

The MVP is defined by a single successful test:

```
podvm run -it debian echo hello world
```

This command must:
1. Pull the debian:latest image from docker.io (if not cached locally)
2. Extract OCI layers to container storage
3. Assemble overlay filesystem on the host
4. Launch a VM with the synthetic kernel and guest init
5. Mount the container rootfs via virtiofs
6. Chroot into the container
7. Execute `echo hello world`
8. Output "hello world" to the terminal
9. Exit with status 0
10. Clean up the VM and temporary storage

## Rationale

`debian` is chosen as the test image because it is small (~75MB), widely used, and guaranteed to exist on Docker Hub. It is not a minimal Alpine image (which might hide issues with glibc) nor a complex application image (which might introduce unrelated failures).

`echo hello world` is the simplest possible command. It requires no application runtime, no network access, no file I/O beyond stdout. If this fails, the problem is in the podvm infrastructure, not the container content.

The `-it` flag tests TTY allocation and interactive mode. Without it, stdout might be buffered differently. This flag ensures the full stdio path works through the serial console.

Pulling from docker.io validates registry integration. The image service must handle HTTPS, manifest parsing, layer download, and content-addressable storage. Using a public registry avoids authentication complexity.

Exit status 0 is critical. The host must correctly capture the guest process exit code and propagate it. This validates the lifecycle management chain: VM launch, process execution, exit detection, and cleanup.

## Implications

The MVP requires implementing all core components simultaneously. The image service, overlay manager, VM launcher, and guest init must all work. This is a large initial commit but ensures integration from day one.

External dependencies for the MVP:
- Cloud Hypervisor binary installed on the host
- KVM available (/dev/kvm)
- Root privileges (or appropriate capabilities for KVM and networking)
- Network access to docker.io
- musl-gcc for building the guest init
- Linux kernel source for building the synthetic kernel

The MVP does not require:
- Daemon (podvmd) - CLI can manage VMs directly
- Bridge networking - not needed for `echo hello world`
- Port forwarding - not needed
- Volume mounts - not needed
- `podvm exec` - not needed
- `podvm build` - not needed (only build needs layer commits)
- Multiple containers - not needed
- Memory ballooning - not needed for single echo command
- vsock IPC - only needed for build/exec
- Guest kernel module - only needed for build

The MVP can run without a daemon. The CLI starts the VM, waits for exit, and cleans up. This simplifies initial development. The daemon can be added later for state management.

## Success Criteria

```
$ podvm run -it debian echo hello world
hello world
$ echo $?
0
```

Timing targets:
- Image pull (first run): <10s on fast connection
- Image pull (cached): <100ms
- VM boot: <200ms
- Process execution: <100ms
- Total (cached): <500ms

## MVP File Structure

```
podvm/
├── src/
│   ├── main.c                   # CLI entry point + argument parsing
│   ├── image.c                  # registry client + layer download
│   ├── image.h
│   ├── storage.c                # container storage (read Podman format)
│   ├── storage.h
│   ├── overlay.c                # overlay assembly
│   ├── overlay.h
│   ├── vm.c                     # Cloud Hypervisor launcher
│   ├── vm.h
│   ├── json.c                   # minimal JSON parser
│   └── json.h
├── guest/
│   ├── init.c                   # guest init (musl static)
│   └── Makefile
├── kernel/
│   └── config                   # synthetic kernel config
├── adrs/                        # architecture decisions
├── SPEC.md                      # full specification
└── Makefile                     # top-level build (C99, musl-gcc)
```

## MVP Implementation Order

1. Build guest init (guest/init.c) - musl static
2. Build synthetic kernel (kernel/config)
3. Implement image pull (src/image.c) - download debian layers
4. Implement storage reader (src/storage.c) - read layer paths
5. Implement overlay assembly (src/overlay.c) - mount overlay
6. Implement VM launcher (src/vm.c) - start Cloud Hypervisor
7. Implement CLI (src/main.c) - wire everything together
8. Test: podvm run -it debian echo hello world

## Build

```bash
# Host tools
mkdir build && cd build
cmake ..
make

# Guest init
cd guest
make
```

CMakeLists.txt handles dependency detection (libcurl, etc.) automatically.

## Dependencies

Host tools (dynamic):
- libc (glibc or musl, host system)
- libcurl (host package)

Guest init (static):
- musl (statically linked, no runtime deps)

## Alternatives Considered

**Alpine as test image:** Rejected. Alpine uses musl, which matches the guest init. This could mask linking issues. Debian uses glibc, which is a better test of the chroot environment.

**No network pull:** Considered using a pre-loaded image. Rejected because registry integration is a core feature. The MVP should prove pull works.

**QEMU as fallback:** The MVP targets Cloud Hypervisor only. QEMU support adds configuration complexity. It can be added after the MVP validates the architecture.

**Daemon from day one:** Rejected for MVP. The daemon adds state management, socket communication, and lifecycle complexity. A stateless CLI is simpler for the first test.
