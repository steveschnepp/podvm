# ADR-018: QEMU as Primary VMM

**Date:** 2024-09-29

**Status:** Accepted

## Context

A VMM (Virtual Machine Monitor) is required to create and manage VMs. The VMM handles hardware emulation, virtio device presentation, and communication with KVM.

Two primary options exist:

1. Cloud Hypervisor: Modern Rust implementation. Clean REST API. Smaller binary. Requires separate installation.
2. QEMU: Mature C implementation. CLI flags + QMP protocol. Ubiquitous on Linux. 20+ years of production use.

The choice affects deployment friction, debugging capability, and feature availability.

## Decision

podvm will use QEMU as the primary VMM for the MVP. Cloud Hypervisor support will be added later as an alternative backend.

## Rationale

QEMU is already installed on most Linux systems. Debian, Ubuntu, Fedora, Arch, RHEL all ship QEMU packages. Users can run `podvm run` immediately after installing podvm. No additional VMM installation is required.

QEMU is battle-tested. Millions of production systems run QEMU. Edge cases in virtio device emulation, interrupt handling, and memory management have been found and fixed over 20 years. Cloud Hypervisor is younger and has less production mileage.

Documentation and community support are extensive. Stack Overflow, mailing lists, and bug trackers have extensive QEMU knowledge. Debugging VMM issues is easier with QEMU because more people have encountered and solved similar problems.

QEMU provides the same virtio devices as Cloud Hypervisor. virtio-blk, virtio-net, virtio-console, virtio-fs, virtio-balloon, vsock are all supported. The guest kernel sees identical devices regardless of VMM.

The --nographic flag provides a simple serial console. Combined with -serial mon:stdio, guest output appears on the host terminal directly. This is simpler than Cloud Hypervisor's separate socket management.

## Implications

QEMU's CLI is more verbose than Cloud Hypervisor's REST API. The VM launcher must construct longer command lines. This is manageable with a build function that generates the QEMU arguments.

QEMU binary is larger (~10MB) than Cloud Hypervisor (~5MB). This does not affect podvm's binary size since QEMU is an external dependency. It affects installation size on the host.

QEMU startup is slightly slower (~200ms vs ~100ms for Cloud Hypervisor). This is acceptable for the MVP. The total container startup time is dominated by image pull and overlay assembly, not VMM boot.

QEMU's QMP (QEMU Machine Protocol) is JSON-based and provides full VM management. This can be used for future features like hot-add memory, device hotplug, and migration. Cloud Hypervisor's REST API is simpler but less featureful.

QEMU supports both PCI and MMIO transports for virtio devices. The default is PCI, which matches Cloud Hypervisor. The guest kernel config needs CONFIG_VIRTIO_PCI=y but not CONFIG_VIRTIO_MMIO=y unless Firecracker support is added later.

## Alternatives Considered

**Cloud Hypervisor:** Considered for the cleaner API and faster boot. Rejected for MVP due to additional installation requirement. Can be added as an alternative backend after MVP validation.

**Firecracker:** Considered for fastest boot. Rejected due to incomplete virtiofs support and limited device model. Better suited for microVMs than container workloads.

**Direct KVM:** Considered for minimal overhead. Rejected due to complexity of writing a VMM from scratch. QEMU/Cloud Hypervisor handle thousands of edge cases in device emulation.

**crosvm:** Considered for sandboxed device model. Rejected due to limited community and documentation. Google-centric project with limited external adoption.

**Xen, KVM tool, lkvm:** Considered but rejected. These are either less featureful than QEMU or less maintained. QEMU remains the reference implementation for KVM-based virtualization.
