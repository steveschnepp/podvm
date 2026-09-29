# ADR-007: Synthetic Kernel with Virtio-Only Drivers

**Date:** 2024-09-29

**Status:** Accepted

## Context

Each podvm container runs in its own VM with its own kernel. Unlike traditional containers that share the host kernel, VM-based containers need a guest kernel. The guest kernel must support the virtual hardware provided by the hypervisor and nothing else.

The kernel configuration determines boot time, memory footprint, and compatibility. A full Linux distribution kernel includes drivers for hundreds of hardware devices, filesystems, and subsystems that a podvm guest never uses.

The guest kernel's job is narrow: mount virtiofs, chroot, and exec a process. It needs virtio drivers, basic filesystem support, and process management. Everything else is overhead.

## Decision

podvm will use a custom synthetic kernel built from upstream Linux source with a minimal configuration. The kernel includes only virtio drivers, essential filesystems (proc, sys, devtmpfs, tmpfs), and process primitives. No real hardware drivers, no network stack (beyond virtio-net), and no optional subsystems.

## Rationale

A minimal kernel boots faster. The synthetic kernel with ~30 config options boots in ~50-100ms. A full distribution kernel takes 200-500ms. For container workloads where containers are created and destroyed frequently, boot time matters.

Smaller kernel means smaller attack surface. The kernel is part of the trusted computing base. Every driver and subsystem is potential attack surface. By excluding unused code, the attack surface is reduced to only what is necessary for virtio operation.

Memory footprint is reduced. A minimal kernel uses ~2-3MB of memory versus ~10-15MB for a distribution kernel. Multiplied across many containers, this savings is significant.

Single kernel image simplifies management. One kernel binary is embedded in the podvm tool and shared across all VMs. Kernel updates apply to all containers simultaneously. There is no per-container kernel version drift.

Virtio drivers are the only hardware interface. The hypervisor presents virtio devices (block, net, console, fs, rng). The guest needs only virtio drivers. This is a stable, well-tested interface that does not change between kernel versions.

## Implications

Kernel updates require rebuilding the synthetic kernel and redistributing the podvm binary. This is more frequent than host kernel updates but affects only the guest kernel. The host kernel remains independent.

Live kernel patching (kpatch, kGraft) is not supported for the synthetic kernel. Patching requires rebuilding and restarting VMs. The minimal kernel surface means security patches are infrequent and rebuilds are straightforward.

Nested virtualization support requires enabling KVM Guest paravirt options. The kernel config includes CONFIG_KVM_GUEST=y for this purpose. If podvm runs inside a VM, the outer VM must expose hardware virtualization extensions.

The kernel does not include module support (CONFIG_MODULES=n). All code is built-in. This simplifies the initramfs (no /lib/modules) but requires a kernel rebuild for any config change.

## Alternatives Considered

**Distribution kernel:** Rejected. Includes thousands of unnecessary drivers and options. Boot time and memory overhead are unacceptable for per-container VMs.

**LinuxKit:** Considered but rejected. LinuxKit provides a toolkit for building minimal Linux kernels. The concept is aligned with podvm's needs, but the build system adds complexity. A direct kernel config provides more control.

**Firecracker kernel:** Considered. Firecracker provides a pre-built minimal kernel. This could work as a shortcut but ties podvm to Firecracker's kernel version and configuration choices. Building our own provides flexibility.

**Unikernel (MirageOS, IncludeOS):** Rejected. Unikernels compile the application with the kernel, producing a single-purpose binary. This does not work for podvm's model of running arbitrary container images.
