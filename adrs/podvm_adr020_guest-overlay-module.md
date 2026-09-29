# ADR-020: Guest Kernel Module for Overlay Management

**Date:** 2024-09-29

**Status:** Accepted

## Context

Container builds (podvm build) require layer commits. Each Dockerfile RUN instruction modifies the upper layer, which must be frozen into a new OCI layer before the next instruction.

The current design has the host managing overlays. The host assembles overlayfs, shares the merged directory via virtiofs, and must handle layer commits. This works for `podvm run` (no layer changes) but is complex for `podvm build` (layer changes between instructions).

A guest kernel module can handle overlay assembly and dirty tracking inside the guest. The guest module communicates with the host via vsock for layer information. This keeps the overlay logic in one place (the guest kernel) rather than splitting it between host and guest userspace.

The guest kernel is synthetic and under our control. Adding a kernel module is straightforward - it is built as part of the kernel compilation, not loaded from external media.

## Decision

podvm will include a guest kernel module (podvm-overlay.ko) that manages overlay assembly and layer commits. The module is compiled into the synthetic kernel. The guest init loads the module and delegates overlay operations to it.

## Rationale

The guest module has direct access to the overlay filesystem. It can track which files are modified, when writes occur, and when a layer commit is requested. This is cleaner than host-side tracking, which must infer changes from virtiofs traffic.

The module integrates with Linux overlayfs natively. It uses the existing overlayfs infrastructure but adds podvm-specific behavior: fetching layer paths from the host, signaling commits, and managing the virtiofs-backed lower layers.

The guest init becomes a thin wrapper. It loads the module, provides configuration (image layers, upper directory), and delegates to the module API. The init grows from ~10KB to ~15KB but gains significant functionality.

The module is testable in isolation. It can be tested with standard kernel module tests. The overlay logic is in one place, not spread across host and guest userspace.

Host-side changes are minimal. The host provides layer paths via virtiofs or vsock. The host handles the actual layer storage (tar extraction, OCI layer creation). The module tracks what changes; the host stores the results.

## Module Design

```c
// podvm-overlay.ko

// Configuration (set by guest init via module params or sysfs)
//   layers: comma-separated virtiofs mount tags for lower layers
//   upper: virtiofs mount tag for writable upper
//   work: virtiofs mount tag for overlay work directory

// Operations:
//   mount -t podvm-overlay -o layers=layer0:layer1,upper=upper,work=work none /merged
//   ioctl(fd, PODVM_FREEZE)    // freeze upper for commit
//   ioctl(fd, PODVM_SNAPSHOT)  // create snapshot of upper
//   ioctl(fd, PODVM_COMMIT)    // commit upper to host as new layer
```

### Module Parameters

```c
static char *layers = "layer0";   // lower layer tags (colon-separated)
module_param(layers, charp, 0644);

static char *upper = "upper";     // upper layer tag
module_param(upper, charp, 0644);

static char *work = "work";       // work dir tag
module_param(work, charp, 0644);
```

### Module API

```c
// ioctl commands for overlay management
#define PODVM_MAGIC 'P'

struct podvm_layer_info {
    char name[256];       // layer name/ID
    char path[1024];      // virtiofs path
    int readonly;         // 1 for lower layers, 0 for upper
};

#define PODVM_GETLAYERS   _IOR(PODVM_MAGIC, 1, struct podvm_layer_info *)
#define PODVM_FREEZE      _IO(PODVM_MAGIC, 2)   // freeze upper
#define PODVM_SNAPSHOT    _IO(PODVM_MAGIC, 3)   // snapshot upper
#define PODVM_COMMIT      _IO(PODVM_MAGIC, 4)   // commit to host
#define PODVM_RESUME      _IO(PODVM_MAGIC, 5)   // resume after commit
```

### Dirty Tracking

```c
// Module hooks into VFS operations to track modifications
// on the upper layer filesystem

static int podvm_rename(struct mnt_idmap *idmap,
                        struct dentry *old, struct dentry *new) {
    // Track file rename in upper layer
    mark_dirty(old);
    return vfs_rename(idmap, old, new);
}

static int podvm_unlink(struct mnt_idmap *idmap,
                        struct dentry *dentry) {
    // Track file deletion in upper layer
    mark_deleted(dentry);
    return vfs_unlink(idmap, dentry);
}

// Similar hooks for: write, mkdir, rmdir, create, truncate
```

### Layer Commit Flow

```
1. Guest init loads module with layer configuration
2. Module mounts overlay using virtiofs-backed lower layers
3. Application runs, writes to upper layer
4. Module tracks all modifications (dirty list)
5. Host signals PODVM_FREEZE (stop accepting writes)
6. Module flushes pending writes, syncs upper
7. Host signals PODVM_COMMIT
8. Module communicates dirty list to host via vsock
9. Host creates new OCI layer from dirty files
10. Host creates new overlay (prev layers + new layer + fresh upper)
11. Module remounts overlay with new configuration
12. Host signals PODVM_RESUME
13. Application continues
```

## Implications

The module adds ~50-100KB to the kernel binary. This is acceptable for the synthetic kernel (currently ~2-3MB). The module is compiled in (not modular) since the guest has no module loading infrastructure.

The module requires CONFIG_OVERLAY_FS=y in the guest kernel. This was previously not needed (host did overlay). The config addition is ~10KB to kernel.

Dirty tracking adds overhead to VFS operations. Each file write, rename, and delete goes through the module's hooks. This is comparable to SELinux or other LSM overhead (~5-10% for write-heavy workloads).

The module must handle concurrent access safely. Multiple processes inside the container may write simultaneously. Spinlocks or RCU protect the dirty list.

The module communicates with the host via vsock. The vsock connection is established by the guest init and passed to the module. This requires the module to use kernel socket APIs.

## Alternatives Considered

**Host-side overlay management:** Current design. Works for `podvm run` but complex for `podvm build`. The host must coordinate with the guest to know when to commit layers. Split logic between host and guest userspace.

**QCOW2 overlays:** Considered using QEMU's block layer. Rejected because OCI layers are directories, not block images. Converting between formats adds complexity.

**No module (userspace agent):** Considered implementing overlay management in a guest userspace agent. Rejected because dirty tracking requires kernel hooks. Userspace cannot intercept VFS operations.

**FUSE-based overlay:** Considered implementing overlay in userspace via FUSE. Rejected due to performance overhead (~30% for metadata-heavy operations). Kernel overlayfs is faster.

**Existing overlayfs with manual management:** Considered using stock overlayfs and managing layers manually. This works for `podvm run` but requires external coordination for build. The module encapsulates this logic.
