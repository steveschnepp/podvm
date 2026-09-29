# ADR-016: vsock for Host-Guest IPC

**Date:** 2024-09-29

**Status:** Accepted

## Context

podvm requires bidirectional communication between host and guest for several operations:

1. **exec**: Host sends a command to execute inside the guest. Guest spawns the process, streams stdout/stderr back, and reports exit status.

2. **build**: Host orchestrates a multi-step build. Each Dockerfile instruction requires the guest to execute a command, signal completion, and receive the next instruction. The host must freeze the upper layer between instructions to create new OCI layers.

3. **copy**: Host copies files into the guest (equivalent of COPY in Dockerfile). Guest writes files to the container filesystem.

4. **signal**: Host sends signals (SIGTERM, SIGKILL) to processes inside the guest.

5. **status**: Host queries guest status (running processes, memory usage).

The serial console (virtio-console) handles stdout/stderr but is not suitable for structured control messages. It is a byte stream, not a message protocol. Multiplexing control and data on the same stream adds complexity and fragility.

## Decision

podvm will use vsock (VM socket) for host-guest IPC. vsock provides a socket-like interface between host and guest without requiring network configuration. The guest agent listens on a well-known CID:port. The host connects to send commands and receive responses.

## Rationale

vsock is purpose-built for host-guest communication. It uses a CID (Context ID) to identify the guest and a port number for service multiplexing. No IP addresses, no bridges, no NAT. The connection works from boot without configuration.

Cloud Hypervisor supports vsock natively. The --vsock parameter creates a vsock device backed by a host Unix socket. The host connects to the Unix socket; the guest connects to /dev/vsock. Data flows bidirectionally.

vsock separates control from data. The serial console continues to handle stdout/stderr. vsock handles structured messages (exec, commit, signal). This separation simplifies both channels. The serial console remains a simple byte stream. vsock carries typed messages.

The protocol is simple. Messages are length-prefixed binary structs. No JSON parsing needed in the guest. The 7KB init can be extended with a small message handler without significant size increase.

vsock works before the filesystem is ready. If virtiofs mount fails, vsock is still available for error reporting. The guest can report boot failures to the host without requiring filesystem access.

## Protocol Design

```
Message format (24 bytes header + payload):

struct message {
    uint32_t length;     // payload length
    uint32_t type;       // message type
    uint32_t id;         // request/response correlation
    uint32_t flags;      // optional flags
    uint8_t  payload[];  // type-specific data
};
```

### Message Types

```
Host → Guest:
  MSG_EXEC        = 0x01   // execute command
  MSG_COPY        = 0x02   // copy file to guest
  MSG_SIGNAL      = 0x03   // send signal to process
  MSG_FREEZE      = 0x04   // prepare for layer commit
  MSG_PING        = 0x10   // health check

Guest → Host:
  MSG_READY       = 0x20   // guest ready for commands
  MSG_EXEC_DONE   = 0x21   // exec completed (with exit code)
  MSG_STDOUT      = 0x22   // stdout data
  MSG_STDERR      = 0x23   // stderr data
  MSG_FROZEN      = 0x24   // upper layer frozen
  MSG_ERROR       = 0x2F   // error response
  MSG_PONG        = 0x30   // ping response
```

### Exec Flow

```
Host                          Guest
  │                             │
  │──── MSG_EXEC ──────────────>│
  │     {cmd, env, tty}         │
  │                             │ fork + exec
  │<──── MSG_STDOUT ────────────│
  │     {data}                  │
  │<──── MSG_STDOUT ────────────│
  │     {data}                  │
  │<──── MSG_EXEC_DONE ─────────│
  │     {exit_code}             │
```

### Build Flow (Layer Commit)

```
Host                          Guest
  │                             │
  │──── MSG_EXEC ──────────────>│
  │     {cmd: "apt-get update"}│
  │<──── MSG_STDOUT ────────────│
  │<──── MSG_EXEC_DONE ─────────│
  │     {exit_code: 0}          │
  │                             │
  │──── MSG_FREEZE ────────────>│  // stop writes to upper
  │<──── MSG_FROZEN ────────────│
  │                             │
  │ host: commit upper → new layer
  │ host: new overlay = layers + fresh upper
  │                             │
  │──── MSG_EXEC ──────────────>│  // next instruction
  │     {cmd: "npm install"}   │
  │                             │
```

## Implications

The guest agent grows from ~7KB to ~15-20KB to include vsock handling and message parsing. This is still minimal for an initramfs binary.

The guest agent becomes PID 1 and must handle signal forwarding. If the host sends SIGTERM to the agent, it should forward to the child process (the entrypoint). This is standard init behavior.

vsock requires kernel support in both host and guest. The host kernel must have vhost-vsock module (standard in modern kernels). The guest kernel requires CONFIG_VSOCK=y and CONFIG_VSOCKETS_COMMON=y (already in the synthetic kernel config).

The vsock device is created by Cloud Hypervisor via --vsock cid=3,sock=/run/podvm/<id>/vsock.sock. The host connects to the Unix socket. The guest sees /dev/vsock with CID 3.

Port allocation must be managed. A well-known port (e.g., 5000) for the podvm agent avoids port conflicts. Multiple agents in different VMs use the same port number because CIDs are unique per VM.

## Alternatives Considered

**Serial console with control protocol:** Considered. The serial console is already present. A control protocol could be layered on top (e.g., escape sequences). However, multiplexing control and data on one stream is error-prone. Serial console should remain a simple byte stream for stdout/stderr.

**virtiofs control file:** Considered. Guest writes commands to a file on virtiofs; host polls for changes. This is simple but has latency (polling interval) and race conditions (concurrent reads/writes). vsock is cleaner.

**Network socket (localhost):** Considered. Guest and host could communicate over the bridge network. However, this requires networking to be configured before IPC works. vsock works from boot without network.

**9p control channel:** Considered. 9p can provide bidirectional communication. However, 9p is slower than vsock and adds complexity. vsock is purpose-built for this use case.

**No IPC (host-only orchestration):** Rejected for build and exec. The host cannot execute commands inside the guest without guest cooperation. The host can start/stop VMs but cannot run arbitrary commands. IPC is required for exec and build.
