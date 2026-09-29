# ADR-010: No Container Runtime Inside the Guest

**Date:** 2024-09-29

**Status:** Accepted

## Context

Traditional container runtimes inside VMs (Docker Desktop, Kata Containers) include a full container runtime stack: containerd, runc (or similar), and associated tooling. This stack manages namespaces, cgroups, and the container lifecycle inside the VM.

podvm takes a different approach: the host manages everything. The guest is a minimal execution environment that receives a filesystem and runs a process. There is no container runtime inside the guest because the VM itself provides isolation.

The question is whether to include any container runtime components (containerd, runc, crun) inside the guest, even optionally.

## Decision

The podvm guest will not include any container runtime components. The guest init is a single ~7KB binary that mounts virtiofs, chroots, and execs. No containerd, no runc, no crun, no namespace setup, no cgroup management.

## Rationale

The VM boundary eliminates the need for namespace-based isolation. Namespaces are a software mechanism for isolating processes on a shared kernel. With a VM, each process set has its own kernel. There is no shared kernel to protect, so namespaces are unnecessary.

cgroups are unnecessary because the hypervisor handles resource limits. CPU and memory limits are VM-level parameters (vCPUs, RAM). The hypervisor enforces these limits at the hardware level. There is no need for cgroup hierarchies inside the guest.

seccomp is unnecessary because the VM boundary filters syscalls. The guest kernel only sees virtio devices. There is no attack surface to filter. The hypervisor provides stronger isolation than any seccomp profile.

Removing the container runtime reduces the guest to a single binary. This minimizes the trusted computing base inside the guest. The init is small enough to audit completely. There is no complex state machine, no OCI bundle parsing, no namespace setup code.

The host already performs all container management. Layer resolution, overlay assembly, network allocation, and lifecycle management happen on the host. The guest does not need to understand containers. It only needs to execute a process with a specific filesystem view.

## Implications

`podvm exec` cannot work by executing a process in the existing container's namespaces. Instead, exec must start a new process inside the same VM, sharing the same filesystem and network. This requires either a vsock-based protocol or a shell inside the guest.

Debugging inside the guest is limited without a shell. For development, busybox can be included in the initramfs (~500KB). For production, errors are logged to the serial console and virtio-console.

Container introspection (ps, top, strace) from the host is limited. The host cannot see processes inside the VM without guest cooperation. A future guest agent could expose process information via vsock.

The guest cannot perform container operations (pull images, create containers). This is by design. podvm is not a container runtime inside a VM. It is a VM-based container runtime where the host manages everything.

## Alternatives Considered

**containerd inside guest:** Rejected. Adds ~50MB to the initramfs. Requires containerd daemon management inside the VM. Adds complexity without benefit when the host already manages container storage.

**runc inside guest:** Considered for exec support. runc could execute processes in new namespaces inside the VM. But namespaces inside a VM are redundant with VM isolation. The added complexity does not justify the marginal benefit.

**CRI-O inside guest:** Rejected. CRI-O is a Kubernetes-oriented runtime. It requires a CRI client and kubelet integration. This is orthogonal to podvm's single-container-per-VM model.

**Shell as default:** Considered but rejected. Including a shell (busybox) adds 487KB to the initramfs. The shell is not needed for normal operation. It can be included in debug builds.
