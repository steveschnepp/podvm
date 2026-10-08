# Guest Architecture: What Belongs Where?

> Defining clear boundaries between host and guest responsibilities.

---

## Current State

```
HOST                              GUEST
──────                            ──────
podvm CLI (C)                     init (musl, ~500 LOC)
  ├── parse args
  ├── start virtiofsd
  ├── start QEMU
  └── wait for exit

                                  mount virtiofs
                                  chroot
                                  exec entrypoint
```

**Problem**: Guest is too thin. Host has to do everything.

---

## The Question

What guest responsibilities could be:

1. **Modular** - Swappable implementations
2. **Optional** - Not always needed
3. **Secure** - Running inside VM isolation

---

## Proposed Guest Modules

### Module Architecture

```
                              GUEST
                              ──────
┌─────────────────────────────────────────────────────────────┐
│  Guest Kernel (synthetic, ~2MB)                              │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│  /init (minimal, ~7KB)                                       │
│  ├── Mount pseudo-fs (proc, sys, dev)                        │
│  ├── Receive config (vsock or cmdline)                       │
│  ├── Load modules (optional)                                 │
│  ├── Setup environment                                       │
│  └── Exec entrypoint                                         │
└─────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
┌───────▼───────┐    ┌───────▼───────┐    ┌───────▼───────┐
│  net module   │    │  fs module    │    │  exec module  │
│  (optional)   │    │  (optional)   │    │  (required)   │
└───────────────┘    └───────────────┘    └───────────────┘
```

---

## Module Definitions

### 1. `exec` Module (Required)

**Purpose**: Run the user's command

**Responsibilities**:
- Parse entrypoint + args + env
- Set uid/gid
- Set working directory
- Chroot/pivot_root
- Execve

**Interface**:
```c
typedef struct ExecConfig {
    char *entrypoint;
    char **argv;
    char **envp;
    char *cwd;
    uid_t uid;
    gid_t gid;
} ExecConfig;

int exec_run(ExecConfig *cfg);
```

**Implementation**: Current init.c logic

---

### 2. `net` Module (Optional)

**Purpose**: Configure guest networking

**When needed**: VM needs network access (apt, curl, etc.)

**Responsibilities**:
- Configure eth0 (IP, routes)
- Set DNS
- Optional: iptables rules

**Interface**:
```c
typedef struct NetConfig {
    char *interface;    // "eth0"
    char *ip;           // "172.16.0.2/24"
    char *gateway;      // "172.16.0.1"
    char *dns[4];
    int dns_count;
} NetConfig;

int net_setup(NetConfig *cfg);
```

**Implementation options**:
- Shell script (`/sbin/ip addr add ...`)
- Minimal C binary (`ifconfig`-like)
- Built into init

**Trade-off**: Adds ~10KB to guest, but many containers need network

---

### 3. `fs` Module (Optional)

**Purpose**: Filesystem setup beyond basic mount

**When needed**: Complex storage, tmpfs, bind mounts

**Responsibilities**:
- Mount additional filesystems
- Create tmpfs for /tmp, /run
- Bind mounts
- Set up /etc (resolv.conf, hosts)

**Interface**:
```c
typedef struct FsMount {
    char *source;
    char *target;
    char *fstype;
    unsigned long flags;
    char *data;
} FsMount;

int fs_setup(FsMount *mounts, int count);
```

**Example mounts**:
```c
FsMount mounts[] = {
    { NULL, "/tmp",    "tmpfs", 0, "size=64M" },
    { NULL, "/run",    "tmpfs", 0, "size=16M" },
    { NULL, "/dev/shm","tmpfs", 0, NULL },
};
```

---

### 4. `pty` Module (Optional)

**Purpose**: Interactive terminal support

**When needed**: `-it` mode, `podvm exec`

**Responsibilities**:
- Set up /dev/pts
- Configure TERM
- Handle window size signals (SIGWINCH)
- Proxy stdin/stdout via vsock

**Interface**:
```c
typedef struct PtyConfig {
    int rows;
    int cols;
    char *term;      // "xterm-256color"
    int vsock_fd;    // for I/O proxy
} PtyConfig;

int pty_setup(PtyConfig *cfg);
```

---

### 5. `log` Module (Optional)

**Purpose**: Guest-side logging

**When needed**: Debugging, observability

**Responsibilities**:
- Log to serial (debug)
- Log to vsock (structured)
- Metrics collection

**Interface**:
```c
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);
void log_metric(const char *name, int value);
```

---

### 6. `health` Module (Optional)

**Purpose**: Health checking and readiness

**When needed**: Orchestrators, load balancers

**Responsibilities**:
- Listen on vsock for health probes
- Report process status
- Expose metrics

**Interface**:
```c
// Background thread
void health_start(int vsock_port);

// Called by exec module
void health_set_ready(void);
void health_set_unhealthy(void);
```

---

## Module Loading

### Option A: Compile-Time

```c
// init.c
#include "modules/exec.h"
#include "modules/net.h"    // optional, #ifdef ENABLE_NET
#include "modules/fs.h"     // optional, #ifdef ENABLE_FS

int main(void) {
    // ... mount pseudofs, parse config ...
    
    #ifdef ENABLE_NET
    net_setup(&net_cfg);
    #endif
    
    #ifdef ENABLE_FS
    fs_setup(mounts, mount_count);
    #endif
    
    exec_run(&exec_cfg);
}
```

**Pros**: Minimal binary, no runtime overhead
**Cons**: Different binaries for different use cases

### Option B: Dynamic Loading

```c
// init.c
typedef int (*ModuleInit)(Config *cfg);

int main(void) {
    // Load modules from /lib/podvm/
    ModuleInit modules[] = { NULL };
    load_modules("/lib/podvm/", modules);
    
    for (int i = 0; modules[i]; i++)
        modules[i](&cfg);
}
```

**Pros**: Single binary, flexible
**Cons**: Needs /lib, larger attack surface

### Option C: Requested via Config

```c
// Guest config from vsock
{
    "modules": ["net", "fs"],
    "net": { "ip": "172.16.0.2/24" },
    "fs": { "tmpfs": ["/tmp:64M", "/run:16M"] }
}
```

**Pros**: Explicit, auditable
**Cons**: More complex protocol

---

## Recommended: Option A (Compile-Time) + Profiles

Create pre-built guest images for different use cases:

| Profile | Modules | Size | Use Case |
|---------|---------|------|----------|
| `minimal` | exec only | ~7KB | Simple commands, scripts |
| `network` | exec + net | ~17KB | apt, curl, git |
| `full` | all | ~30KB | Development, debugging |

```bash
# Build profiles
make guest PROFILE=minimal   # Just exec
make guest PROFILE=network   # + networking
make guest PROFILE=full      # + everything
```

---

## Host-Side Module Mapping

The host also has optional components:

### Host Modules

| Module | Purpose | When Needed |
|--------|---------|-------------|
| `overlay` | Assemble OCI layers | Always |
| `network` | Create bridge, tap devices | If guest needs network |
| `vsock` | Host↔guest communication | Always (future) |
| `storage` | Mount backends | If using block/NBD |
| `registry` | Pull OCI images | If not pre-pulled |

### Host-Guest Protocol

```
HOST                              GUEST
──────                            ──────
overlay module                    exec module
    │                                 │
    ├─── vsock: config ──────────────►│
    │    { entrypoint, env, ... }     │
    │                                 │
    │    ◄──── vsock: ready ──────────┤
    │                                 │
    │    ◄──── vsock: exit_code ──────┤
    │                                 │
network module (optional)         net module (optional)
    │                                 │
    ├─── tap device ────────────────►│
    │    ip addr add ...              │
```

---

## Minimal Guest Spec

```c
// guest/init.c - The absolute minimum

#include "modules/exec.h"

int main(void) {
    // 1. Mount /proc, /sys, /dev
    mount_pseudofs();
    
    // 2. Parse config (cmdline or vsock)
    Config cfg;
    parse_config(&cfg);
    
    // 3. Mount rootfs (virtiofs)
    mount_rootfs(cfg.root);
    
    // 4. Chroot + exec
    chroot(cfg.root);
    chdir(cfg.cwd ?: "/");
    execvp(cfg.entrypoint, cfg.argv);
}
```

Size: **~5KB** compiled with musl

---

## Extended Guest Spec

```c
// guest/init.c - Full featured

#include "modules/exec.h"
#include "modules/net.h"
#include "modules/fs.h"
#include "modules/log.h"

int main(void) {
    log_info("podvm-guest starting");
    
    mount_pseudofs();
    
    Config cfg;
    parse_config(&cfg);
    
    // Filesystem setup
    fs_setup(cfg.mounts, cfg.mount_count);
    mount_rootfs(cfg.root);
    
    // Network setup (if configured)
    if (cfg.net.ip)
        net_setup(&cfg.net);
    
    // Logging
    log_info("executing %s", cfg.entrypoint);
    
    // Chroot + exec
    chroot(cfg.root);
    chdir(cfg.cwd ?: "/");
    execvp(cfg.entrypoint, cfg.argv);
}
```

Size: **~15KB** compiled with musl

---

## Open Questions

1. **Module location**: Inside init binary or separate binaries?
2. **Module config**: Via cmdline, vsock, or config file in initramfs?
3. **Module dependencies**: Can net depend on fs (for /etc/resolv.conf)?
4. **Module versioning**: How to match host/guest module versions?

---

## Next Steps

1. Define module interface headers
2. Implement `exec` module (already exists)
3. Implement `net` module (basic IP config)
4. Create build profiles
5. Document host-guest protocol
