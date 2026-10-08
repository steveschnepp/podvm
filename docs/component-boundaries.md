# Component Boundaries

> Clear separation of what lives where.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  HOST                                                            │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  podvm (CLI + daemon)                                       │ │
│  │  src/main.c                                                 │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  virtiofsd                                                  │ │
│  │  (external binary)                                          │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  QEMU                                                       │ │
│  │  (external binary)                                          │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
                              │
                    virtiofs │ vsock
                              │
┌─────────────────────────────┼───────────────────────────────────┐
│  VM                         │                                   │
│                             │                                   │
│  ┌──────────────────────────▼─────────────────────────────────┐ │
│  │  Synthetic Kernel                                           │ │
│  │  kernel/config → bzImage (~2MB)                             │ │
│  └──────────────────────────┬─────────────────────────────────┘ │
│                              │                                   │
│  ┌──────────────────────────▼─────────────────────────────────┐ │
│  │  /init (guest init)                                         │ │
│  │  guest/init.c → init (~5KB)                                 │ │
│  └──────────────────────────┬─────────────────────────────────┘ │
│                              │                                   │
│  ┌──────────────────────────▼─────────────────────────────────┐ │
│  │  Container Rootfs (via virtiofs)                            │ │
│  │  /rootfs/                                                   │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Component 1: HOST

### Location
```
src/main.c          # CLI + launcher
```

### Responsibilities

| Task | Description |
|------|-------------|
| CLI parsing | `podvm run [-it] <image> <cmd>` |
| Image handling | Pull/resolve OCI layers (future) |
| Overlay assembly | Mount overlayfs on host |
| virtiofsd mgmt | Start/stop virtiofs daemon |
| QEMU launch | Fork/exec QEMU with args |
| Signal handling | Cleanup on SIGINT/SIGTERM |
| Exit reporting | Forward guest exit code |

### Does NOT Do

- No filesystem setup inside guest
- No network configuration inside guest
- No process execution inside guest

### Current Code

```c
// src/main.c
int main(int argc, char **argv) {
    // Parse args
    // Validate rootfs directory
    // Start virtiofsd
    // Start QEMU
    // Wait for exit
    // Cleanup
}
```

### Future Additions

- OCI image pulling
- Overlay assembly
- Network bridge setup (tap devices)
- vsock communication
- Container lifecycle (stop, kill, exec)

---

## Component 2: GUEST KERNEL

### Location
```
kernel/config       # Kernel configuration
```

### Build
```bash
cd linux-x86_64
make defconfig
cp ../../kernel/config .config
make olddefconfig
make -j$(nproc) bzImage
cp arch/x86/boot/bzImage ../../kernel/bzImage
```

### What's Included

| Category | Config | Purpose |
|----------|--------|---------|
| **CPU** | `CONFIG_SMP=y`, `CONFIG_NR_CPUS=8` | Multi-core support |
| **Virtio** | `CONFIG_VIRTIO_PCI=y` | Device communication |
| | `CONFIG_VIRTIO_BLK=y` | Block devices |
| | `CONFIG_VIRTIO_CONSOLE=y` | Serial/console |
| | `CONFIG_VIRTIO_FS=y` | Shared filesystem |
| | `CONFIG_VIRTIO_NET=y` | Network |
| | `CONFIG_VIRTIO_RNG=y` | Random number |
| | `CONFIG_VIRTIO_BALLOON=y` | Memory management |
| **Vsock** | `CONFIG_VSOCK=y` | Host↔guest IPC |
| **Filesystem** | `CONFIG_TMPFS=y` | /tmp, /run |
| | `CONFIG_PROC_FS=y` | /proc |
| | `CONFIG_SYSFS=y` | /sys |
| | `CONFIG_DEVTMPFS=y` | /dev |
| **Console** | `CONFIG_SERIAL_8250_CONSOLE=y` | Debug output |
| | `CONFIG_VT_CONSOLE=y` | Virtual terminal |
| **Compression** | `CONFIG_CRYPTO_LZ4=y` | Initramfs decompression |
| | `CONFIG_CRYPTO_ZSTD=y` | Initramfs decompression |

### What's NOT Included

| Category | Reason |
|----------|--------|
| OverlayFS | Host handles this |
| Ext4/XFS/Btrfs | No local disks |
| ATA/SCSI/USB | No hardware |
| Sound/DRM | No GUI |
| WiFi/Bluetooth | No wireless |
| NFS/CIFS | No network filesystems |

### Size Target

~2MB compressed bzImage

---

## Component 3: GUEST INIT

### Location
```
guest/init.c        # Init source
guest/Makefile      # Build rules
guest/build-initramfs.sh  # Pack initramfs
```

### Build
```bash
cd guest
make                    # Builds init binary
./build-initramfs.sh    # Creates initramfs.cpio.gz
```

### Responsibilities

| Step | Action | Code |
|------|--------|------|
| 1 | Mount pseudo-filesystems | `mount("proc", "/proc", "proc", ...)` |
| 2 | Parse kernel cmdline | Read `/proc/cmdline` |
| 3 | Mount container rootfs | `mount(NULL, "/rootfs", "virtiofs", ...)` |
| 4 | Set hostname | `sethostname(...)` |
| 5 | Chroot | `chroot("/rootfs")` |
| 6 | Set working directory | `chdir(...)` |
| 7 | Switch user | `setuid()`, `setgid()` |
| 8 | Exec entrypoint | `execvp(entrypoint, argv)` |

### Does NOT Do

- No image pulling
- No overlay assembly
- No network setup (yet)
- No multi-container support
- No health checking

### Current Code Structure

```c
// guest/init.c

// Config from kernel cmdline
typedef struct {
    char root[PATH_SIZE];         // podvm.root=/rootfs
    char entrypoint[PATH_SIZE];   // podvm.entrypoint=/bin/sh
    char args[MAX_ARGS][PATH_SIZE];
    char envs[MAX_ENVS][PATH_SIZE];
    char cwd[PATH_SIZE];
    char hostname[256];
    uid_t uid;
    gid_t gid;
} Config;

int main(void) {
    mount_pseudofs();
    parse_config(&cfg);
    mount_rootfs(&cfg);
    chroot(cfg.root);
    execvp(cfg.entrypoint, argv);
}
```

### Size Target

~5KB compiled with musl static

---

## Communication Flow

```
HOST                                GUEST
──────                              ──────

1. Parse CLI args
   │
2. Start virtiofsd
   │
3. Start QEMU with:
   │  -kernel bzImage
   │  -initrd initramfs.cpio.gz
   │  -append "podvm.root=..."
   │  -chardev socket,path=virtiofs.sock
   │
   │                                    4. Kernel boots
   │                                       │
   │                                    5. /init starts
   │                                       │
   │                                    6. Mount virtiofs
   │                                       mount("/rootfs", "virtiofs")
   │                                       │
   │                                    7. Chroot + exec
   │                                       chroot("/rootfs")
   │                                       execvp("/bin/sh")
   │                                       │
   │                                    8. Container runs
   │                                       │
   │                                       │ (process exits)
   │                                       │
   │                                    9. init exits
   │                                       │
4. Waitpid returns
   │
5. Report exit code
```

---

## Current vs Proposed

| Aspect | Current | Proposed |
|--------|---------|----------|
| Host | CLI only | CLI + daemon |
| Kernel | Config defined | Same |
| Init | Monolithic | Modular |
| IPC | Kernel cmdline | vsock |
| Networking | None | Tap + bridge |
| Storage | virtiofs only | virtiofs + 9p + block |

---

## Files Summary

```
podvm/
├── src/
│   └── main.c              # HOST: CLI + launcher
├── kernel/
│   └── config              # GUEST KERNEL: build config
├── guest/
│   ├── init.c              # GUEST INIT: main source
│   ├── Makefile            # Build init binary
│   └── build-initramfs.sh  # Pack initramfs
├── CMakeLists.txt          # Host build
└── docs/
    └── ...
```
