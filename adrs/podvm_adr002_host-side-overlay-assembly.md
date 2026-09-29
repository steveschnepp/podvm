# ADR-002: Host-Side Overlay Assembly

**Date:** 2024-09-29

**Status:** Accepted

## Context

Container images consist of stacked layers. At runtime, these layers are combined using an overlay filesystem: lower layers provide read-only base content, an upper layer captures writes, and a merged view presents the unified filesystem to the process.

Two approaches exist for providing this filesystem to a VM guest:

1. Pass individual layer directories to the guest via virtiofs and assemble the overlay inside the guest.
2. Assemble the overlay on the host and pass the merged directory to the guest.

The first approach requires the guest to understand layer ordering, mount overlayfs, and manage upper/work directories. The second approach requires only a single virtiofs mount of the already-assembled result.

## Decision

The host will assemble the overlay filesystem for each container. The guest receives a single virtiofs share pointing to the merged directory. The guest init performs only chroot and exec.

## Rationale

The host already resolves OCI images and knows the layer order. The container storage backend (shared with Podman) maintains this metadata. Assembling the overlay on the host avoids duplicating this logic in the guest.

The guest init becomes trivially simple: mount one virtiofs share, chroot, exec. This is ~50 lines of C. The alternative (guest-side assembly) would require parsing layer manifests, handling multiple virtiofs shares per layer, and implementing overlay mount logic in the guest. This adds complexity for no benefit.

Host-side assembly means the guest needs no overlayfs kernel support. The synthetic kernel config becomes smaller. The guest does not need to know about layers at all. It sees a complete filesystem.

Performance is comparable. The overlay mount exists on the host either way. The virtiofs daemon serves file reads from the merged directory regardless of where the mount lives. The additional hop through virtiofs is identical in both approaches.

## Implications

Each container requires a unique merged directory on the host. The overlay manager creates per-container upper and work directories under /var/lib/podvm/containers/<id>/. These are cleaned up when the container is removed.

Concurrent access to lower layers is safe. Overlayfs supports multiple merged views sharing the same lower layers. Multiple containers from the same image share the lower layers but have independent upper layers.

The host must have sufficient file descriptors for all active overlay mounts. The default Linux limit (typically 1024 per process, or higher with sysctl) should be verified for deployments with many concurrent containers.

Container writes go through virtiofs to the host upper directory. This adds latency compared to native overlayfs writes inside a container. Write-heavy workloads should be benchmarked. The upper directory can be placed on fast local storage (NVMe) to mitigate this.

## Alternatives Considered

**Guest-side overlay assembly:** Rejected. Would require overlayfs support in the guest kernel, multiple virtiofs shares per container (one per layer), and layer ordering logic in the guest init. Adds complexity without clear benefit.

**9p shared filesystem:** Considered as fallback. 9p is simpler than virtiofs but has ~50% throughput. virtiofs is preferred for performance; 9p can serve as compatibility fallback for older kernels.
