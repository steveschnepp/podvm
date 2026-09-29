# ADR-003: musl Static Linking for Guest Init

**Date:** 2024-09-29

**Status:** Accepted

## Context

The guest init is the only userspace binary in the podvm guest. It runs as PID 1 inside each VM. Its responsibilities are limited: read kernel cmdline, mount virtiofs, chroot, and exec the entrypoint. This is a small amount of work that could be implemented in assembly, raw syscalls, or a standard C library.

The binary runs inside an initramfs that is extracted into RAM at boot. Size matters because it is loaded for every container. The binary must also be self-contained with no external dependencies.

Three options exist for linking:

1. glibc static: Standard C library, ~500KB binary.
2. musl static: Minimal C library, ~7KB binary.
3. No libc (raw syscalls): Direct syscall wrappers, ~2KB binary.

## Decision

The guest init will be compiled with musl-gcc using static linking, -Os optimization, and aggressive stripping. The resulting binary is approximately 7KB.

## Rationale

musl provides standard C functions (strcmp, sscanf, mount, chroot) without the ~500KB overhead of glibc. The binary is self-contained with no shared library dependencies. This is critical for initramfs where every kilobyte counts.

Raw syscalls (option 3) would reduce size to ~2KB but require reimplementing string parsing, command line splitting, and error handling. The development and debugging cost is not justified for a 5KB difference. musl provides these utilities with negligible overhead.

musl's static linking is simpler than glibc's. glibc static binaries sometimes have issues with NSS, DNS resolution, and locale handling. Since the guest init does not perform DNS resolution or locale operations, this is not a problem, but musl avoids the complexity entirely.

The build flags (-fno-stack-protector, -fno-asynchronous-unwind-tables, -ffunction-sections, -fdata-sections, --gc-sections) strip all unnecessary features. The binary has no exception handling, no thread-local storage, and no debug symbols. This is appropriate for a single-purpose init that either succeeds or prints an error and exits.

## Implications

The guest init cannot use features not provided by musl: no locale support, no wide character handling, no complex math. These are not needed for the init's limited responsibilities.

Debugging inside the guest is limited. Without a shell (busybox adds 487KB), errors are reported to the serial console or virtio-console. For development, a debug build can include busybox in the initramfs (~500KB total).

The initramfs total size is ~10KB compressed: 7KB init binary plus empty directories. This loads into guest RAM in negligible time. The boot bottleneck is kernel decompression (~50ms), not initramfs extraction.

musl's libc version must be pinned in the build environment to ensure consistent behavior across builds. Musl provides stable ABI guarantees, so this is primarily for reproducibility.

## Alternatives Considered

**glibc static:** Rejected. 500KB binary for functions not used by the init. NSS and DNS resolution code is dead weight.

**No libc (raw syscalls):** Rejected for development velocity. The 5KB savings does not justify reimplementing string parsing. Considered acceptable if a security audit requires minimizing the trusted computing base.

**Busybox init:** Rejected as primary init. 487KB is too large for a minimal guest. Useful as an optional debug tool in the initramfs.
