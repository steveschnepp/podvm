# ADR-015: DAX virtiofs for Memory Deduplication

**Date:** 2024-09-29

**Status:** Accepted

## Context

virtiofs by default uses a page cache inside the guest. When a guest reads a file via virtiofs, the data is copied into the guest's page cache. If multiple VMs read the same file (e.g., shared container base layers), each VM caches its own copy.

For podvm, this defeats the purpose of shared layers. A 40MB base layer read by 10 VMs consumes 400MB of host memory (10 × 40MB copies), even though the data is identical. The layers are shared on disk but duplicated in memory.

This is the fundamental tension between VM isolation and memory efficiency. Containers share page cache naturally because they share the kernel. VMs do not share page cache because each has its own kernel and address space.

## Decision

podvm will use virtiofs with DAX (Direct Access) mode for read-only container layers. DAX maps guest file access directly to host memory without copying into the guest page cache. This provides single-copy semantics: one copy of each layer exists in host memory, accessible to all VMs.

## Rationale

DAX eliminates the duplication problem entirely. When a guest reads a file via DAX-enabled virtiofs, the access goes directly to the host's page cache. There is no copy into guest memory. The same physical memory page serves all VMs reading the same file.

The guest kernel maps the virtiofs shared region directly into its address space. File reads are memory reads to host-backed pages. This is faster than traditional virtiofs (no copy overhead) and uses less memory (no duplication).

Lower container layers are ideal for DAX. They are read-only, never modified, and shared across all containers from the same image. DAX provides perfect deduplication for these layers.

Upper layers (per-container writable) should use normal caching. Each container modifies its own upper layer independently. DAX is not beneficial for writable data that differs per container.

Cloud Hypervisor supports virtiofs DAX via the --dax flag. The guest kernel requires CONFIG_VIRTIO_FS_DAX=y and CONFIG_X86_PMEM_LEGACY=y. These are standard options with no compatibility concerns.

## Implications

DAX requires the virtiofs shared region to be mapped into the guest physical address space. This limits the total size of DAX-backed shares. The default DAX window is 256MB-1GB depending on hypervisor configuration. Multiple containers sharing large layers may need a larger window.

The guest kernel must support DAX. The synthetic kernel config adds CONFIG_VIRTIO_FS_DAX=y. This is a small addition (~5KB to kernel) but must be present for DAX to function.

DAX semantics differ from normal file I/O. Memory-mapped files via DAX are not subject to normal page cache behavior. Applications that rely on explicit cache control (msync, fadvise) may behave differently. For container workloads (running arbitrary binaries), this is typically transparent.

The balloon driver interacts with DAX differently. DAX-backed memory is not part of the normal guest page cache and cannot be reclaimed by balloon inflation. The balloon only affects normal guest memory (upper layers, application data). This is actually desirable: balloon reclaims application memory, not shared layer memory.

Host memory pressure management changes with DAX. Shared layers are pinned in host memory as long as any VM references them. The host cannot reclaim these pages without unmapping from all guests. This is similar to shared library mapping in processes.

## Alternatives Considered

**Read-only mount without DAX:** Considered. Mounting virtiofs read-only avoids write overhead but does not prevent guest page cache duplication. Each VM still caches its own copy of read data. DAX is required for true deduplication.

**Normal virtiofs with balloon reclaim:** Considered. The balloon can reclaim guest page cache memory. However, this is reactive (reclaim after allocation) rather than preventive (never allocate). DAX avoids the allocation entirely.

**QCOW2 with backing files:** Considered as an alternative to virtiofs. QCOW2 supports copy-on-write via backing chains. However, this requires converting OCI layers to QCOW2 format, adding a conversion step. virtiofs with DAX provides the same deduplication without conversion.

**9p filesystem:** Rejected. 9p does not support DAX. All file data is copied between host and guest. Performance is ~50% of native. virtiofs with DAX is both faster and more memory-efficient.

**NFS or shared filesystem:** Considered but rejected. NFS requires network configuration and has higher latency. A shared filesystem (Ceph, Gluster) adds infrastructure complexity. virtiofs is purpose-built for host-guest sharing.
