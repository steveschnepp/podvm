# ADR-009: Bridge Networking as Default

**Date:** 2024-09-29

**Status:** Accepted, but deferred: the functional phase uses QEMU
user-mode networking first (ADR-023). This ADR remains the design of
record for bridge/tap networking in the performance phase.

## Context

Containers require networking to communicate with each other and with external hosts. Several networking models exist for VM-based containers:

1. Bridged networking: Containers connect to a bridge (virtual switch). The bridge routes traffic between containers and to external networks via NAT.
2. MACVTAP: Containers get macvtap interfaces directly on the physical network. No NAT, but no inter-container routing without external switches.
3. Host networking: Container shares the host's network namespace. No isolation.
4. CNI plugins: Pluggable networking via the Container Network Interface standard.

The default must work out of the box without external dependencies. Advanced networking should be available via plugins or configuration.

## Decision

Bridge networking will be the default mode. podvm creates a Linux bridge (podvm0), assigns it a subnet (10.89.0.0/24), and connects each container via a TAP interface. NAT provides external connectivity. Port forwarding uses iptables DNAT.

## Rationale

Bridge networking is the established pattern. Docker and Podman both default to bridge networking. Users expect `podvm run nginx` to create a container with an IP address accessible from the host.

Inter-container communication works automatically. Containers on the same bridge can communicate directly. This is essential for multi-container applications (web server + database).

NAT provides outbound connectivity without configuration. Containers can reach external services (package managers, registries, APIs) through the host's network connection. No additional setup is required.

Port forwarding via iptables is well-understood. `-p 8080:80` creates a DNAT rule that forwards host:8080 to container:80. This is the same mechanism Docker uses. Users familiar with Docker port forwarding will find podvm identical.

The bridge is created on first use and persists across reboots. podvmd detects whether podvm0 exists and creates it if not. This eliminates first-run configuration.

## Implications

Bridge networking adds latency compared to host networking. Traffic between containers traverses the bridge and undergoes NAT for external access. For most workloads this is negligible (~1-5% throughput reduction). Latency-sensitive workloads should use --network=host.

Port forwarding requires root or CAP_NET_ADMIN. Rootless operation needs either slirp4netns (user-mode networking) or appropriate capabilities. This is a known limitation of rootless networking.

The bridge subnet (10.89.0.0/24) must not conflict with existing networks. podvmd should detect conflicts and choose an alternative subnet. The default can be configured in /etc/containers/containers.conf.

Multiple bridges are supported for network isolation. `podvm network create mynet` creates a separate bridge. Containers on different bridges cannot communicate unless explicitly connected. This provides tenant isolation.

## Alternatives Considered

**MACVTAP:** Rejected as default. Containers get direct network access but cannot communicate with each other without external switch configuration. Requires routable IP addresses, which is not available in all environments.

**CNI plugins:** Considered for advanced networking (Calico, Cilium, Weave). Valuable for Kubernetes integration but too complex for default networking. CNI support can be added as a --network=cni option.

**slirp4netns default:** Considered for rootless operation. slirp4netns provides user-mode networking without root. However, performance is poor (~50% throughput reduction) and it adds a dependency. Bridge networking remains the default for rootful operation.

**IPv6 only:** Considered but rejected. IPv4 is still dominant in container workloads. Dual-stack support (IPv4 + IPv6) is the correct approach for new runtimes.
