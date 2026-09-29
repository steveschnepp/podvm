# ADR-017: PID Namespace for PID 1 Illusion

**Date:** 2024-09-29

**Status:** Accepted

## Context

In a real container, the entrypoint process is PID 1. This is not just a number - it affects behavior:

1. `/proc/1/cmdline` shows the entrypoint command
2. Signals to PID 1 propagate to the child process
3. PID 1 has special signal handling (does not die on SIGTERM unless it has a handler)
4. Some applications check their own PID for container detection
5. Orphan processes are reparented to PID 1
6. zombie reaping is PID 1's responsibility

In podvm's guest, the init process naturally runs as PID 1. The user's entrypoint becomes PID 2. This breaks applications that expect PID 1 behavior and container detection logic.

A PID namespace solves this. The guest init creates a new PID namespace before execing the entrypoint. Inside the new namespace, the entrypoint is PID 1. The init process exists outside the namespace as PID 1 in the root namespace but is invisible to the user.

## Decision

The guest init will create a PID namespace before execing the entrypoint. The entrypoint runs as PID 1 inside the namespace. /proc is mounted inside the namespace to reflect the correct PID numbering.

## Rationale

PID namespaces are the only namespace podvm needs inside the guest. The VM already provides:

- Process isolation: separate kernel, separate process table
- Mount isolation: separate filesystem (via virtiofs + chroot)
- Network isolation: separate network stack (via virtio-net)
- User isolation: separate user namespace (VM is its own universe)

The only thing missing is the PID 1 convention. A PID namespace provides this with minimal overhead. No other namespaces are needed.

The overhead is negligible. Creating a PID namespace requires one clone() call and one mount() call. This adds ~1ms to container startup. The namespace itself uses minimal kernel memory (one pid_namespace struct, ~400 bytes).

PID namespace semantics match real containers exactly. The entrypoint sees itself as PID 1. Orphan processes are reparented to it. zombie reaping works correctly. /proc/1/ shows the entrypoint. Container detection tools (like `is-container`) work correctly.

The init process remains PID 1 in the root namespace. It can still communicate with the host via vsock. It can still forward signals. It is invisible to the user but present for management.

## Implications

The guest init must use clone() with CLONE_NEWPID. This requires the clone() syscall and PID namespace support in the guest kernel. CONFIG_NAMESPACES=y and CONFIG_PID_NS=y are required in the synthetic kernel config.

/proc must be mounted inside the new PID namespace. If /proc is mounted before creating the namespace, it shows incorrect PIDs. The init must: (1) create PID namespace, (2) mount /proc inside namespace, (3) exec entrypoint.

Signal handling becomes more complex. The init process (PID 1 in root namespace) must forward signals to the entrypoint (PID 1 in child namespace). This is standard practice for container runtimes and adds ~20 lines of code.

Process exit handling requires wait(). The init must wait for the entrypoint to exit and forward the exit status to the host. This is also standard practice.

Multiple processes inside the container are supported. The entrypoint (PID 1 in namespace) can fork children. They appear as PID 2, 3, etc. inside the namespace. This matches real container behavior.

## Alternatives Considered

**No PID namespace:** Rejected. The entrypoint would be PID 2. /proc/1/ would show the init, not the entrypoint. Container detection would fail. Signal semantics would differ from real containers.

**All namespaces (unshare):** Considered but rejected. Creating mount, network, and user namespaces inside the guest is redundant. The VM already provides these isolations. Only PID namespace is needed for the PID 1 convention.

**chroot without PID namespace:** Rejected. chroot provides filesystem isolation but not PID isolation. The entrypoint would see the full guest process table (including the init). PID 1 would still be the init.

**exec as PID 1 directly:** Considered. The init could exec the entrypoint directly, becoming PID 1. However, this loses the ability to handle vsock messages, forward signals, and manage the container lifecycle. The init must remain running for management.

**Systemd as PID 1:** Considered for future. Systemd provides process supervision, cgroup management, and logging. However, systemd is ~50MB and slow to boot. For the MVP, a minimal init is sufficient. Systemd could be an option for advanced users.
