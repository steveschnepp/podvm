# ADR-008: Daemon-Based Architecture with podvmd

**Date:** 2024-09-29

**Status:** Accepted

## Context

Container runtimes face a design choice: process-per-command (like早期 Docker) or long-running daemon (like modern containerd/Podman). Each approach has trade-offs in resource usage, state management, and complexity.

podvm manages VMs, overlay mounts, network bridges, and port forwarding. These resources have lifecycle requirements that span multiple CLI invocations. A container started with `podvm run` must be stoppable with `podvm stop` minutes later.

The daemon can maintain state across commands, manage background resources, and handle signals gracefully. Without a daemon, each CLI invocation would need to discover and manage state independently.

## Decision

podvm will use a long-running daemon (podvmd) that manages container lifecycle, VM processes, network configuration, and image metadata. The CLI (podvm) communicates with the daemon over a Unix socket.

## Rationale

VM processes are long-lived. A container may run for days or weeks. The daemon tracks which VMs are running, their PIDs, resource usage, and exit status. Without a daemon, `podvm ps` would need to discover running VMs by scanning processes or reading state files.

Network configuration persists across commands. Port forwarding rules, bridge assignments, and IP allocations must survive CLI invocations. The daemon maintains iptables/nftables rules and cleans them up when containers stop.

Event streaming requires a long-lived process. Docker and Podman emit events (start, stop, die, oom) that monitoring tools consume. The daemon can buffer and broadcast events to multiple subscribers.

Graceful shutdown is simpler with a daemon. When the host shuts down, the daemon receives SIGTERM and can stop all VMs cleanly, flush virtiofs caches, and release network resources. Without a daemon, orphaned VMs could persist.

The daemon pattern is proven. Docker, Podman, and containerd all use daemons. The operational model is well-understood by users and tooling.

## Implications

The daemon is a single point of failure. If podvmd crashes, running containers continue but cannot be managed. Recovery requires the daemon to discover orphaned VMs on restart. State files in /var/lib/podvm/containers/ enable this.

Daemon startup adds latency to the first command. `podvm run` on a cold system must start the daemon, which then starts the VM. This adds ~100-200ms. Subsequent commands are fast because the daemon is already running.

The daemon requires root privileges (or appropriate capabilities) to manage KVM, configure bridges, and manipulate iptables. Rootless operation requires additional setup (user namespaces, TAP management) and is a future enhancement.

Socket permissions control access. The daemon socket at /run/podvm/podvm.sock should be readable only by the owning user (rootless) or appropriately restricted (rootful). This prevents unauthorized container management.

## Alternatives Considered

**Process-per-command:** Rejected. Each `podvm run` would fork a process that lives as long as the container. State management becomes complex: PID files, lock files, cleanup handlers. The daemon centralizes this logic.

**containerd as backend:** Considered but rejected. containerd provides container lifecycle management but adds complexity. podvm's VM-based model is simpler than containerd's namespace-based model. Using containerd would negate the simplicity benefit.

**Systemd unit per container:** Considered. Each container could be a systemd service. This provides lifecycle management via systemd but couples podvm to systemd. Not all hosts use systemd (containers, embedded systems, BSD-inspired setups).
