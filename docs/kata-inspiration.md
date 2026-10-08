# Kata Containers Inspiration

> What we can learn from Kata's 8+ years of VM-based container runtime development.

---

## Architecture Comparison

| Aspect | Kata Containers | podvm (Current) | podvm (Proposed) |
|--------|-----------------|-----------------|------------------|
| Guest process | Long-running agent | One-shot init | Minimal init + vsock |
| Communication | vsock + ttRPC | Kernel cmdline | vsock (simple protocol) |
| Hypervisors | QEMU, CH, Firecracker, Dragonball | QEMU only | QEMU primary, extensible |
| Networking | CNM/CNI via netns | None | Tap + bridge |
| Storage | virtiofs, 9p, block, NBD | virtiofs only | virtiofs primary |
| Config format | TOML | Kernel cmdline | Kernel cmdline + vsock |
| OCI images | Full support via containerd | None (raw rootfs) | Overlay assembly |

---

## Key Learnings

### 1. Communication: vsock > Kernel Cmdline

**Kata's approach**: Uses `virtio-vsock` for bidirectional host↔guest communication.

```
Host (runtime) ←→ vsock ←→ Guest (agent)
```

**Why it's better**:
- Kernel cmdline is limited (~4KB) and one-directional
- vsock supports arbitrary messages, streaming, error handling
- No need to encode complex config in cmdline

**Our takeaway**: Use vsock for:
- Container lifecycle commands (exec, stop, signal)
- Environment variables (can be large)
- File descriptors (for -it mode)
- Exit code reporting

**Implementation**:
```c
// Guest side: simple vsock listener
int vsock_fd = socket(AF_VSOCK, SOCK_STREAM, 0);
struct sockaddr_vm addr = {
    .svm_family = AF_VSOCK,
    .svm_cid = VMADDR_CID_ANY,  // listen on any CID
    .svm_port = 1234,           // our port
};
bind(vsock_fd, (struct sockaddr*)&addr, sizeof(addr));
listen(vsock_fd, 1);
```

### 2. Agent vs One-Shot Init

**Kata's approach**: Long-running agent that:
- Handles multiple containers per VM (pods)
- Manages container lifecycle
- Provides exec, attach, signal, port-forward
- Reports status back to host

**Our approach**: One-shot init that:
- Runs single command
- Exits when command exits
- Simple, minimal attack surface

**Trade-offs**:

| Factor | Agent | One-Shot |
|--------|-------|----------|
| Complexity | High (~50K LOC) | Low (~500 LOC) |
| Startup | Slower (agent init) | Faster |
| Multi-container | Yes (pods) | No |
| Exec in running | Yes | No (need separate VM) |
| Security | Larger attack surface | Minimal |

**Our takeaway**: Keep one-shot init for MVP, but design for future agent support:
- Use vsock even for one-shot (future-proof)
- Keep protocol simple so it can evolve to agent

### 3. Hypervisor Abstraction

**Kata's approach**: Abstract hypervisor behind an interface:

```go
type Hypervisor interface {
    CreateVM(ctx context.Context, config *VMConfig) (*VM, error)
    StopVM(ctx context.Context, wait bool) error
    AddDevice(ctx context.Context, device *config.Resource) error
    // ...
}
```

**Our takeaway**: Define a hypervisor interface early:

```c
typedef struct Hypervisor {
    const char *name;
    int  (*start)(struct Hypervisor *h, VMConfig *cfg);
    int  (*stop)(struct Hypervisor *h, int wait);
    int  (*add_device)(struct Hypervisor *h, Device *dev);
    void (*cleanup)(struct Hypervisor *h);
} Hypervisor;

// QEMU implementation
extern Hypervisor qemu_hypervisor;
```

This allows adding Cloud Hypervisor or Firecracker later without rewriting.

### 4. Memory Backend for virtiofs

**Kata's approach** (from qemu.go):
```
-object memory-backend-memfd,id=mem,size=SIZE,share=on
-numa node,memdev=mem
```

**Our current code**: Hardcoded `128M`:
```c
"-object", "memory-backend-memfd,id=mem,size=128M,share=on",
```

**Fix**: Use config.memory:
```c
char mem_backend[64];
snprintf(mem_backend, sizeof(mem_backend),
    "memory-backend-memfd,id=mem,size=%dM,share=on", config.memory);
```

### 5. QEMU Machine Types

**Kata's approach**: Configurable machine type per arch:
- x86_64: `q35` (default)
- aarch64: `virt`
- ppc64le: `pseries`
- s390x: `s390x`

**Our takeaway**: Support machine type config:

```c
struct {
    const char *machine;  // "q35", "virt", etc.
    const char *cpu;      // "host", "max", etc.
    // ...
} config = {
    .machine = "q35",
    .cpu = "host",
};
```

### 6. Serial Console Handling

**Kata's approach**:
- Serial for debug logging
- Agent communicates via vsock (not serial)
- Console optional (for -it mode)

**Our approach**: Everything via serial (`-serial mon:stdio`)

**Improvement**: Separate debug console from data channel:

```c
// Debug output to serial
"-serial", "file:/var/log/podvm/guest.log",
// Interactive mode: use stdio for vsock
"-serial", "mon:stdio",  // only for -it
```

---

## Networking Inspiration

### Kata's Network Stack

```
Host Network Namespace
├── Bridge (cni0 / podvm0)
├── veth pairs per container
└── Tap device per VM ←→ Guest
    └── Guest Network Namespace
        ├── eth0 (virtio-net)
        └── Routes
```

### Our Proposed Network (Minimal)

```bash
# Host setup
ip link add podvm0 type bridge
ip addr add 172.16.0.1/24 dev podvm0
ip link set podvm0 up

# QEMU args
-netdev tap,id=net0,ifname=tap0,script=no,downscript=no
-device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56

# Guest config (via vsock or cmdline)
# podvm.ip=172.16.0.2/24 podvm.gateway=172.16.0.1
```

---

## Storage Inspiration

### Kata's Storage Layers

```
Container Layer (overlay upper)
    ↓
Container Mount Point
    ↓
 virtiofs / 9p / block device
    ↓
Host Filesystem (overlayfs merged)
```

### Our Current Storage

```
OCI Image Layers (host)
    ↓
OverlayFS Assembly (host)
    ↓
 virtiofs share
    ↓
Guest Mount Point
```

**Key insight from Kata**: Support multiple storage backends:

| Backend | Use Case | Performance |
|---------|----------|-------------|
| virtiofs | Default, shared filesystem | Good |
| 9p | Fallback, simpler | Slower |
| block device | High I/O workloads | Best |
| NBD | Network storage | Network-limited |

---

## Configuration Inspiration

### Kata's Config (TOML)

```toml
[hypervisor.qemu]
path = "/usr/bin/qemu-system-x86_64"
kernel = "/opt/kata/share/kata-containers/vmlinux.container"
image = "/opt/kata/share/kata-containers/kata-containers.img"
machine_type = "q35"
default_vcpus = 1
default_memory = 2048

[agent.kata]
kernel_modules = []
```

### Our Config (Kernel Cmdline + File)

**Current**: All via cmdline (limited to ~4KB)

**Proposed**: Hybrid approach:
- Critical params in cmdline (root, entrypoint)
- Extended config via vsock or config file in initramfs

```
# cmdline (minimal)
podvm.root=/rootfs podvm.init=/init

# config file (in initramfs or via vsock)
{
    "entrypoint": "/bin/sh",
    "args": ["-l"],
    "env": {"HOME": "/root", "PATH": "..."},
    "cwd": "/root",
    "user": "1000:1000"
}
```

---

## Action Items

### Phase 1: Quick Wins (from this analysis)

1. **Fix memory backend** - Use config.memory instead of hardcoded 128M
2. **Add machine type config** - Support q35/virt/etc
3. **Add vsock skeleton** - Even if not used yet, prepare for it
4. **Add network basic** - Tap device + bridge

### Phase 2: Kata-Inspired Features

1. **vsock protocol** - Define message format for host↔guest
2. **Hypervisor abstraction** - Interface for multiple VMMs
3. **Config file support** - TOML or JSON config
4. **Storage backends** - virtiofs, 9p, block

### Phase 3: Advanced (Future)

1. **Agent mode** - Optional long-running guest agent
2. **Pod support** - Multiple containers per VM
3. **CRI integration** - Kubernetes compatibility

---

## References

- [Kata Architecture](https://github.com/kata-containers/kata-containers/blob/main/docs/design/architecture/README.md)
- [Kata Virtualization](https://github.com/kata-containers/kata-containers/blob/main/docs/design/virtualization.md)
- [virtcontainers API](https://github.com/kata-containers/kata-containers/blob/main/src/runtime/virtcontainers/documentation/api/1.0/api.md)
- [Kata QEMU Config](https://github.com/kata-containers/kata-containers/blob/main/src/runtime/config/configuration-qemu.toml.in)
