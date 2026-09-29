# ADR-001: Use VMs Instead of Namespaces for Container Isolation

**Date:** 2024-09-29

**Status:** Accepted

## Context

Container runtimes like Docker and Podman use Linux namespaces (PID, net, mnt, uts, ipc, user) and cgroups to isolate processes. This approach shares a single kernel between all containers, which creates a large attack surface. Kernel vulnerabilities in namespace handling, seccomp bypass, or cgroup escapes have led to real-world container breakouts.

Docker Desktop on macOS already solves this by running containers inside a Linux VM. The VM provides hardware-level isolation while maintaining container UX. However, Docker Desktop's approach includes a full container runtime (containerd + runc) inside the VM, adding redundancy when running on a Linux host.

The question is whether to use full VMs as the primary isolation primitive for container workloads on Linux, eliminating the need for namespace-based isolation entirely.

## Decision

podvm will use KVM-based virtual machines as the isolation boundary for each container. Each container runs in its own lightweight VM with a synthetic kernel (virtio-only drivers). The container filesystem is provided to the guest via virtiofs mounts from the host.

## Rationale

The VM boundary provides stronger isolation than namespaces. A guest kernel compromise does not affect the host kernel or other containers. This eliminates entire classes of vulnerabilities: namespace escapes, cgroup escapes, seccomp bypasses, and user namespace mapping bugs.

Docker Desktop on macOS proves this approach works in production. The UX is identical to native containers. The difference is the isolation boundary moves from software (namespaces) to hardware (KVM).

Host-side overlay assembly eliminates redundancy. The host already understands OCI images and layer ordering. By assembling the overlay on the host and sharing the merged result via virtiofs, the guest needs no container runtime, no namespace setup code, and no layer management. The guest init is ~7KB of static C.

The per-VM overhead is acceptable for most workloads. ~15MB memory and ~100ms boot time compare favorably to Docker Desktop's ~500MB and ~3s cold start. Workloads requiring maximum density (hundreds of containers on one host) may need namespace mode, but most production deployments run tens of containers where VM isolation is preferable.

## Implications

Each container consumes ~15MB memory for the VM, compared to ~1MB for a namespace container. This is the cost of stronger isolation. Memory-constrained environments must account for this overhead when sizing hosts.

The synthetic kernel is shared across all VMs. Kernel updates require rebuilding one kernel image and restarting VMs. There is no live kernel patching for guests. This is acceptable because the kernel is minimal (~2MB, ~30 config options) and rebuilds are fast.

Virtiofs is the critical dependency. If virtiofs performance is insufficient for a workload, a 9p fallback exists but with ~50% throughput. Storage-heavy workloads should be benchmarked against the virtiofs overhead.

Nested virtualization (running podvm inside a VM) requires the outer VM to expose VMX/SVM extensions. This is supported by major cloud providers but adds complexity. Documentation should clearly state this requirement.

## Alternatives Considered

**Namespace-based isolation (Podman rootless):** Rejected. While rootless containers improve security, they still share the host kernel. Namespace vulnerabilities remain exploitable. The attack surface is larger than VM isolation.

**gVisor:** Rejected. gVisor intercepts syscalls and reimplements kernel functionality in userspace. This adds complexity and compatibility issues. VM isolation is simpler and more complete.

**Kata Containers:** Considered but rejected. Kata provides VM-based containers but includes a full containerd + agent stack inside the guest. podvm's approach is simpler: a 7KB init binary instead of a container runtime. Kata's complexity is unnecessary when host-side overlay assembly handles the filesystem.

**Docker Desktop's approach (VM + containerd inside):** Rejected for Linux. This adds a redundant container runtime layer. On Linux, the host already manages container storage. Running containerd inside the VM duplicates this functionality.
