# ADR-004: Cloud Hypervisor as Primary Hypervisor Backend

**Date:** 2024-09-29

**Status:** Accepted

## Context

podvm requires a hypervisor to run guest VMs. Several options exist for KVM-based virtualization on Linux:

1. QEMU: The reference implementation. Full device model, extensive configuration, mature codebase. Heavily threaded with significant memory overhead.
2. Cloud Hypervisor: Rust implementation by Intel. Modern design, minimal device model, virtiofs and vsock support built-in, REST API for management.
3. Firecracker: AWS implementation for Lambda/Fargate. Extremely fast boot, minimal overhead, limited device model. Virtiofs support is partial.
4. crosvm: Google implementation for Chrome OS. Sandboxed device model, limited community adoption.

The choice affects API design, feature availability, and maintenance burden.

## Decision

Cloud Hypervisor will be the primary hypervisor backend. QEMU will be supported as a fallback. Firecracker support is planned for future work.

## Rationale

Cloud Hypervisor provides a REST API for VM management. This simplifies the host launcher significantly. VM configuration, boot, shutdown, and device hotplug are all API calls. QEMU requires command-line argument construction and QMP socket communication for equivalent functionality.

Cloud Hypervisor has native virtiofs support via virtiofsd. The daemon can run host-side or be embedded. This matches podvm's host-side overlay assembly model. QEMU requires separate virtiofsd management and more complex configuration.

The device model is intentionally minimal. Cloud Hypervisor supports virtio devices (block, net, fs, console, rng, vsock) and nothing else. This matches podvm's synthetic kernel approach where only virtio drivers are needed. QEMU emulates hundreds of legacy devices that podvm never uses.

Rust implementation provides memory safety guarantees. The hypervisor is a critical security boundary. Memory corruption in the hypervisor can escape to the host. Rust eliminates entire classes of vulnerabilities that affect C codebases like QEMU.

Boot time is comparable to Firecracker for podvm's workload. The synthetic kernel boots in ~50-100ms regardless of hypervisor. Cloud Hypervisor's overhead over Firecracker (~10-20ms) is negligible for container workloads.

## Implications

Cloud Hypervisor requires KVM and a recent Linux host kernel (5.10+). Older systems must use the QEMU fallback. This should be documented and the launcher should detect KVM availability.

Cloud Hypervisor's feature set is smaller than QEMU. Advanced features like migration, huge pages, and VFIO passthrough may require QEMU for some use cases. The fallback architecture ensures these remain available.

The hypervisor binary must be installed on the host. podvm should either bundle Cloud Hypervisor or require it as an external dependency. Bundling simplifies deployment but increases binary size.

API stability between Cloud Hypervisor versions must be monitored. Breaking API changes would require launcher updates. Pinning to specific versions in documentation reduces support burden.

## Alternatives Considered

**QEMU (primary):** Considered but rejected for complexity. QEMU's command-line interface is notoriously complex. The QMP protocol is JSON-based but has edge cases. Cloud Hypervisor's REST API is simpler and more predictable.

**Firecracker (primary):** Rejected due to incomplete virtiofs support. Firecracker is optimized for AWS's use case (microVMs with minimal I/O). Podvm requires reliable virtiofs for container filesystem sharing. Firecracker support is planned as a secondary backend for latency-sensitive workloads.

**crosvm:** Rejected due to limited community and documentation. Google's internal focus makes external adoption difficult. The sandboxed device model is interesting but not critical for podvm's threat model.
