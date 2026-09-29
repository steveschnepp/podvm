# ADR-014: Memory Ballooning for Container-Level Efficiency

**Date:** 2024-09-29

**Status:** Accepted

## Context

The primary advantage of containers over VMs is memory efficiency. Containers share the host kernel and do not reserve fixed memory allocations. An idle container consumes minimal memory. A busy container can expand to use available host memory.

Traditional VMs allocate fixed memory. A VM configured with 128MB reserves 128MB even if the guest uses only 10MB. With many containers, this locked memory adds up quickly and negates the density advantage of containers.

Memory ballooning solves this. A balloon driver inside the guest cooperates with the hypervisor to dynamically adjust the guest's usable memory. When the guest is idle, the balloon inflates, marking pages as unused. The hypervisor reclaims those pages for other VMs or the host. When the guest needs memory, the balloon deflates, and the hypervisor returns pages.

## Decision

podvm will use virtio-balloon for dynamic memory management. Each VM starts with a configured memory limit (e.g., 128MB) but the balloon driver allows the host to reclaim unused memory. This provides container-like memory efficiency while maintaining VM isolation.

## Rationale

Memory efficiency is non-negotiable for podvm to compete with container runtimes. Without ballooning, 100 containers at 128MB each lock 12.8GB of host memory regardless of actual usage. With ballooning, idle containers release memory back to the host, and the same 100 containers might use only 1-2GB total.

The virtio-balloon driver is mature and widely supported. It has been in Linux since kernel 2.6.25. Cloud Hypervisor, QEMU, and Firecracker all support it. The guest kernel config requires only CONFIG_VIRTIO_BALLOON=y.

Ballooning is transparent to the container. The guest init and application process see no difference. Memory allocation and deallocation happen at the kernel level. Applications continue to use malloc/free normally.

The hypervisor manages balloon policy. Cloud Hypervisor supports deflate-on-oom and free-page-hinting. These features allow the guest to proactively release unused pages without explicit inflate commands. The host reclaims memory automatically.

## Implications

The balloon driver must be included in the synthetic kernel. CONFIG_VIRTIO_BALLOON=y adds minimal overhead (~10KB to kernel). The balloon device appears as /dev/vballoon in the guest.

Memory limits are still enforced. The balloon cannot cause the guest to exceed its configured maximum. If a guest needs 128MB, the balloon deflates fully. The maximum is a hard limit; ballooning only reduces actual usage below the maximum.

The host must track balloon state per VM. When memory pressure occurs, the host can inflate balloons in idle VMs to reclaim pages. This requires the daemon to monitor guest memory usage and balloon inflation/deflation statistics.

Overcommit is possible with careful tuning. If total VM memory limits exceed host physical memory, ballooning allows the host to manage the difference. This mirrors container memory overcommit behavior. However, aggressive overcommit risks OOM situations.

## Alternatives Considered

**Fixed memory allocation:** Rejected. This is the traditional VM model. It wastes memory and makes podvm less efficient than containers for memory-constrained workloads.

**KSM (Kernel Same-page Merging):** Considered as a complement. KSM deduplicates identical memory pages across VMs. It helps with multiple VMs running the same kernel but does not solve the fixed allocation problem. KSM can be used alongside ballooning.

**Memory hotplug:** Considered but rejected for MVP. Hotplug allows adding memory to a running VM. This is more complex than ballooning and does not address memory reclamation.

**No memory limit:** Rejected. Containers without memory limits can consume all host memory. VMs without limits risk host OOM. Limits are required for safe multi-tenant operation.
