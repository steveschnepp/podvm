# Guest Kernel: What to Strip

> Making the ~2MB kernel even smaller.

---

## Current Config Analysis

| Category | Config | Size Impact | Verdict |
|----------|--------|-------------|---------|
| **CPU** | | | |
| | `CONFIG_SMP=y` | +100KB | Keep (multi-core) |
| | `CONFIG_NR_CPUS=8` | +20KB | Reduce to 4 |
| **Paravirt** | | | |
| | `CONFIG_HYPERVISOR_GUEST=y` | 0 | Keep |
| | `CONFIG_PARAVIRT=y` | 0 | Keep |
| | `CONFIG_KVM_GUEST=y` | 0 | Keep |
| **Virtio** | | | |
| | `CONFIG_VIRTIO=y` | 0 | Keep |
| | `CONFIG_VIRTIO_PCI=y` | +50KB | Keep (primary bus) |
| | `CONFIG_VIRTIO_MMIO=y` | +30KB | **STRIP** (PCI sufficient) |
| | `CONFIG_VIRTIO_BLK=y` | +20KB | **STRIP** (no block devices) |
| | `CONFIG_VIRTIO_CONSOLE=y` | +10KB | Keep (serial) |
| | `CONFIG_VIRTIO_FS=y` | +15KB | Keep (rootfs) |
| | `CONFIG_VIRTIO_NET=y` | +25KB | **STRIP** (no guest networking) |
| | `CONFIG_VIRTIO_RNG=y` | +5KB | **STRIP** (not critical) |
| | `CONFIG_VIRTIO_BALLOON=y` | +10KB | **STRIP** (no memory ballooning) |
| **Vsock** | | | |
| | `CONFIG_VSOCK=y` | +20KB | **STRIP** (use cmdline for now) |
| | `CONFIG_VIRTIO_VSOCKETS_COMMON=y` | +5KB | **STRIP** |
| **Filesystem** | | | |
| | `CONFIG_TMPFS=y` | +15KB | Keep (/tmp, /run) |
| | `CONFIG_TMPFS_XATTR=y` | +3KB | Keep (permissions) |
| | `CONFIG_PROC_FS=y` | 0 | Keep (/proc/cmdline) |
| | `CONFIG_SYSFS=y` | 0 | Keep (/sys) |
| | `CONFIG_DEVTMPFS=y` | +10KB | Keep (/dev) |
| | `CONFIG_DEVTMPFS_MOUNT=y` | +2KB | Keep (auto-mount) |
| **Console** | | | |
| | `CONFIG_TTY=y` | +20KB | Keep |
| | `CONFIG_VT=y` | +30KB | **STRIP** (use serial only) |
| | `CONFIG_VT_CONSOLE=y` | +10KB | **STRIP** |
| | `CONFIG_SERIAL_8250=y` | +10KB | Keep |
| | `CONFIG_SERIAL_8250_CONSOLE=y` | +5KB | Keep |
| **Kernel** | | | |
| | `CONFIG_PRINTK=y` | 0 | Keep |
| | `CONFIG_BUG=y` | +5KB | **STRIP** (optional) |
| | `CONFIG_FUTEX=y` | +5KB | Keep (needed by musl) |
| | `CONFIG_EPOLL=y` | +5KB | Keep (needed by musl) |
| | `CONFIG_SIGNALFD=y` | +2KB | Keep (needed by musl) |
| | `CONFIG_TIMERFD=y` | +2KB | **STRIP** (optional) |
| | `CONFIG_EVENTFD=y` | +2KB | Keep (needed by musl) |
| | `CONFIG_AIO=y` | +10KB | **STRIP** (not needed) |
| **Compression** | | | |
| | `CONFIG_CRYPTO_LZ4=y` | +10KB | Keep (one is enough) |
| | `CONFIG_CRYPTO_ZSTD=y` | +20KB | **STRIP** |
| | `CONFIG_CRYPTO_GZIP=y` | +15KB | **STRIP** |

---

## Proposed Minimal Config

```bash
# ──── Processor ────
CONFIG_SMP=y
CONFIG_NR_CPUS=4              # Reduced from 8
CONFIG_X86_64=y

# ──── Paravirtualization ────
CONFIG_HYPERVISOR_GUEST=y
CONFIG_PARAVIRT=y
CONFIG_KVM_GUEST=y

# ──── Virtio Drivers (built-in) ────
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y           # Primary bus
# CONFIG_VIRTIO_MMIO is not set    # STRIP
# CONFIG_VIRTIO_BLK is not set     # STRIP
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_FS=y
# CONFIG_VIRTIO_NET is not set     # STRIP
# CONFIG_VIRTIO_RNG is not set     # STRIP
# CONFIG_VIRTIO_BALLOON is not set # STRIP

# ──── Vsock ────
# CONFIG_VSOCK is not set           # STRIP
# CONFIG_VIRTIO_VSOCKETS_COMMON is not set  # STRIP

# ──── Filesystems ────
CONFIG_TMPFS=y
CONFIG_TMPFS_XATTR=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y

# ──── Console ────
CONFIG_TTY=y
# CONFIG_VT is not set              # STRIP
# CONFIG_VT_CONSOLE is not set      # STRIP
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y

# ──── Essential ────
CONFIG_PRINTK=y
# CONFIG_BUG is not set             # STRIP
CONFIG_FUTEX=y
CONFIG_EPOLL=y
CONFIG_SIGNALFD=y
# CONFIG_TIMERFD is not set         # STRIP
CONFIG_EVENTFD=y
# CONFIG_AIO is not set             # STRIP

# ──── Compression ────
CONFIG_CRYPTO_LZ4=y
# CONFIG_CRYPTO_ZSTD is not set     # STRIP
# CONFIG_CRYPTO_GZIP is not set     # STRIP

# ──── NOT needed ────
# CONFIG_OVERLAY_FS is not set
# CONFIG_EXT4_FS is not set
# CONFIG_BTRFS_FS is not set
# CONFIG_XFS_FS is not set
# CONFIG_NFS_FS is not set
# CONFIG_CIFS is not set
# CONFIG_ATA is not set
# CONFIG_SCSI is not set
# CONFIG_USB is not set
# CONFIG_SOUND is not set
# CONFIG_DRM is not set
# CONFIG_WLAN is not set
# CONFIG_BLUETOOTH is not set
# CONFIG_INFINIBAND is not set
```

---

## Size Savings

| Item | Removed | Savings |
|------|---------|---------|
| VIRTIO_MMIO | Device bus | ~30KB |
| VIRTIO_BLK | Block driver | ~20KB |
| VIRTIO_NET | Network driver | ~25KB |
| VIRTIO_RNG | RNG driver | ~5KB |
| VIRTIO_BALLOON | Balloon driver | ~10KB |
| VSOCK | IPC | ~25KB |
| VT/VT_CONSOLE | Virtual terminal | ~40KB |
| BUG | Debug assertions | ~5KB |
| TIMERFD | Timer FDs | ~2KB |
| AIO | Async I/O | ~10KB |
| ZSTD/GZIP | Compression | ~35KB |
| NR_CPUS 8→4 | CPU slots | ~20KB |
| **Total** | | **~227KB** |

**Before**: ~2MB  
**After**: ~1.8MB (estimated)

---

## What's Lost

| Feature | Impact | Mitigation |
|---------|--------|------------|
| Network in guest | No apt/curl | Host-side only |
| Block devices | No local disk | virtiofs only |
| Memory ballooning | Fixed memory | Overcommit OK |
| Vsock | No IPC | Cmdline only |
| VT console | No graphical | Serial only |
| Multiple compression | Only LZ4 | Rebuild initramfs |

---

## Three Profiles

### Profile: `minimal` (~1.5MB)

Strip everything non-essential:

```bash
# Only keep:
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_FS=y
CONFIG_TMPFS=y
CONFIG_PROC_FS=y
CONFIG_DEVTMPFS=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_FUTEX=y
CONFIG_EPOLL=y
CONFIG_SIGNALFD=y
CONFIG_EVENTFD=y
CONFIG_PRINTK=y
CONFIG_TTY=y
CONFIG_CRYPTO_LZ4=y
```

**Use case**: Simple commands, scripts, no network

### Profile: `network` (~1.7MB)

Add networking:

```bash
# Minimal +
CONFIG_VIRTIO_NET=y
CONFIG_INET=y
CONFIG_IPV6=y
CONFIG_PACKET=y
CONFIG_NET=y
CONFIG_SYSCTL=y
```

**Use case**: apt, curl, git

### Profile: `full` (~2MB)

Current config with all features.

**Use case**: Development, debugging

---

## Build Commands

```bash
# Minimal kernel
make defconfig
# Apply minimal config
scripts/config --disable VIRTIO_MMIO
scripts/config --disable VIRTIO_BLK
scripts/config --disable VIRTIO_NET
scripts/config --disable VIRTIO_RNG
scripts/config --disable VIRTIO_BALLOON
scripts/config --disable VSOCK
scripts/config --disable VT
scripts/config --disable VT_CONSOLE
scripts/config --disable BUG
scripts/config --disable TIMERFD
scripts/config --disable AIO
scripts/config --set-val NR_CPUS 4
scripts/config --disable CRYPTO_ZSTD
scripts/config --disable CRYPTO_GZIP
make olddefconfig
make -j$(nproc) bzImage
```

---

## Validation

After building, check size:

```bash
ls -la arch/x86/boot/bzImage
# Should be ~1.5-1.8MB

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
