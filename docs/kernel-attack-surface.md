# Guest Kernel: Attack Surface Reduction

> Security, not size. Every driver is a potential vulnerability.

---

## Principle

> "The most secure code is code that doesn't exist."

Every kernel feature is:
- Code that can have bugs
- Code that handles untrusted input
- Code that runs with kernel privileges

**Goal**: Minimize what's in the kernel to reduce what an attacker can exploit.

---

## Threat Model

| Attacker | Goal | Vector |
|----------|------|--------|
| Malicious container | Escape VM, hit host | Exploit kernel bug |
| Compromised entrypoint | Privilege escalation | Exploit syscall/driver |
| Crafted input | Kernel panic, DoS | Malformed virtio request |

**Assumption**: Container workload is UNTRUSTED.

---

## Attack Surface Analysis

### Virtio Drivers (HIGH RISK)

Each driver parses device requests from host. A malicious host or compromised virtiofsd could send crafted requests.

| Driver | Risk | Justification |
|--------|------|---------------|
| `VIRTIO_PCI` | **KEEP** | Required for any virtio device |
| `VIRTIO_MMIO` | **REMOVE** | Not needed with PCI; separate attack path |
| `VIRTIO_CONSOLE` | **KEEP** | Required for serial output |
| `VIRTIO_FS` | **KEEP** | Required for rootfs |
| `VIRTIO_BLK` | **REMOVE** | No block devices; remove driver code |
| `VIRTIO_NET` | **REMOVE** (initially) | No guest networking; remove driver code |
| `VIRTIO_RNG` | **REMOVE** | Not critical; remove attack surface |
| `VIRTIO_BALLOON` | **REMOVE** | Not using; remove driver code |

**Why remove even if "small"?**
- Each driver has interrupt handlers, DMA setup, memory mapping
- Bugs in virtio drivers have led to VM escapes (CVE-2020-xxxxx)
- Unnecessary code = unnecessary risk

### Vsock (MEDIUM RISK)

| Config | Risk | Justification |
|--------|------|---------------|
| `VSOCK` | **REMOVE** | Use kernel cmdline for now; vsock adds attack surface |
| `VIRTIO_VSOCKETS_COMMON` | **REMOVE** | Same |

**Future**: If vsock needed, audit carefully and minimize protocol.

### Filesystems (MEDIUM RISK)

Filesystem code parses superblocks, inodes, xattrs. Malicious images could trigger bugs.

| Filesystem | Risk | Justification |
|------------|------|---------------|
| `TMPFS` | **KEEP** | Simple, well-audited, needed for /tmp |
| `TMPFS_XATTR` | **REMOVE** | Unless explicitly needed; reduce features |
| `PROC_FS` | **KEEP** | Required for /proc/cmdline |
| `SYSFS` | **KEEP** | Required for /sys |
| `DEVTMPFS` | **KEEP** | Required for /dev |
| `DEVTMPFS_MOUNT` | **KEEP** | Convenience, low risk |
| `OVERLAY_FS` | **REMOVE** | Host handles overlay |
| `EXT4/XFS/BTRFS` | **REMOVE** | No local disks |
| `NFS/CIFS` | **REMOVE** | No network filesystems |

**Why remove TMPFS_XATTR?**
- Each feature is code
- xattr parsing has had bugs (CVE-2021-xxxxx)
- Only add back if needed

### Console/TTY (LOW-MEDIUM RISK)

| Config | Risk | Justification |
|--------|------|---------------|
| `TTY` | **KEEP** | Required for basic I/O |
| `VT` | **REMOVE** | Virtual terminal not needed; use serial |
| `VT_CONSOLE` | **REMOVE** | Same |
| `SERIAL_8250` | **KEEP** | Required for debug output |
| `SERIAL_8250_CONSOLE` | **KEEP** | Required |

**Why remove VT?**
- VT subsystem is complex (Unicode, selection, scrollback)
- Has had multiple CVEs
- Serial is simpler, less attack surface

### Kernel Core (MUST HAVE but audit)

| Config | Risk | Justification |
|--------|------|---------------|
| `PRINTK` | **KEEP** | Required for kernel messages |
| `BUG` | **REMOVE** | Debug assertions; production shouldn't use |
| `FUTEX` | **KEEP** | Required by musl libc |
| `EPOLL` | **KEEP** | Required by musl libc |
| `SIGNALFD` | **KEEP** | Required by musl libc |
| `EVENTFD` | **KEEP** | Required by musl libc |
| `TIMERFD` | **REMOVE** | Optional; reduce syscalls available |
| `AIO` | **REMOVE** | Not needed; reduce attack surface |

**Why remove TIMERFD?**
- Adds syscall surface
- Container workload can use alternatives
- Reduces what attacker can use

### Compression (LOW RISK but unnecessary)

| Config | Risk | Justification |
|--------|------|---------------|
| `CRYPTO_LZ4` | **KEEP** | One algorithm for initramfs |
| `CRYPTO_ZSTD` | **REMOVE** | Not needed |
| `CRYPTO_GZIP` | **REMOVE** | Not needed |

**Why remove?**
- Crypto code is complex
- Only need one decompressor
- Less code = less risk

---

## Revised Minimal Config

```bash
# ──── Processor ────
CONFIG_SMP=y
CONFIG_NR_CPUS=4
CONFIG_X86_64=y

# ──── Paravirtualization ────
CONFIG_HYPERVISOR_GUEST=y
CONFIG_PARAVIRT=y
CONFIG_KVM_GUEST=y

# ──── Virtio Drivers (built-in) ────
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
# CONFIG_VIRTIO_MMIO is not set    # ATTACK SURFACE
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_FS=y
# CONFIG_VIRTIO_BLK is not set     # ATTACK SURFACE
# CONFIG_VIRTIO_NET is not set     # ATTACK SURFACE
# CONFIG_VIRTIO_RNG is not set     # ATTACK SURFACE
# CONFIG_VIRTIO_BALLOON is not set # ATTACK SURFACE

# ──── Vsock ────
# CONFIG_VSOCK is not set              # ATTACK SURFACE
# CONFIG_VIRTIO_VSOCKETS_COMMON is not set  # ATTACK SURFACE

# ──── Filesystems ────
CONFIG_TMPFS=y
# CONFIG_TMPFS_XATTR is not set   # ATTACK SURFACE
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y

# ──── Console ────
CONFIG_TTY=y
# CONFIG_VT is not set             # ATTACK SURFACE
# CONFIG_VT_CONSOLE is not set     # ATTACK SURFACE
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y

# ──── Essential ────
CONFIG_PRINTK=y
# CONFIG_BUG is not set            # ATTACK SURFACE
CONFIG_FUTEX=y
CONFIG_EPOLL=y
CONFIG_SIGNALFD=y
# CONFIG_TIMERFD is not set        # ATTACK SURFACE
CONFIG_EVENTFD=y
# CONFIG_AIO is not set            # ATTACK SURFACE

# ──── Compression ────
CONFIG_CRYPTO_LZ4=y
# CONFIG_CRYPTO_ZSTD is not set    # ATTACK SURFACE
# CONFIG_CRYPTO_GZIP is not set    # ATTACK SURFACE
```

---

## What We Removed and Why

| Removed | Attack Surface Reason |
|---------|----------------------|
| `VIRTIO_MMIO` | Separate bus driver; PCI sufficient |
| `VIRTIO_BLK` | Parses block requests; no local disks |
| `VIRTIO_NET` | Parses network packets; no guest network |
| `VIRTIO_RNG` | Minor; remove if not critical |
| `VIRTIO_BALLOON` | Memory management; remove if fixed |
| `VSOCK` | IPC protocol; use cmdline instead |
| `TMPFS_XATTR` | xattr parsing bugs |
| `VT` | Complex terminal subsystem; CVE history |
| `VT_CONSOLE` | Same |
| `BUG` | Debug assertions; production shouldn't use |
| `TIMERFD` | Extra syscall surface |
| `AIO` | Async I/O; not needed |
| `ZSTD` / `GZIP` | Multiple decompressors; keep one |

---

## Future Additions (When Needed)

| Feature | When | Security Consideration |
|---------|------|------------------------|
| `VIRTIO_NET` | Guest needs network | Audit network driver carefully |
| `VSOCK` | Host↔guest IPC needed | Minimize protocol; audit |
| `TMPFS_XATTR` | Containers need xattrs | Accept risk or find alternative |

**Rule**: Don't add until needed. When adding, audit first.

---

## Validation

```bash
# Check what's enabled
grep "=y" .config | wc -l
# Goal: < 100 enabled options

# Compare to default defconfig
diff <(grep "=y" defconfig | sort) <(grep "=y" .config | sort)

# Boot test
qemu-system-x86_64 \
    -machine q35,accel=kvm \
    -cpu host \
    -m 128 \
    -kernel arch/x86/boot/bzImage \
    -initrd ../initramfs.cpio.gz \
    -append "podvm.root=/rootfs podvm.entrypoint=/bin/sh" \
    -nographic
```
