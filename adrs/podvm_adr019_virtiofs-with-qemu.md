# ADR-019: virtiofs with QEMU via virtiofsd

**Date:** 2024-09-29

**Status:** Accepted

## Context

QEMU supports virtiofs through a separate daemon called virtiofsd. The daemon runs on the host and serves the filesystem over a vhost-user socket. QEMU presents this socket to the guest as a virtio-fs device.

This is different from Cloud Hypervisor, which can embed virtiofsd or use it externally with simpler configuration. QEMU requires explicit memory backend configuration and vhost-user setup.

The trade-off is performance vs complexity. virtiofs provides ~80% native filesystem performance. 9p provides ~50%. For container workloads that read many small files (package managers, dependency resolution), the difference is noticeable.

## Decision

podvm will use virtiofs with QEMU for container filesystem sharing. virtiofsd runs as a child process of podvmd, one instance per container. The shared directory is the assembled overlay merged directory.

## Rationale

virtiofs performance is critical for container workloads. `apt-get update` reads hundreds of small files. `npm install` reads thousands. At 50% throughput (9p), these operations take twice as long. At 80% (virtiofs), the difference is acceptable.

virtiofsd is a single binary provided by the QEMU package. On Debian/Ubuntu, it is installed as part of qemu-system. No additional installation is required. The daemon is simple: it listens on a Unix socket and serves files from a specified directory.

The QEMU configuration is more verbose but manageable. The launcher generates the QEMU command line including:
- memory-backend-memfd with share=on (required for virtiofs)
- chardev socket pointing to virtiofsd
- vhost-user-fs-pci device with tag and chardev

This is a one-time setup in the VM launcher. The complexity is hidden from users.

virtiofsd runs per-container, not per-host. Each container gets its own virtiofsd instance serving its merged directory. This provides isolation and allows different containers to have different filesystem views.

## QEMU Configuration

```bash
# Full QEMU command with virtiofs + balloon + QMP
qemu-system-x86_64 \
  -machine q35,accel=kvm \
  -cpu host \
  -m 128 \
  -smp 1 \
  \
  # Memory backend (required for virtiofs)
  -object memory-backend-memfd,id=mem,size=128M,share=on \
  -numa node,memdev=mem \
  \
  # virtiofs device
  -chardev socket,id=char0,path=/run/podvm/<id>/virtiofs-rootfs.sock \
  -device vhost-user-fs-pci,chardev=char0,tag=rootfs \
  \
  # Memory balloon
  -device virtio-balloon-pci \
  \
  # QMP control socket
  -qmp unix:/run/podvm/<id>/qmp.sock,server,nowait \
  \
  # Kernel and init
  -kernel /var/lib/podvm/kernel/bzImage \
  -initrd /var/lib/podvm/kernel/initramfs.cpio.gz \
  -append "podvm.root=/rootfs podvm.entrypoint=/bin/sh podvm.args=-c,echo,hello" \
  \
  # Serial console
  -serial mon:stdio \
  -nographic \
  -no-reboot
```

## virtiofsd Invocation

```bash
# One virtiofsd per container
virtiofsd \
  --socket-path=/run/podvm/<id>/virtiofs-rootfs.sock \
  --shared-dir=/var/lib/podvm/containers/<id>/merged \
  --cache=auto \
  --sandbox=none
```

## Implications

virtiofsd must be started before QEMU. The socket must exist when QEMU boots. The launcher starts virtiofsd, waits for the socket, then starts QEMU.

virtiofsd is a child process of the daemon. If the daemon crashes, virtiofsd instances are cleaned up. If virtiofsd crashes, the VM loses filesystem access and should be stopped.

Memory configuration is non-trivial. The -object memory-backend-memfd with share=on is required for virtiofs. Without it, the guest cannot access the shared filesystem. The -numa node,memdev=mem parameter connects the memory backend to the VM.

The guest kernel must have virtiofs support. CONFIG_VIRTIO_FS=y is required. DAX support (CONFIG_VIRTIO_FS_DAX=y) is optional but recommended for memory efficiency.

virtiofsd has a --sandbox=none flag that disables seccomp filtering inside the daemon. This is required for QEMU integration. The daemon runs with reduced privileges anyway (dropped capabilities, chroot to shared directory).

## Alternatives Considered

**9p filesystem:** Considered for simplicity. No daemon required. Works with QEMU out of the box. Rejected due to 50% throughput penalty. Package managers and dependency installers are noticeably slower.

**QCOW2 with backing files:** Considered. Convert OCI layers to QCOW2, use QEMU's native block device support. Rejected due to conversion overhead and complexity. virtiofs provides the same deduplication without conversion.

**NFS mount:** Considered. Mount container layers via NFS from the host. Rejected due to network configuration requirements and performance overhead. virtiofs is purpose-built for host-guest sharing.

**Share host filesystem directly:** Considered. Mount /var/lib/containers/storage directly into the guest. Rejected because the overlay is assembled on the host. The guest needs the merged view, not raw layers.
