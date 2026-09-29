# Mission Log: podvm Design Session

**Date:** 2024-09-29
**Context:** Initial design session for podvm -- a container runtime that replaces Linux namespaces with full VMs, using identical OCI images as Podman/Docker.

---

## What We Did

### Phase 1: Initial Concept

Started with the question: can we build a Docker/Podman-compatible runtime using full VMs instead of cgroups? The idea was to provide container UX with VM isolation.

First sketch: QEMU + virtiofs for filesystem sharing, custom guest userspace for overlay management. The initial design had the guest assembling overlays from individual layer directories.

### Phase 2: Docker Desktop Insight

Key insight: Docker Desktop on macOS already does exactly this. It runs containers inside a Linux VM. The difference is Docker Desktop includes a full container runtime (containerd + runc) inside the VM because it must emulate a Linux host.

podvm eliminates that layer. The host already is Linux, so the VM only needs to execute a process. This simplifies the guest dramatically.

### Phase 3: Minimal Guest

Iterated on guest minimalism:

- Started with busybox (~487KB) -- too large
- Moved to musl static (~7KB) -- good
- Considered raw syscalls (~2KB) -- too painful to develop
- Settled on musl static as the right balance

The guest init does only: mount virtiofs, chroot, exec. No container runtime, no namespace setup, no cgroup management.

### Phase 4: Host-Side Overlay Assembly

Changed from guest-side to host-side overlay assembly. The host already knows layer order from container storage. It assembles the overlay and shares the merged directory via virtiofs. The guest sees a complete filesystem, not individual layers.

This simplified the guest init further and removed the need for overlayfs in the guest kernel.

### Phase 5: OCI Image Compatibility

Decided to share Podman's container storage directly. No image conversion. podvm reads the same layer directories that Podman writes. Images pulled by Podman are immediately available to podvm and vice versa.

### Phase 6: Kernel and VMM Selection

Explored several options:

- Cloud Hypervisor vs QEMU vs Firecracker
- Decided on QEMU for MVP: ubiquitous, battle-tested, no extra installation
- Cloud Hypervisor can be added later as an alternative

Kernel config: synthetic, virtio-only, ~30 config options, ~2-3MB.

### Phase 7: Memory Management

Identified memory efficiency as the key challenge. Without ballooning, VMs waste more memory than containers.

Two solutions:
1. virtio-balloon: dynamic memory reclamation
2. DAX virtiofs: deduplicate shared layers across VMs

Together, VM memory efficiency approaches container efficiency.

### Phase 8: IPC and Build Support

Realized build (podvm build) requires host-guest communication for layer commits. Each RUN instruction needs the host to freeze the upper layer and create a new OCI layer.

Decided on vsock for structured IPC (exec, commit, signal). Serial console remains for stdout/stderr.

### Phase 9: PID Namespace and Guest Module

Two final insights:

1. PID namespace: guest init creates a PID namespace so the entrypoint appears as PID 1, matching real container behavior.

2. Guest kernel module (podvm-overlay.ko): handles overlay assembly and dirty tracking inside the guest. Since the guest kernel is our synthetic kernel, we can build the module directly into it. This enables transparent layer commits during build.

---

## What We Learned

### Technical

- **Key insight: only build creates layers**: `docker run` only needs ONE writable upper layer. Lower layers are read-only and never change. This means `podvm run` needs no IPC, no guest module, no layer commits. Just mount and go. vsock and guest module are only needed for `podvm build`.

- **Docker Desktop pattern works on Linux**: The VM-per-container model is proven on macOS. On Linux, it eliminates the redundant container runtime layer.

- **virtiofs vs 9p**: virtiofs provides ~80% native performance vs ~50% for 9p. The virtiofsd daemon is required but is a single binary from the QEMU package.

- **QEMU virtiofs setup**: Requires memory-backend-memfd with share=on and vhost-user-fs-pci device. More verbose than Cloud Hypervisor but works.

- **Memory ballooning**: virtio-balloon is essential for VM density. Without it, each VM locks its full memory allocation regardless of usage.

- **DAX for layer deduplication**: virtiofs with DAX maps files directly into guest address space without page cache duplication. Multiple VMs sharing the same layer use one copy in host memory.

- **QMP**: QEMU Machine Protocol provides JSON-based control for balloon management, VM lifecycle, and status queries. One socket per VM.

- **PID namespace is the only namespace needed**: The VM provides all other isolation (mount, network, user). Only PID namespace is needed for the PID 1 convention.

### Process

- **First principles thinking**: Starting from "what is a container?" (a process with a different filesystem view) led to simpler design than starting from "how does Docker work?"

- **Iterative simplification**: Each iteration removed complexity. Guest went from busybox + overlay assembly to musl static + chroot. Host went from complex to: assemble overlay, share via virtiofs, start QEMU.

- **ADRs before code**: Writing ADRs for each decision forced clear reasoning and documented alternatives considered. This prevented scope creep and kept the design focused.

---

## What We Decided

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Isolation boundary | VM (KVM) | Stronger than namespaces, eliminates namespace bugs |
| Guest init | musl static (~7KB) | Small, standard C, no dependencies |
| Kernel | Synthetic (virtio-only, ~2MB) | Minimal, fast boot, no real hardware drivers |
| VMM | QEMU (primary) | Ubiquitous, battle-tested, no extra install |
| Filesystem sharing | virtiofs via virtiofsd | ~80% native performance |
| Memory management | virtio-balloon + DAX | Container-level memory efficiency |
| Image compatibility | Share Podman storage | Zero conversion, shared ecosystem |
| Host-side overlay | Host assembles, guest sees merged | Simplifies guest, no overlayfs in guest |
| IPC | vsock | Purpose-built for host-guest, bidirectional |
| PID 1 illusion | PID namespace | Only namespace needed, matches container semantics |
| Overlay management | Guest kernel module | Our kernel, dirty tracking, layer commits |
| Build system | CMake | Standard C build |

---

## Rules Added

- **Guest is minimal**: If it can be done on the host, do it on the host. The guest only executes processes.

- **Shared storage**: Don't create separate image stores. Read Podman's storage directly.

- **No redundant runtimes**: Don't put containerd/runc inside the VM. The VM IS the runtime.

- **Balloon everything**: Every VM gets virtio-balloon. Memory efficiency is non-negotiable.

- **DAX for read-only layers**: Lower layers are shared via DAX. No page cache duplication.

---

## What We'd Do Differently

- **Start with QEMU from the beginning**: Spent time on Cloud Hypervisor before settling on QEMU. QEMU is simpler for MVP because it's already installed.

- **Write ADRs earlier**: The architecture document (SPEC.md) was written before the ADRs. The ADRs forced better decisions. Should have started with ADRs.

- **Test with a real image earlier**: The MVP test (podvm run -it debian echo hello world) should be attempted before finalizing all design decisions. Some assumptions may be wrong.

---

## Files Changed

| File | Purpose |
|------|---------|
| `SPEC.md` | Full specification (needs update for QEMU, vsock, module) |
| `adrs/podvm_adr001_use-vms-for-isolation.md` | Core decision: VMs over namespaces |
| `adrs/podvm_adr002_host-side-overlay-assembly.md` | Host assembles overlay, not guest |
| `adrs/podvm_adr003_musl-static-guest-init.md` | Guest init is musl static |
| `adrs/podvm_adr004_cloud-hypervisor-primary.md` | (Superseded by ADR-018: QEMU) |
| `adrs/podvm_adr005_oci-image-compatibility.md` | Share Podman's container storage |
| `adrs/podvm_adr006_kernel-cmdline-configuration.md` | Pass config via kernel cmdline |
| `adrs/podvm_adr007_synthetic-kernel.md` | Minimal virtio-only kernel |
| `adrs/podvm_adr008_daemon-architecture.md` | Long-running podvmd daemon |
| `adrs/podvm_adr009_bridge-networking.md` | Bridge networking default |
| `adrs/podvm_adr010_no-guest-container-runtime.md` | No containerd/runc in guest |
| `adrs/podvm_adr011_container-storage-locking.md` | Respect Podman's storage locks |
| `adrs/podvm_adr012_serial-console-logging.md` | Serial console for stdout/stderr |
| `adrs/podvm_adr013_mvp-first-run.md` | MVP: podvm run -it debian echo hello world |
| `adrs/podvm_adr014_memory-balloon.md` | virtio-balloon for memory reclamation |
| `adrs/podvm_adr015_dax-virtiofs-deduplication.md` | DAX for shared layer deduplication |
| `adrs/podvm_adr016_vsock-ipc.md` | vsock for host-guest communication |
| `adrs/podvm_adr017_pid-namespace-for-pid1.md` | PID namespace for PID 1 convention |
| `adrs/podvm_adr018_qemu-primary-vmm.md` | QEMU as primary VMM |
| `adrs/podvm_adr019_virtiofs-with-qemu.md` | virtiofs via virtiofsd with QEMU |
| `adrs/podvm_adr020_guest-overlay-module.md` | Guest kernel module for overlay management |

---

## Next Steps

### MVP Implementation

1. Build guest init (guest/init.c) with musl-gcc
2. Build synthetic kernel (kernel/config)
3. Implement image pull (src/image.c) - download debian from docker.io
4. Implement storage reader (src/storage.c) - read Podman layer paths
5. Implement overlay assembly (src/overlay.c) - mount overlay on host
6. Implement VM launcher (src/vm.c) - start QEMU with virtiofs
7. Implement CLI (src/main.c) - wire everything together
8. Test: podvm run -it debian echo hello world

### Post-MVP

- vsock IPC for exec and build
- Guest kernel module (podvm-overlay.ko) for layer management
- Balloon management in daemon
- Bridge networking
- Port forwarding
- podvm build support
- Cloud Hypervisor alternative backend

### Open Questions

- How does virtiofsd lifecycle management work with QEMU? (start before QEMU, cleanup after)
- What happens when QEMU crashes? (daemon detects via QMP, cleans up)
- How to handle large images (>1GB layers) with virtiofs?
- Should the guest module use DAX for upper layer too?
