# podvm Architecture Diagrams

## System Overview

```
+------------------------------------------------------------------+
| HOST                                                              |
|                                                                  |
|  +--------------------------------------------------------------+|
|  | podvm CLI                                                     ||
|  | (argument parsing, output formatting)                         ||
|  +----------------------------+----------------------------------+|
|                               |                                   |
|  +----------------------------v----------------------------------+|
|  | Image Service                                                  ||
|  | (queries container storage, resolves layers)                   ||
|  +----------------------------+----------------------------------+|
|                               |                                   |
|  +----------------------------v----------------------------------+|
|  | Overlay Manager                                                ||
|  | (assembles overlayfs per container)                            ||
|  +----------------------------+----------------------------------+|
|                               |                                   |
|  +----------------------------v----------------------------------+|
|  | VM Launcher                                                    ||
|  | (starts QEMU, manages virtiofsd)                               ||
|  +----------------------------+----------------------------------+|
|                               |                                   |
|  +----------------------------v----------------------------------+|
|  | QEMU                                                           ||
|  | (virtualization, virtio devices)                               ||
|  +----------------------------+----------------------------------+|
|                               |                                   |
+------------------------------+------------------------------------+
                               |
                      virtiofs | virtio-console | vsock
                               |
+------------------------------+------------------------------------+
|  GUEST (per-container VM)    |                                   |
|                               |                                   |
|  +----------------------------v----------------------------------+|
|  | Synthetic Kernel                                              ||
|  | (virtio drivers, ~2MB)                                        ||
|  | +------------------------------------------------------------+||
|  | | podvm-overlay.ko (guest module)                             |||
|  | | (overlay assembly, dirty tracking)                          |||
|  | +------------------------------------------------------------+||
|  +----------------------------+----------------------------------+|
|                               |                                   |
|  +----------------------------v----------------------------------+|
|  | /init (musl static, ~7KB)                                     ||
|  | (loads module, mounts overlay, chroots, execs)                ||
|  +--------------------------------------------------------------+|
|                                                                  |
+------------------------------------------------------------------+
```

## Container Lifecycle

```
podvm run -it debian echo hello world
|
+---> resolve layers from container storage
+---> create upper/work dirs for this container
+---> assemble overlay on host
+---> start virtiofsd (serves merged dir)
+---> start QEMU with virtiofs + balloon + QMP
|
+---> guest boots (~50-100ms)
+---> init reads /proc/cmdline
+---> init mounts virtiofs at /rootfs
+---> init creates PID namespace
+---> init chroots to /rootfs
+---> init execs echo "hello world"
|
+---> echo writes to stdout
+---> stdout -> virtio-console -> host terminal
|
+---> echo exits with status 0
+---> init exits
+---> QEMU exits
+---> host detects exit via QMP
+---> host cleans up upper/work dirs
```

## Overlay Assembly (Host-Side)

```
OCI Image Layers (from container storage):

  /var/lib/containers/storage/overlay/layer/abc123/  (base)
  /var/lib/containers/storage/overlay/layer/def456/  (middle)
  /var/lib/containers/storage/overlay/layer/ghi789/  (top)

        |
        v

Host Assembles Overlay:

  mount -t overlay overlay \
    -o lowerdir=abc123:def456:ghi789,\
             upperdir=/var/lib/podvm/containers/<id>/upper,\
             workdir=/var/lib/podvm/containers/<id>/work \
    /var/lib/podvm/containers/<id>/merged

        |
        v

virtiofsd Shares Merged Directory:

  virtiofsd --socket-path=virtiofs.sock \
            --shared-dir=/var/lib/podvm/containers/<id>/merged

        |
        v

Guest Sees Complete Filesystem:

  /rootfs/ (via virtiofs)
    +-- bin/
    +-- etc/
    +-- usr/
    +-- var/
    +-- ... (complete debian filesystem)
```

## Memory Management

```
                        Without podvm              With podvm
                        (fixed allocation)         (balloon + DAX)
                        ------------------         ------------------

VM 1 (idle):           128MB locked               10MB actual (balloon inflated)
VM 2 (busy):           128MB locked               128MB actual (balloon deflated)
VM 3 (idle):           128MB locked               10MB actual (balloon inflated)

Shared layer cache:    10 x 40MB = 400MB          1 x 40MB = 40MB (DAX)

Total:                 784MB                       188MB
```

## DAX Layer Deduplication

```
Without DAX:

  VM1 page cache: [layer0 data] [layer1 data] [layer2 data]  40MB
  VM2 page cache: [layer0 data] [layer1 data] [layer2 data]  40MB
  VM3 page cache: [layer0 data] [layer1 data] [layer2 data]  40MB
  Total: 120MB (same data duplicated 3 times)

With DAX:

  Host page cache: [layer0 data] [layer1 data] [layer2 data]  40MB
  VM1: maps directly to host pages (no copy)
  VM2: maps directly to host pages (no copy)
  VM3: maps directly to host pages (no copy)
  Total: 40MB (single copy, shared)
```

## vsock IPC Protocol

```
Host                              Guest
  |                                 |
  |--- MSG_EXEC {cmd, env} -------->|  (send command)
  |                                 |  fork + exec
  |<------ MSG_STDOUT {data} -------|  (stream output)
  |<------ MSG_STDOUT {data} -------|
  |<------ MSG_EXEC_DONE {code} ----|  (command finished)
  |                                 |
  |--- MSG_FREEZE ----------------->|  (prepare for layer commit)
  |<------ MSG_FROZEN --------------|  (writes paused)
  |                                 |
  | host: commit upper -> new layer |
  | host: new overlay assembled     |
  |                                 |
  |--- MSG_RESUME ----------------->|  (continue with new overlay)
  |<------ MSG_READY ---------------|  (ready for next command)
```

## QEMU Device Layout

```
+------------------------------------------------------------------+
| QEMU VM                                                           |
|                                                                  |
|  CPU (KVM)                                                        |
|    |                                                              |
|    +-- Memory (128MB, memfd shared for virtiofs)                 |
|    |                                                              |
|    +-- PCI Bus                                                    |
|         |                                                         |
|         +-- virtio-blk (if needed)                               |
|         |                                                         |
|         +-- virtio-net (bridge network)                          |
|         |                                                         |
|         +-- virtio-console (stdout/stderr)                       |
|         |                                                         |
|         +-- virtio-fs (container rootfs via virtiofsd)           |
|         |                                                         |
|         +-- virtio-balloon (memory reclamation)                  |
|         |                                                         |
|         +-- vhost-vsock (host-guest IPC)                         |
|                                                                  |
|  QMP socket (control: balloon, status, shutdown)                 |
|  Serial console (boot messages, debug)                           |
|                                                                  |
+------------------------------------------------------------------+
```

## Build Flow with Guest Module

```
Host                              Guest
  |                                 |
  | virtiofs: layers + upper        |
  +-------------------------------->| module mounts overlay
  |                                 | exec "apt-get update"
  |                                 | writes tracked by module
  |                                 |
  | vsock: PODVM_FREEZE             |
  +-------------------------------->| module flushes, syncs
  |                                 |
  | vsock: PODVM_COMMIT             |
  +-------------------------------->| module sends dirty list
  |                                 |
  | host: tar dirty -> new layer    |
  | host: new overlay = layers +    |
  |       new layer + fresh upper   |
  |                                 |
  | vsock: PODVM_RESUME             |
  +-------------------------------->| module remounts overlay
  |                                 | exec "npm install"
  |                                 | ... repeat
```

## Comparison: Docker Desktop vs podvm

```
Docker Desktop (macOS):
+------------------------------------------------------------------+
| macOS                                                            |
|  +--------------------------------------------------------------+|
|  | Linux VM (~500MB)                                            ||
|  |  +----------------------------------------------------------+||
|  |  | containerd (~50MB)                                        |||
|  |  | runc (~10MB)                                              |||
|  |  | namespaces + cgroups                                      |||
|  |  | overlayfs                                                 |||
|  |  | process                                                   |||
|  |  +----------------------------------------------------------+||
|  +--------------------------------------------------------------+|
+------------------------------------------------------------------+

podvm (Linux):
+------------------------------------------------------------------+
| Linux host                                                       |
|  +--------------------------------------------------------------+|
|  | Linux VM (~2MB kernel)                                       ||
|  |  +----------------------------------------------------------+||
|  |  | init (~10KB)                                              |||
|  |  | PID namespace                                             |||
|  |  | virtiofs mount                                            |||
|  |  | chroot + exec                                             |||
|  |  +----------------------------------------------------------+||
|  +--------------------------------------------------------------+|
+------------------------------------------------------------------+
```
