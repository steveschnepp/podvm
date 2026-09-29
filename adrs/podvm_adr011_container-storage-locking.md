# ADR-011: Container Storage Locking for Concurrent Access

**Date:** 2024-09-29

**Status:** Accepted

## Context

podvm shares container storage with Podman. Both runtimes may access the same layer directories, metadata files, and image manifests concurrently. Without proper locking, concurrent access can corrupt metadata, delete in-use layers, or create inconsistent states.

The containers/storage library uses file-based locking (flock) on metadata directories. These locks are well-tested and used by Podman, Buildah, and Skopeo. podvm must respect the same locking protocol.

The challenge is that podvm's daemon (podvmd) performs many storage operations: image resolution, layer path lookup, overlay assembly, and cleanup. Each operation must be properly locked to prevent races with Podman or other podvm instances.

## Decision

podvm will use the containers/storage library's locking mechanisms for all storage operations. The daemon acquires appropriate locks (shared for reads, exclusive for writes) before accessing storage metadata. Lock granularity follows containers/storage conventions.

## Rationale

Using the same locking as Podman prevents corruption. If Podman is pulling an image while podvm is reading layer metadata, both must hold appropriate locks. Ignoring locks would create race conditions that are difficult to reproduce and diagnose.

File-based locking is simple and debuggable. Lock files exist on disk and can be inspected with `ls -la` and `fuser`. No external lock service (etcd, Consul) is required. This matches Podman's deployment model.

Shared locks for reads allow concurrent access. Multiple podvm instances (or podvm + Podman) can read layer metadata simultaneously. This is the common case: many containers reading the same image layers.

Exclusive locks for writes prevent corruption. When podvm creates a container (writes upper directory metadata), it holds an exclusive lock. This prevents concurrent modification of the same container's metadata.

Lock timeout prevents deadlocks. If a lock cannot be acquired within a reasonable time (e.g., 30 seconds), the operation fails with a clear error. This prevents indefinite hangs from stuck processes.

## Implications

The daemon must be aware of lock contention. If Podman holds a lock for an extended period (large image pull), podvm operations may queue. Monitoring lock wait times helps identify contention issues.

Nested locking must follow a consistent order. If multiple locks are needed (image lock, layer lock, container lock), they must be acquired in the same order everywhere. Inconsistent ordering causes deadlocks.

The lock files live in the storage directory. If the storage directory is on a network filesystem (NFS), locking may not work correctly. containers/storage documents this limitation. podvm should warn if the storage directory is on NFS.

Crash recovery requires lock cleanup. If podvmd crashes while holding a lock, the lock file may be stale. flock locks are released on process exit, so this is typically handled by the kernel. Advisory locks may need manual cleanup.

## Alternatives Considered

**No locking (optimistic):** Rejected. Race conditions are inevitable with concurrent access. The cost of debugging corruption issues far exceeds the cost of proper locking.

**Database-backed storage:** Considered but rejected. A SQLite database could provide ACID guarantees. However, this diverges from Podman's storage format. Shared storage requires shared locking.

**External lock service:** Rejected. etcd, Consul, or Redis add infrastructure dependencies. File-based locking is sufficient for single-host operation.
