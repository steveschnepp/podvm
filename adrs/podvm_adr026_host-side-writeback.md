# ADR-026: Host-Side Overlay Write-Back via Named Containers

**Date:** 2026-10-08

**Status:** Implemented

## Context

When podvm runs an image from podman storage (ADR-024), it mounts the
image's layer chain as an overlayfs inside the QEMU child's user
namespace and serves the merged view to the guest over 9p (ADR-022). The
overlay needs a writable upper layer. podvm placed that upper on a
private tmpfs, so every in-guest write vanished when the VM exited. That
is correct for a throwaway `run`, but it means a container cannot keep
any state across invocations -- no caches, no databases, no build output.

ADR-020 anticipated this and proposed a guest kernel module
(podvm-overlay.ko) that assembles the overlay *inside* the guest, tracks
dirty files via VFS hooks, and commits them back to the host as new OCI
layers over vsock. That design was written for the virtiofs era and for
`podvm build`, where a Dockerfile's RUN instructions each need a layer
commit.

The architecture that actually got built diverged in two ways that make
the guest module the wrong tool here. First, the transport is 9p serving
a *host-side* overlay, not virtiofs serving per-layer mount tags; the
guest never sees layer paths, so a guest module has nothing to assemble.
Second, the guest kernel is now the *host's* kernel for the common boot
path (ADR-018/ADR-022), not a synthetic kernel podvm controls, so
"just compile a module into it" no longer holds.

What was needed first was the smaller, more common capability: keep a
container's writable layer on the host so it survives the VM, without
changing the guest at all.

## Decision

Implement write-back host-side, keyed by a container name. `podvm -n
<name> ...` places the overlay's upper and work directories in a
persistent per-container state directory on the host instead of a tmpfs:

    $XDG_STATE_HOME/podvm/containers/<name>/{u,w}
    (defaulting to ~/.local/state/podvm/containers/...)

Because the overlay is mounted in the QEMU child's userns, where the
invoking uid maps to root, files created in that upper are owned by the
invoking user outside -- no root-owned state is left behind. Writes made
by the container land in the host directory and are visible to the next
run that uses the same name. Without `-n`, the upper stays on tmpfs and
the historical throwaway behavior is preserved exactly.

This is deliberately opt-in per name: an unnamed run is still ephemeral,
and two runs sharing a name share state, matching how docker/podman
treat a named container's writable layer.

## Rationale

This keeps the isolation boundary where ADR-001 and ADR-024 already put
it: the VM is the boundary, and the host-side overlay is staging, not a
trust change. Persisting that overlay to a host directory the user owns
introduces no new privilege -- the directory is written as the invoking
user via the existing userns mapping.

It is also the minimal change that unlocks the capability. The guest is
untouched; no kernel module, no vsock commit protocol, no dirty-file
tracking. Overlayfs already writes every change to the upper as it
happens, so "persistence" is just *not throwing the upper away*. A
kernel module would add a VFS-hook overhead (ADR-020 estimates 5-10% on
write-heavy workloads) to solve a problem -- *when* to commit -- that a
persistent upper does not have.

Opt-in via `-n` avoids surprising existing callers: scripts that relied
on a clean slate each run keep getting one. Sharing state by name is the
convention every container user already expects.

## Implications

State lives under `$XDG_STATE_HOME`, so it is per-user and follows the
XDG convention; `podvm` never cleans it up. Deleting the directory is
how a container's state is removed (there is no `podvm rm` yet). The
directory can grow without bound as the container writes -- this is the
same characteristic as any container writable layer, and OCI layer
committing (the part of ADR-020 aimed at `podvm build`) is the natural
next step to compact it.

Concurrent runs sharing a name would share an overlay upper, which
overlayfs does not support for two writers. This is not yet guarded; the
convention (one live container per name) is the same one docker
enforces, but podvm does not currently detect the conflict.

ADR-020 is **not superseded** in full. Its guest-module design remains
the plan for `podvm build` layer commits, which this does not address.
This ADR supersedes ADR-020 only for the "`podvm run` needs a writable
layer that persists" case, and records that the module's original
premise -- a synthetic guest kernel and virtiofs-served layers -- no
longer describes the built system. ADR-020 should be revisited before
`podvm build` is attempted, to re-evaluate whether a guest module is
still the right shape given 9p and the host kernel.

## Alternatives Considered

**Guest kernel module (ADR-020 as written).** The original plan. Deferred
for `run`: it solves layer *committing*, which a persistent upper does
not need, and its premise (guest-assembled overlays over virtiofs, a
podvm-controlled synthetic kernel) does not match the 9p + host-kernel
reality. It remains the reference for `podvm build`.

**Committing the upper to a new OCI layer on every exit.** Rejected as
the default: most runs are ephemeral, and synthesizing a layer per exit
is wasted work when simply retaining the directory is free. It is the
right *complement* later (to compact a long-lived container's state),
not a substitute for persistence.

**A vsock-driven save/restore protocol.** Rejected: the guest already
writes through 9p to the overlay, so the host has the data the moment
it is written. A protocol to ask the guest to flush would be redundant
with what overlayfs already guarantees.
