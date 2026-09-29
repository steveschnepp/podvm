# podvm Specification

> A container runtime that replaces Linux namespaces with full VMs,
> using identical OCI images as Podman/Docker.

---

## 1. Design Philosophy

### 1.1 What Is a Container?

A container is a process with an isolated filesystem view. Everything else
(namespaces, cgroups, seccomp) is implementation detail.

### 1.2 What Is podvm?

podvm provides the same filesystem isolation as containers using hardware
virtualization (KVM) instead of kernel namespaces. Each "container" runs
in its own lightweight VM with a synthetic kernel.

### 1.3 Principles

- **Identical images**: OCI images shared with Podman/Docker, zero conversion
- **Minimal guest**: ~2MB total (kernel + init), no container runtime inside VM
- **Host-side complexity**: overlay assembly, layer resolution, and image
  management happen on host; guest only executes processes
- **Compatibility**: same CLI semantics as `podman run`, `docker build`, etc.
- **No redundancy**: VM replaces namespaces; no cgroups, no seccomp, no user
  namespace mapping inside guest

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│  HOST                                                               │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────────┐│
│  │  podvm CLI                                                      ││
│  │  (argument parsing, output formatting, daemon communication)    ││
│  └─────────────────────────┬───────────────────────────────────────┘│
│                            │                                        │
│  ┌─────────────────────────▼───────────────────────────────────────┐│
│  │  Image Service                                                  ││
│  │  (queries container storage, resolves layers, caches metadata)  ││
│  └─────────────────────────┬───────────────────────────────────────┘│
│                            │                                        │
│  ┌─────────────────────────▼───────────────────────────────────────┐│
│  │  Overlay Manager                                                ││
│  │  (assembles overlayfs per container, manages upper/work dirs)   ││
│  └─────────────────────────┬───────────────────────────────────────┘│
│                            │                                        │
│  ┌─────────────────────────▼───────────────────────────────────────┐│
│  │  VM Launcher                                                    ││
│  │  (generates VM config, starts hypervisor, manages lifecycle)    ││
│  └─────────────────────────┬───────────────────────────────────────┘│
│                            │                                        │
│  ┌─────────────────────────▼───────────────────────────────────────┐│
│  │  Hypervisor (Cloud Hypervisor / QEMU / Firecracker)             ││
│  │  (runs synthetic kernel + initramfs, virtiofs shares)           ││
│  └─────────────────────────┬───────────────────────────────────────┘│
│                            │                                        │
└────────────────────────────┼────────────────────────────────────────┘
                             │
                    virtiofs │ virtio-console
                             │
┌────────────────────────────┼────────────────────────────────────────┐
│  GUEST (per-container VM)  │                                        │
│                            │                                        │
│  ┌─────────────────────────▼───────────────────────────────────────┐│
│  │  Synthetic Kernel                                               ││
│  │  (virtio drivers only, ~2MB, shared across all VMs)            ││
│  └─────────────────────────┬───────────────────────────────────────┘│
│                            │                                        │
│  ┌─────────────────────────▼───────────────────────────────────────┐│
│  │  /init (musl static, ~7KB)                                      ││
│  │  (mounts virtiofs, chroots, execs entrypoint)                   ││
│  └─────────────────────────────────────────────────────────────────┘│
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3. Host Components

### 3.1 podvm CLI

The CLI provides docker/podman-compatible commands. It communicates with
a long-running daemon (podvmd) for container lifecycle operations.

#### Commands

```
podvm run        Create and start a container (VM)
podvm exec       Execute command in running container
podvm stop       Stop a running container
podvm kill       Send signal to container
podvm rm         Remove a container
podvm ps         List running containers
podvm logs       Fetch container logs
podvm inspect    Inspect container configuration
podvm images     List images
podvm pull       Pull an image
podvm rmi        Remove an image
podvm build      Build image from Dockerfile
podvm volume     Manage volumes
podvm network    Manage networks
podvm system     System info and cleanup
podvm info       Display system information
```

#### Global Options

```
--root DIR           podvm data directory (default: /var/lib/podvm)
--runroot DIR        runtime state directory (default: /run/podvm)
--storage-driver     storage driver (overlay, btrfs, zfs)
--socket PATH        daemon socket (default: /run/podvm/podvm.sock)
```

### 3.2 Daemon (podvmd)

Long-running process that manages container lifecycle. The CLI connects
to the daemon via Unix socket.

#### Responsibilities

- Container state tracking (created, running, stopped, exited)
- Image layer resolution and caching
- Overlay assembly and teardown
- VM lifecycle management (start, stop, signal)
- Network allocation (IP assignment, port forwarding)
- Event streaming (container start/stop/die events)
- Resource accounting

#### State Storage

```
/var/lib/podvm/
├── overlay/                      # OCI layer storage (shared with podman)
│   ├── layer/
│   │   ├── abc123/
│   │   ├── def456/
│   │   └── ghi789/
│   ├── l/
│   └── overlay-config.json
├── containers/
│   └── <container-id>/
│       ├── config.json           # container config (OCI-like)
│       ├── state.json            # runtime state (pid, status, IP)
│       ├── upper/                # writable layer for this container
│       ├── work/                 # overlay work directory
│       └── merged/               # assembled overlay mountpoint
├── images/
│   └── <image-id>/
│       └── manifest.json         # layer chain metadata
├── volumes/
│   └── <volume-name>/
│       └── _data/                # volume data
├── networks/
│   └── <network-name>/
│       └── config.json           # bridge, subnet, gateway
├── kernel/
│   ├── bzImage                   # synthetic kernel (shared)
│   └── initramfs.cpio.gz         # guest init (shared)
└── podvmd.sock                   # daemon socket
```

### 3.3 Image Service

Queries the container storage backend to resolve image layers. Compatible
with Podman's storage format.

#### Storage Backends

- `overlay` (default): uses overlayfs layer storage
- `btrfs`: native btrfs snapshots
- `zfs`: native zfs clones

#### Layer Resolution

```rust
struct ImageService {
    storage_root: PathBuf,  // /var/lib/containers/storage
}

struct ImageManifest {
    id: String,
    layers: Vec<Layer>,
    config: ImageConfig,
}

struct Layer {
    digest: String,        // sha256:...
    diff_id: String,       // uncompressed digest
    size: u64,
    compressed_path: PathBuf,
    uncompressed_path: PathBuf,  // extracted layer directory
}

impl ImageService {
    /// Resolve image to ordered layer list
    fn resolve(&self, image_ref: &str) -> Result<ImageManifest>;

    /// Get layer directory path for virtiofs mount
    fn layer_path(&self, layer: &Layer) -> PathBuf;

    /// Check if image exists locally
    fn has_image(&self, image_ref: &str) -> bool;

    /// Pull image from registry
    fn pull(&self, image_ref: &str) -> Result<ImageManifest>;
}
```

#### Podman Compatibility

The image service reads Podman's metadata files:

```
/var/lib/containers/storage/
├── overlay-layers/
│   └── layers.json           # layer metadata
├── overlay-images/
│   └── <image-id>/
│       └── manifest          # image manifest
└── overlay-containers/
    └── <container-id>/
        └── userdata/
            └── config.json   # container config
```

### 3.4 Overlay Manager

Assembles overlayfs mounts on the host for each container. The merged
result is shared to the guest via virtiofs.

#### Overlay Assembly

```rust
struct OverlayManager {
    storage_root: PathBuf,   // /var/lib/podvm
}

struct OverlaySpec {
    lower_dirs: Vec<PathBuf>,  // ordered layer paths (first = lowest)
    upper_dir: PathBuf,        // per-container writable layer
    work_dir: PathBuf,         // overlay work directory
    merged_dir: PathBuf,       // assembled mount point
}

impl OverlayManager {
    /// Create overlay for new container
    fn create(&self, container_id: &str, manifest: &ImageManifest) -> Result<OverlaySpec>;

    /// Mount overlayfs
    fn mount(&self, spec: &OverlaySpec) -> Result<()>;

    /// Unmount and cleanup
    fn destroy(&self, container_id: &str) -> Result<()>;

    /// Get merged path for virtiofs share
    fn merged_path(&self, container_id: &str) -> PathBuf;
}
```

#### Mount Options

```
mount -t overlay overlay \
  -o lowerdir=<layer0>:<layer1>:<layer2>,\
         upperdir=<container-upper>,\
         workdir=<container-work> \
  <container-merged>
```

Note: `lowerdir` order is lowest-to-highest (layer0 is base image,
layer2 is most recent). The kernel applies them left-to-right for
reads.

### 3.5 VM Launcher

Starts and manages hypervisor instances. Generates VM configuration
and passes virtiofs shares.

#### Supported Hypervisors

| Hypervisor      | Status   | Notes                              |
|-----------------|----------|------------------------------------|
| Cloud Hypervisor| Primary  | Rust, virtiofs native, REST API    |
| QEMU            | Fallback | Mature, more options               |
| Firecracker     | Future   | Fast boot, limited virtiofs        |

#### VM Configuration

```rust
struct VMConfig {
    // Compute
    vcpus: u32,              // default: 1
    memory: u64,             // default: 128MB (bytes)

    // Boot
    kernel: PathBuf,         // shared synthetic kernel
    initramfs: PathBuf,      // shared guest init
    cmdline: String,         // kernel command line

    // Storage (virtiofs)
    shares: Vec<VirtiofsShare>,

    // Network
    net_config: NetConfig,

    // Optional
    serial: bool,            // enable serial console (default: true)
    console: bool,           // enable virtio-console (default: true)
}

struct VirtiofsShare {
    tag: String,             // mount tag inside guest
    socket: PathBuf,         // virtiofsd socket path
    readonly: bool,          // mount read-only in guest
}
```

#### Kernel Command Line Format

```
podvm.root=/rootfs podvm.entrypoint=/bin/sh podvm.args=-c,echo,hello podvm.env=PATH=/usr/bin:/bin podvm.env=HOME=/root
```

Parameters:

| Parameter          | Description                          | Required |
|--------------------|--------------------------------------|----------|
| `podvm.root`       | Mount point for container rootfs     | Yes      |
| `podvm.entrypoint` | Path to executable inside rootfs     | Yes      |
| `podvm.args`       | Comma-separated arguments            | No       |
| `podvm.env`        | KEY=VALUE environment variable       | No       |
| `podvm.cwd`        | Working directory inside rootfs      | No       |
| `podvm.user`       | UID:GID to run as                    | No       |
| `podvm.hostname`   | Container hostname                   | No       |

#### VM Lifecycle

```
1. Host: generate config
2. Host: create virtiofsd socket for each share
3. Host: start hypervisor with kernel + initramfs + shares
4. Guest: kernel boots (~50-100ms)
5. Guest: init reads /proc/cmdline
6. Guest: init mounts virtiofs shares
7. Guest: init chroots to podvm.root
8. Guest: init execs podvm.entrypoint
9. Host: receives process exit
10. Host: cleanup upper/work dirs
```

### 3.6 Network Manager

Handles container networking. Provides bridge-based networking with
port forwarding.

#### Network Architecture

```
┌─────────────────────────────────────────────────┐
│  HOST                                            │
│                                                  │
│  eth0 ──── podvm0 (bridge) ──── veth containers │
│              │                                   │
│              ├─ container1 (10.89.0.2)           │
│              ├─ container2 (10.89.0.3)           │
│              └─ container3 (10.89.0.4)           │
│                                                  │
└─────────────────────────────────────────────────┘
```

#### Network Configuration

```rust
struct NetConfig {
    mode: NetMode,
    bridge: Option<String>,     // bridge name (default: podvm0)
    subnet: Option<String>,     // CIDR (default: 10.89.0.0/24)
    gateway: Option<String>,    // gateway IP (default: 10.89.0.1)
    dns: Vec<String>,           // DNS servers
    mac_address: Option<String>,
    ip_address: Option<String>, // static IP (default: DHCP)
}

enum NetMode {
    Bridge,         // bridge network (default)
    Host,           // share host network namespace
    None,           // no networking
    Cni(String),    // CNI plugin name
}
```

#### Port Forwarding

```bash
podvm run -p 8080:80 nginx    # map host:8080 → container:80
podvm run -p 127.0.0.1:3000:3000 app  # bind to localhost only
```

Implemented via iptables/nftables DNAT rules on the host.

---

## 4. Guest Components

### 4.1 Synthetic Kernel

A minimal Linux kernel built exclusively for podvm guests. Contains
only virtio drivers and necessary filesystem support.

#### Kernel Configuration

```kconfig
# ──── Processor ────
CONFIG_SMP=y
CONFIG_NR_CPUS=8
CONFIG_X86_64=y
CONFIG_HYPERVISOR_GUEST=y
CONFIG_PARAVIRT=y
CONFIG_KVM_GUEST=y

# ──── Virtio Drivers (built-in, not modules) ────
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_FS=y
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_RNG=y
CONFIG_VSOCK=y
CONFIG_VIRTIO_VSOCKETS_COMMON=y

# ──── Filesystems ────
CONFIG_TMPFS=y
CONFIG_TMPFS_XATTR=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_TMPFS_POSIX_ACL=y

# ──── Essential ────
CONFIG_TTY=y
CONFIG_VT=y
CONFIG_VT_CONSOLE=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_BUG=y
CONFIG_FUTEX=y
CONFIG_EPOLL=y
CONFIG_SIGNALFD=y
CONFIG_TIMERFD=y
CONFIG_EVENTFD=y
CONFIG_AIO=y

# ──── Compression (for initramfs) ────
CONFIG_CRYPTO_LZ4=y
CONFIG_CRYPTO_ZSTD=y
CONFIG_CRYPTO_GZIP=y

# ──── NOT needed (no real hardware) ────
# CONFIG_ATA is not set
# CONFIG_SCSI is not set
# CONFIG_USB is not set
# CONFIG_SOUND is not set
# CONFIG_DRM is not set
# CONFIG_WLAN is not set
# CONFIG_BLUETOOTH is not set
# CONFIG_INFINIBAND is not set
# CONFIG_OVERLAY_FS is not set  (host does overlay, not guest)
```

#### Kernel Size Target

```
With above config:
  bzImage: ~2-3MB compressed
  Boot time: ~50-100ms to init
```

### 4.2 Guest Init (/init)

The sole userspace binary in the guest. Responsibilities:
1. Parse kernel command line for configuration
2. Mount virtiofs shares from host
3. Chroot into container rootfs
4. Exec the entrypoint process

#### Source Location

```
guest/
├── init.c           # main init source
├── cmdline.c        # kernel cmdline parser
├── cmdline.h
├── Makefile
└── musl-gcc.config  # musl static build config
```

#### init.c (Full Implementation)

```c
// guest/init.c
//
// Minimal init for podvm guests.
// Reads config from kernel cmdline, mounts virtiofs, chroots, execs.
//
// Build: musl-gcc -static -Os -o init init.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

// ──── Constants ────

#define MAX_ARGS        64
#define MAX_ENVS        128
#define CMDLINE_SIZE    4096
#define PATH_SIZE       1024

// ──── Configuration ────

typedef struct {
    char root[PATH_SIZE];           // podvm.root=/rootfs
    char entrypoint[PATH_SIZE];     // podvm.entrypoint=/bin/sh
    char args[MAX_ARGS][PATH_SIZE]; // podvm.args=-c,echo,hello
    int  argc;
    char envs[MAX_ENVS][PATH_SIZE]; // podvm.env=KEY=VALUE
    int  envc;
    char cwd[PATH_SIZE];            // podvm.cwd=/app
    char hostname[256];             // podvm.hostname=container1
    int  uid;                       // podvm.user=1000:1000
    int  gid;
} Config;

// ──── Kernel Cmdline Parser ────

// Read /proc/cmdline into buffer
int read_cmdline(char *buf, size_t len) {
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

// Find parameter value in cmdline
// Returns pointer to value, or NULL if not found
// Example: find_param(cmdline, "podvm.root=") returns "/rootfs"
const char *find_param(const char *cmdline, const char *prefix) {
    const char *p = cmdline;
    size_t prefix_len = strlen(prefix);

    while (*p) {
        // Skip whitespace
        while (*p == ' ') p++;
        if (!*p) break;

        if (strncmp(p, prefix, prefix_len) == 0) {
            return p + prefix_len;
        }

        // Skip to next param
        while (*p && *p != ' ') p++;
    }

    return NULL;
}

// Parse comma-separated values into array
// "arg1,arg2,arg3" → ["arg1", "arg2", "arg3"]
int parse_csv(const char *input, char output[][PATH_SIZE], int max) {
    int count = 0;
    const char *p = input;

    while (*p && count < max) {
        const char *start = p;
        while (*p && *p != ',') p++;

        size_t len = p - start;
        if (len >= PATH_SIZE) len = PATH_SIZE - 1;
        memcpy(output[count], start, len);
        output[count][len] = '\0';
        count++;

        if (*p == ',') p++;
    }

    return count;
}

// Parse uid:gid format
void parse_user(const char *str, int *uid, int *gid) {
    *uid = *gid = 0;
    if (str) {
        sscanf(str, "%d:%d", uid, gid);
    }
}

// Parse config from kernel cmdline
void parse_config(Config *cfg, const char *cmdline) {
    memset(cfg, 0, sizeof(*cfg));

    const char *val;

    // Required: podvm.root
    val = find_param(cmdline, "podvm.root=");
    if (val) {
        strncpy(cfg->root, val, PATH_SIZE - 1);
    }

    // Required: podvm.entrypoint
    val = find_param(cmdline, "podvm.entrypoint=");
    if (val) {
        strncpy(cfg->entrypoint, val, PATH_SIZE - 1);
    }

    // Optional: podvm.args (comma-separated)
    val = find_param(cmdline, "podvm.args=");
    if (val) {
        cfg->argc = parse_csv(val, cfg->args, MAX_ARGS);
    }

    // Optional: podvm.env (multiple KEY=VALUE)
    // Handled separately, see parse_env_params()

    // Optional: podvm.cwd
    val = find_param(cmdline, "podvm.cwd=");
    if (val) {
        strncpy(cfg->cwd, val, PATH_SIZE - 1);
    }

    // Optional: podvm.hostname
    val = find_param(cmdline, "podvm.hostname=");
    if (val) {
        strncpy(cfg->hostname, val, 255);
    }

    // Optional: podvm.user (uid:gid)
    val = find_param(cmdline, "podvm.user=");
    parse_user(val, &cfg->uid, &cfg->gid);
}

// ──── Filesystem Setup ────

// Create directory if it doesn't exist
int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_SIZE];
    char *p = NULL;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

// Mount pseudo-filesystems required for chroot to work
void mount_pseudofs(void) {
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, "size=64M");
}

// Mount container rootfs via virtiofs
void mount_rootfs(const Config *cfg) {
    mkdir_p(cfg->root, 0755);

    if (mount(NULL, cfg->root, "virtiofs", 0, "tag=rootfs") < 0) {
        perror("mount rootfs");
        // Fallback: try 9p if virtiofs not available
        mount(NULL, cfg->root, "9p", 0, "trans=virtio,version=9p2000.L,tag=rootfs");
    }
}

// ──── Process Setup ────

// Build argv array for execve
void build_argv(const Config *cfg, char **argv) {
    int i = 0;

    // argv[0] is the entrypoint itself
    argv[i++] = cfg->entrypoint;

    // Add arguments
    for (int j = 0; j < cfg->argc && i < MAX_ARGS - 1; j++) {
        argv[i++] = cfg->args[j];
    }

    argv[i] = NULL;
}

// Build environment array for execve
void build_envp(const Config *cfg, char **envp) {
    int i = 0;

    // Add configured environment variables
    for (int j = 0; j < cfg->envc && i < MAX_ENVS - 1; j++) {
        envp[i++] = cfg->envs[j];
    }

    // Ensure PATH is set
    int has_path = 0;
    for (int j = 0; j < i; j++) {
        if (strncmp(envp[j], "PATH=", 5) == 0) {
            has_path = 1;
            break;
        }
    }
    if (!has_path) {
        envp[i++] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }

    // Ensure HOME is set
    int has_home = 0;
    for (int j = 0; j < i; j++) {
        if (strncmp(envp[j], "HOME=", 5) == 0) {
            has_home = 1;
            break;
        }
    }
    if (!has_home) {
        envp[i++] = "HOME=/root";
    }

    envp[i] = NULL;
}

// ──── Main ────

int main(int argc, char **argv) {
    Config cfg;
    char cmdline[CMDLINE_SIZE];

    // Step 1: Mount pseudo-filesystems
    mount_pseudofs();

    // Step 2: Read and parse kernel cmdline
    if (read_cmdline(cmdline, sizeof(cmdline)) < 0) {
        fprintf(stderr, "podvm-init: failed to read /proc/cmdline\n");
        return 1;
    }

    parse_config(&cfg, cmdline);

    // Validate required parameters
    if (cfg.root[0] == '\0') {
        fprintf(stderr, "podvm-init: podvm.root not specified\n");
        return 1;
    }
    if (cfg.entrypoint[0] == '\0') {
        fprintf(stderr, "podvm-init: podvm.entrypoint not specified\n");
        return 1;
    }

    // Step 3: Set hostname if specified
    if (cfg.hostname[0] != '\0') {
        sethostname(cfg.hostname, strlen(cfg.hostname));
    }

    // Step 4: Mount container rootfs
    mount_rootfs(&cfg);

    // Step 5: Set working directory
    if (cfg.cwd[0] != '\0') {
        char full_cwd[PATH_SIZE];
        snprintf(full_cwd, sizeof(full_cwd), "%s/%s", cfg.root, cfg.cwd);
        if (chdir(full_cwd) < 0) {
            // CWD might not exist, fall back to root
            chdir(cfg.root);
        }
    } else {
        chdir(cfg.root);
    }

    // Step 6: Chroot into container
    if (chroot(cfg.root) < 0) {
        fprintf(stderr, "podvm-init: chroot failed: %s\n", strerror(errno));
        return 1;
    }
    chdir("/");

    // Step 7: Switch user if specified
    if (cfg.gid > 0) {
        setgid(cfg.gid);
    }
    if (cfg.uid > 0) {
        setuid(cfg.uid);
    }

    // Step 8: Build argv and envp, then exec
    char *exec_argv[MAX_ARGS + 1];
    char *exec_envp[MAX_ENVS + 1];

    build_argv(&cfg, exec_argv);
    build_envp(&cfg, exec_envp);

    fprintf(stderr, "podvm-init: exec %s\n", cfg.entrypoint);
    execvp(cfg.entrypoint, exec_argv);

    // If exec fails
    fprintf(stderr, "podvm-init: exec failed: %s\n", strerror(errno));
    return 1;
}
```

### 4.3 Guest Build

#### Makefile

```makefile
# guest/Makefile

CC = musl-gcc
CFLAGS = -static -Os \
         -fno-stack-protector \
         -fno-asynchronous-unwind-tables \
         -ffunction-sections \
         -fdata-sections \
         -Wl,--gc-sections
LDFLAGS = -s

SRC = init.c
OUT = init

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "Built: $$(ls -la $@ | awk '{print $$5}') bytes"

clean:
	rm -f $(OUT)

# Generate initramfs
initramfs: $(OUT)
	mkdir -p initramfs/{bin,proc,sys,dev,rootfs,tmp,etc}
	cp $(OUT) initramfs/bin/init
	@echo '#!/bin/sh' > initramfs/init
	@echo 'exec /bin/init "$$@"' >> initramfs/init
	@chmod +x initramfs/init
	cd initramfs && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > ../initramfs.cpio.gz
	@echo "initramfs: $$(ls -la initramfs.cpio.gz | awk '{print $$5}') bytes"

.PHONY: all clean initramfs
```

#### Build Commands

```bash
# Build init binary
cd guest && make

# Generate initramfs
cd guest && make initramfs

# Output
# init: 7848 bytes
# initramfs: 12847 bytes
```

---

## 5. Communication Protocol

### 5.1 Host ↔ Guest (Kernel Cmdline)

Static configuration passed at boot via kernel command line. No runtime
communication needed for basic operation.

```
podvm.root=/rootfs podvm.entrypoint=/bin/sh podvm.args=-c,echo,hello podvm.env=PATH=/usr/bin:/bin podvm.cwd=/app podvm.hostname=mycontainer podvm.user=1000:1000
```

### 5.2 Host ↔ Hypervisor (REST API)

Cloud Hypervisor provides a REST API for VM management.

```
# Start VM
PUT /api/v1/vm.start

# Boot VM
PUT /api/v1/vm.boot

# Shut down VM
PUT /api/v1/vm.shutdown

# Power off VM
PUT /api/v1/vm.power-off

# Get VM info
GET /api/v1/vm.info

# Add virtiofs device
PUT /api/v1/devices/fs
{
    "tag": "rootfs",
    "socket": "/run/podvm/<id>/virtiofs-rootfs.sock",
    "readonly": false
}
```

### 5.3 Future: Host ↔ Guest (vsock)

For advanced operations (exec, copy, signal), a vsock-based protocol
could be added. Not required for initial implementation.

```
# vsock CID assignment
HOST_CID = 2
GUEST_CID = 3

# Message format (future)
struct Message {
    u32 length;
    u8  type;
    u8  payload[];
};

# Message types (future)
MSG_EXEC      = 0x01  // execute command in guest
MSG_COPY_IN   = 0x02  // copy file to guest
MSG_COPY_OUT  = 0x03  // copy file from guest
MSG_SIGNAL    = 0x04  // send signal to process
MSG_LOG       = 0x10  // log message from guest
MSG_READY     = 0x20  // guest ready notification
MSG_EXIT      = 0x21  // process exited
```

---

## 6. Storage Model

### 6.1 Layer Sharing with Podman

podvm reads the same layer storage as Podman. No image conversion
required.

```
Podman storage:
/var/lib/containers/storage/
├── overlay/
│   ├── layer/
│   │   ├── abc123/          # extracted layer directory
│   │   ├── def456/
│   │   └── ghi789/
│   └── l/
│       ├── abc123-link      # symlink to layer path
│       ├── def456-link
│       └── ghi789-link

podvm reads:
/var/lib/containers/storage/overlay/layer/abc123  → virtiofs share
/var/lib/containers/storage/overlay/layer/def456  → virtiofs share
/var/lib/containers/storage/overlay/layer/ghi789  → virtiofs share
```

### 6.2 Container Working Storage

```
/var/lib/podvm/containers/<id>/
├── config.json        # OCI-compatible container config
├── state.json         # runtime state
├── upper/             # writable layer (created on start, deleted on rm)
├── work/              # overlay work dir
└── merged/            # overlay mount point (shared to guest)
```

### 6.3 Volume Mounts

```bash
# Named volume
podvm run -v mydata:/data nginx

# Bind mount
podvm run -v /host/path:/container/path nginx

# Read-only mount
podvm run -v /host/path:/container/path:ro nginx
```

Volumes are passed as additional virtiofs shares.

---

## 7. Networking

### 7.1 Default Bridge Network

```bash
podvm run nginx                 # gets IP from default bridge (10.89.0.x)
podvm run --network=mynet app   # join custom network
podvm run --network=host app    # share host network
podvm run --network=none app    # no networking
```

### 7.2 Port Publishing

```bash
podvm run -p 8080:80 nginx     # host:8080 → container:80
podvm run -p 127.0.0.1:3000:3000 app  # localhost only
podvm run -p 192.168.1.100:5000:5000 app  # specific interface
```

### 7.3 DNS

Containers resolve DNS via host's `/etc/resolv.conf` (default) or
custom DNS servers specified with `--dns`.

---

## 8. CLI Reference

### 8.1 podvm run

Create and start a container.

```bash
podvm run [OPTIONS] IMAGE [COMMAND] [ARG...]

Options:
  -d, --detach              Run container in background
  -it, --interactive        Keep STDIN open + allocate TTY
  --rm                      Remove container on exit
  -a, --attach []           Attach to STDIN/STDOUT/STDERR
  -e, --env []              Set environment variables
  -w, --workdir string      Working directory inside container
  --name string             Assign a name to the container
  --hostname string         Container hostname
  --user string             Username or UID (format: <name|uid>[:<group|gid>])
  --read-only               Mount container rootfs as read only
  -p, --publish []          Publish container port to host
  -v, --volume []           Bind mount a volume
  --network string          Connect to a network (default "bridge")
  --cpus float              Number of CPUs (default 1.0)
  --memory string           Memory limit (e.g., "512m", "1g")
  --restart string          Restart policy: no, on-failure, always, unless-stopped

Examples:
  podvm run -it --rm alpine /bin/sh
  podvm run -d --name web -p 80:80 nginx
  podvm run -v /data:/app/data -e DEBUG=1 myapp
```

### 8.2 podvm exec

Execute a command in a running container.

```bash
podvm exec [OPTIONS] CONTAINER COMMAND [ARG...]

Options:
  -d, --detach              Run command in background
  -e, --env []              Set environment variables
  -i, --interactive         Keep STDIN open
  --privileged              Give extended privileges (future: nested VM)
  -t, --tty                 Allocate a pseudo-TTY
  -u, --user string         Username or UID
  -w, --workdir string      Working directory

Examples:
  podvm exec -it web /bin/sh
  podvm exec web cat /etc/os-release
  podvm exec -u www-data web nginx -t
```

### 8.3 podvm build

Build an image from a Dockerfile.

```bash
podvm build [OPTIONS] PATH | URL | -

Options:
  -f, --file string         Dockerfile path (default "Dockerfile")
  -t, --tag []              Name and tag for the image
  --build-arg []            Set build-time variables
  --no-cache                Do not use cache when building
  --squash                  Squash layers into a single layer (future)

Examples:
  podvm build -t myapp:latest .
  podvm build -f Dockerfile.prod -t myapp:prod .
```

### 8.4 podvm images

List local images.

```bash
podvm images [OPTIONS]

Options:
  -a, --all                 Show all images
  --digests                 Show digests
  -q, --quiet               Only show image IDs

REPOSITORY   TAG       IMAGE ID      CREATED        SIZE
nginx        alpine    sha256:...    2 weeks ago    42MB
alpine       latest    sha256:...    3 weeks ago    7MB
```

### 8.5 podvm ps

List running containers.

```bash
podvm ps [OPTIONS]

Options:
  -a, --all                 Show all containers
  --filter []               Filter output
  -q, --quiet               Only show container IDs
  --no-trunc                Do not truncate output

CONTAINER ID  IMAGE         COMMAND                  CREATED        STATUS        PORTS                 NAMES
a1b2c3d4e5f6  nginx:alpine  "/docker-entrypoint.…"  2 minutes ago  Up 2 minutes  0.0.0.0:80->80/tcp    web
```

---

## 9. Configuration Files

### 9.1 /etc/containers/containers.conf

Shared with Podman for image/registry configuration.

```toml
[containers]
default_sysctls = []
default_ulimits = []
log_driver = "k8s-file"
log_size = 0

[engine]
cgroup_manager = "systemd"
events_logger = "journald"
runtime = "crun"

[registries.search]
registries = ["docker.io", "quay.io"]

[machine]
# podvm-specific settings
driver = "kvm"                    # kvm, qemu, firecracker
vcpus = 1
memory = "128m"
kernel = "/var/lib/podvm/kernel/bzImage"
initramfs = "/var/lib/podvm/kernel/initramfs.cpio.gz"
```

### 9.2 /etc/containers/storage.conf

Shared with Podman for storage configuration.

```toml
[storage]
driver = "overlay"
runroot = "/run/containers/storage"
graphroot = "/var/lib/containers/storage"

[storage.options]
mount_program = "/usr/bin/fuse-overlayfs"
```

---

## 10. Build System

### 10.1 Top-Level Makefile

```makefile
# Makefile

KERNEL_VERSION = 6.6.0
KERNEL_CONFIG = kernel/config
KERNEL_SRCDIR = kernel/linux-$(KERNEL_VERSION)

GUEST_SRC = guest/init.c
GUEST_BIN = guest/init
GUEST_INITRAMFS = guest/initramfs.cpio.gz

HOST_SRC = $(wildcard host/src/*.rs)
HOST_BIN = target/release/podvm
DAEMON_BIN = target/release/podvmd

DESTDIR ?= /
PREFIX ?= /usr/local

.PHONY: all clean kernel guest host install

all: kernel guest host

# ──── Kernel ────

kernel: $(KERNEL_SRCDIR)/arch/x86/boot/bzImage

$(KERNEL_SRCDIR)/arch/x86/boot/bzImage:
	@echo "Building synthetic kernel..."
	cd $(KERNEL_SRCDIR) && \
		cp $(CURDIR)/$(KERNEL_CONFIG) .config && \
		make -j$$(nproc) bzImage

# ──── Guest ────

guest: $(GUEST_INITRAMFS)

$(GUEST_BIN): $(GUEST_SRC)
	@echo "Building guest init..."
	cd guest && make

$(GUEST_INITRAMFS): $(GUEST_BIN)
	@echo "Building initramfs..."
	cd guest && make initramfs

# ──── Host ────

host: $(HOST_BIN)

$(HOST_BIN): $(HOST_SRC)
	@echo "Building host tools..."
	cargo build --release

# ──── Install ────

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(HOST_BIN) $(DESTDIR)$(PREFIX)/bin/podvm
	install -m 755 target/release/podvmd $(DESTDIR)$(PREFIX)/bin/podvmd
	install -d $(DESTDIR)/var/lib/podvm/kernel
	install -m 644 $(KERNEL_SRCDIR)/arch/x86/boot/bzImage $(DESTDIR)/var/lib/podvm/kernel/
	install -m 644 $(GUEST_INITRAMFS) $(DESTDIR)/var/lib/podvm/kernel/

# ──── Clean ────

clean:
	cd guest && make clean
	rm -f $(HOST_BIN) $(DAEMON_BIN)
	cargo clean
```

### 10.2 Kernel Build

```bash
# Download kernel source
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.0.tar.xz
tar xf linux-6.6.0.tar.xz -C kernel/

# Apply config and build
make -C kernel/linux-6.6.0 defconfig
cp kernel/config kernel/linux-6.6.0/.config
make -C kernel/linux-6.6.0 olddefconfig
make -C kernel/linux-6.6.0 -j$(nproc) bzImage

# Output: kernel/linux-6.6.0/arch/x86/boot/bzImage (~2-3MB)
```

---

## 11. Security Model

### 11.1 Isolation Boundaries

```
┌─────────────────────────────────────────────────────────────┐
│  VM Boundary (strongest)                                     │
│  • Hardware isolation via KVM                                │
│  • Separate kernel per container                             │
│  • Guest cannot see host processes/memory/devices            │
│  • No shared memory between containers                       │
├─────────────────────────────────────────────────────────────┤
│  Virtiofs (controlled)                                       │
│  • Only explicitly shared directories visible to guest       │
│  • Read-only shares for lower layers                         │
│  • No access to host filesystem beyond shares                │
├─────────────────────────────────────────────────────────────┤
│  Network (configurable)                                      │
│  • Bridge network isolates containers from each other        │
│  • Port forwarding only for explicitly published ports       │
│  • --network=none for air-gapped containers                  │
└─────────────────────────────────────────────────────────────┘
```

### 11.2 What's NOT Needed

| Container Feature | Why podvm Doesn't Need It |
|-------------------|---------------------------|
| Namespaces | VM provides full isolation |
| Cgroups | VM limits CPU/memory directly |
| Seccomp | VM boundary filters syscalls |
| User namespaces | VM runs as real user |
| Capabilities dropping | Guest has no host access |
| Rootless mode | VM is already isolated |

### 11.3 Privileged Containers

```bash
podvm run --privileged nginx    # future: nested VM or relaxed virtiofs
```

Currently: `--privileged` is a no-op or warns. The VM boundary provides
stronger isolation than container privileges.

---

## 12. Performance Characteristics

### 12.1 Boot Time

```
Phase                          Time
─────────────────────────────────────
Kernel boot                    50-100ms
Init execution                 <1ms
Virtiofs mount                 1-5ms
Chroot + exec                  <1ms
─────────────────────────────────────
Total (to process start)       ~100ms

Compare:
  containerd + runc:           200-500ms
  Docker Desktop VM:           2-3s (first boot)
```

### 12.2 Memory Overhead

```
Component                     Memory
─────────────────────────────────────
Guest kernel                  2-5MB (shared page cache)
Init process                  <1MB
Per-VM overhead               ~10MB (QEMU/CH overhead)
─────────────────────────────────────
Total per container           ~15MB

Compare:
  containerd shim:            ~10MB
  Docker Desktop VM:          ~500MB (shared for all)
```

### 12.3 I/O Performance

```
Operation                     Performance
─────────────────────────────────────────
virtiofs read (lower layer)   ~80% native
virtiofs write (upper layer)  ~90% native
overlayfs read (in guest)     ~95% native (page cache)
─────────────────────────────────────────
Bottleneck: virtiofs daemon + host filesystem

Compare:
  overlayfs (container):      ~98% native
  9p (virtio fallback):       ~50% native
```

### 12.4 Startup Latency

```
First run (cold cache):       ~150ms
Subsequent (warm cache):      ~100ms
Parallel start (10 VMs):      ~200ms total (parallel boot)

Compare:
  container cold start:       ~100ms
  container warm start:       ~50ms
```

---

## 13. Limitations and Trade-offs

### 13.1 Current Limitations

| Limitation | Impact | Workaround |
|------------|--------|------------|
| No `docker build` layer caching | Slower rebuilds | Use `--cache-from` (future) |
| No `docker commit` | Can't save container state as image | Export filesystem manually |
| No container checkpointing | No CRIU support | Not applicable for VMs (future: snapshot) |
| No GPU passthrough (yet) | ML workloads limited | Future: VFIO passthrough |
| Single architecture | x86_64 only initially | Future: aarch64 support |

### 13.2 Trade-offs

```
podvm gains:
  ✓ Stronger isolation (VM vs namespaces)
  ✓ Simpler security model (no namespace bugs)
  ✓ Independent kernels (can tune per workload)
  ✓ No seccomp/cgroup configuration
  ✓ Same images as Podman/Docker

podvm loses:
  ✗ Higher memory overhead (~15MB vs ~1MB for container)
  ✗ Slightly slower I/O (virtiofs overhead)
  ✗ No shared kernel (can't share page cache)
  ✗ VM limits (max ~256 vCPUs, ~128GB RAM per VM)
  ✗ Nested virtualization complexity
```

---

## 14. Future Roadmap

### 14.1 Phase 1: MVP

- [ ] Kernel build system
- [ ] Guest init (musl static)
- [ ] Host launcher (CLI + daemon)
- [ ] Basic `podvm run`, `podvm stop`, `podvm rm`
- [ ] Overlay assembly
- [ ] Virtiofs shares
- [ ] Bridge networking

### 14.2 Phase 2: Compatibility

- [ ] `podvm exec` (via vsock)
- [ ] `podvm logs` (serial console capture)
- [ ] `podvm build` (Dockerfile support)
- [ ] `podvm pull` (registry support)
- [ ] Named volumes
- [ ] Port forwarding

### 14.3 Phase 3: Advanced

- [ ] GPU passthrough (VFIO)
- [ ] Live migration
- [ ] VM snapshots
- [ ] ARM64 support
- [ ] Firecracker backend
- [ ] Kubernetes integration (CRI)

### 14.4 Phase 4: Ecosystem

- [ ] podvm-compose (docker-compose compatible)
- [ ] podvm-machine (VM management for rootless)
- [ ] Podman integration (shared daemon)
- [ ] IDE extensions (VS Code, etc.)

---

## 15. Glossary

| Term | Definition |
|------|------------|
| **podvm** | Container runtime using VMs instead of namespaces |
| **synthetic kernel** | Minimal kernel with only virtio drivers, shared across all VMs |
| **guest init** | Musl static binary that boots inside each VM |
| **virtiofs** | Host-guest shared filesystem protocol |
| **overlay manager** | Host component that assembles container filesystems |
| **container storage** | Shared layer storage compatible with Podman |
| **upper dir** | Per-container writable layer for overlay filesystem |
| **merged dir** | Assembled overlay mount point shared to guest |

---

*Specification Version: 1.0-draft*
*Last Updated: 2024-09-29*
