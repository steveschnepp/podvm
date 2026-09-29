# ADR-005: OCI Image Compatibility with Podman Storage

**Date:** 2024-09-29

**Status:** Accepted

## Context

Container images follow the OCI (Open Container Initiative) image format specification. Different runtimes store images differently: Docker uses its own graphdriver storage, Podman uses containers/storage, and containerd uses content-addressable stores.

A new container runtime risks creating a siloed image store. Users would need to pull images separately for podvm versus Docker/Podman, doubling storage usage and complicating workflows.

The ideal approach is to read the same image storage as an existing runtime. Podman's containers/storage library stores images in a well-documented format with JSON metadata and extracted layer directories.

## Decision

podvm will read and share Podman's container storage at /var/lib/containers/storage. Images pulled by Podman are immediately available to podvm and vice versa. No image conversion or re-pulling is required.

## Rationale

Podman's storage format is well-documented and stable. Layers are stored as extracted directories under /var/lib/containers/storage/overlay/layer/. Metadata is in JSON files. This format has been stable for years and is shared across Podman, Buildah, and Skopeo.

Shared storage means podvm inherits Podman's ecosystem. `podman pull nginx:alpine` makes the image available to `podvm run nginx:alpine`. Users do not need to learn new image management commands. Buildah can build images that podvm runs.

The layer directories are directly usable as virtiofs shares. No conversion to QCOW2 or other VM-specific formats is needed. The host reads the extracted layer path and mounts it read-only inside the guest. This eliminates an entire conversion step.

Storage deduplication works automatically. If both Podman and podvm use the same image, the layers exist once on disk. There is no duplication penalty for using both runtimes on the same host.

## Implications

podvm must respect Podman's storage locking mechanisms. containers/storage uses file locks to prevent concurrent modifications. The daemon must acquire appropriate locks when reading layer metadata.

Storage driver compatibility must be maintained. Podman supports overlay, btrfs, zfs, and vfs drivers. podvm should support at least overlay (default) initially, with other drivers as needed. The overlay manager abstraction layer handles this.

Changes to Podman's storage format would require podvm updates. The risk is low given the format's stability, but the storage interface should be isolated behind a clean API boundary to minimize impact.

Root ownership of storage directories must be coordinated. Podman rootful and rootless modes use different storage paths. podvm should detect and use the correct path based on the calling user's context.

## Alternatives Considered

**Separate image store:** Rejected. Doubles storage usage, requires separate pull commands, fragments the image ecosystem. No benefit over shared storage.

**containerd as image backend:** Considered but rejected. containerd's content-addressable store is powerful but adds a dependency. Podman's storage is simpler and already widely deployed.

**Docker image format:** Rejected. Docker's storage format is less documented and changes more frequently. Podman's format is the de facto standard for non-Docker OCI tooling.
