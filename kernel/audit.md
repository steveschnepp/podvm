# Kernel Config Audit

> What else is hiding in there?

---

## Major Attack Surface Areas

### 1. BPF (CRITICAL - REMOVE)

BPF is a **VM inside the kernel** that runs untrusted code. Massive attack surface.

```
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_BPFILTER=y
CONFIG_BPF_EVENTS=y
```

**Remove all of these.** BPF has had dozens of CVEs. No reason for a container guest.

---

### 2. Networking Stack (REMOVE if no guest network)

If we removed VIRTIO_NET, why keep the network stack?

```
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IPV6=y
CONFIG_PACKET=y
CONFIG_UNIX=y
CONFIG_NET_SCHED=y
CONFIG_NETFILTER=y
```

**Remove unless guest needs network.** Even without a NIC, the stack code exists.

---

### 3. Namespaces (KEEP - needed for containers)

```
CONFIG_NAMESPACES=y
CONFIG_UTS_NS=y
CONFIG_IPC_NS=y
CONFIG_USER_NS=y
CONFIG_PID_NS=y
CONFIG_NET_NS=y
```

**Keep.** Containers need namespaces for isolation.

---

### 4. Capabilities (KEEP)

```
CONFIG_SECURITY=y
CONFIG_SECURITY_CAPABILITIES=y
```

**Keep.** Needed for proper privilege separation.

---

### 5. Seccomp (KEEP)

```
CONFIG_SECCOMP=y
CONFIG_SECCOMP_FILTER=y
```

**Keep.** Containers use seccomp to filter syscalls.

---

### 6. Modules (REMOVE)

```
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
```

**Remove.** Everything is built-in. No loading at runtime.

---

### 7. Tracing/Debug (REMOVE)

```
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
CONFIG_KPROBES=y
CONFIG_UPROBES=y
CONFIG_PERF_EVENTS=y
CONFIG_DEBUG_FS=y
CONFIG_KGDB=y
CONFIG_MAGIC_SYSRQ=y
```

**Remove all.** Debug features are attack surface.

---

### 8. Audit (REMOVE)

```
CONFIG_AUDIT=y
CONFIG_AUDITSYSCALL=y
```

**Remove.** Guest doesn't need audit subsystem.

---

### 9. Security Modules (REMOVE unless needed)

```
CONFIG_SECURITY_APPARMOR=y
CONFIG_SECURITY_SELINUX=y
CONFIG_SECURITY_YAMA=y
```

**Remove.** Not needed in minimal guest.

---

### 10. Power Management (REMOVE)

```
CONFIG_ACPI=y
CONFIG_PM=y
CONFIG_HIBERNATION=y
CONFIG_SUSPEND=y
```

**Remove.** VM doesn't need ACPI/suspend.

---

### 11. PCI (KEEP for virtio)

```
CONFIG_PCI=y
CONFIG_PCI_HOST_GENERIC=y
```

**Keep.** Required for virtio-pci.

---

### 12. Kernel Compression (CHANGE)

```
CONFIG_KERNEL_GZIP=y      # REMOVE
CONFIG_KERNEL_LZ4=y       # REMOVE
CONFIG_KERNEL_UNCOMPRESSED=y  # ADD (faster boot)
```

**Use uncompressed kernel** for faster boot.

---

## Recommended Additions to Config

```bash
# ──── Security hardening ────
CONFIG_CC_STACKPROTECTOR_STRONG=y
CONFIG_RANDOMIZE_BASE=y           # KASLR
CONFIG_INIT_STACK_ALL_ZERO=y
CONFIG_HARDENED_USERCOPY=y
CONFIG_FORTIFY_SOURCE=y

# ──── Namespaces (for containers) ────
CONFIG_NAMESPACES=y
CONFIG_UTS_NS=y
CONFIG_IPC_NS=y
CONFIG_USER_NS=y
CONFIG_PID_NS=y
CONFIG_NET_NS=y

# ──── Seccomp ────
CONFIG_SECCOMP=y
CONFIG_SECCOMP_FILTER=y

# ──── Capabilities ────
CONFIG_SECURITY=y
CONFIG_SECURITY_CAPABILITIES=y

# ──── Kernel ────
CONFIG_KERNEL_UNCOMPRESSED=y
```

---

## Summary: What to REMOVE

| Category | Configs | Reason |
|----------|---------|--------|
| BPF | BPF, BPF_SYSCALL, BPF_JIT, etc. | VM inside kernel |
| Networking | NET, INET, IPV6, PACKET, etc. | No guest network |
| Modules | MODULES, MODULE_UNLOAD | Built-in only |
| Tracing | FTRACE, KPROBES, PERF_EVENTS | Debug surface |
| Audit | AUDIT, AUDITSYSCALL | Not needed |
| Security modules | APPARMOR, SELINUX, YAMA | Not needed |
| Power | ACPI, PM, SUSPEND | VM doesn't need |
| Debug | DEBUG_FS, KGDB, MAGIC_SYSRQ | Attack surface |

---

## Summary: What to KEEP

| Category | Configs | Reason |
|----------|---------|--------|
| Virtio | VIRTIO_PCI, CONSOLE, FS, BALLOON | Device access |
| Vsock | VSOCK, VIRTIO_VSOCKETS | Host↔guest IPC |
| Namespaces | NAMESPACES, UTS/IPC/USER/PID/NET_NS | Container isolation |
| Seccomp | SECCOMP, SECCOMP_FILTER | Syscall filtering |
| Capabilities | SECURITY, SECURITY_CAPABILITIES | Privilege control |
| Filesystems | TMPFS, PROC, SYSFS, DEVTMPFS | Essential mounts |
| Console | TTY, SERIAL_8250 | Output |
| Musl | FUTEX, EPOLL, SIGNALFD, EVENTFD | Libc requirements |
| Security | CC_STACKPROTECTOR, RANDOMIZE_BASE | Hardening |
