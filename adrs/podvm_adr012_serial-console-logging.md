# ADR-012: Serial Console for Guest Logging

**Date:** 2024-09-29

**Status:** Accepted

## Context

Containers produce output (stdout, stderr) that users need to see. Traditional containers capture file descriptors directly. VM-based containers need a different mechanism since the guest and host are separate systems.

Options for guest output include:

1. Serial console: Guest writes to /dev/console, which maps to a virtual serial port. Host captures output from the serial device.
2. virtio-console: Bidirectional character device. Guest writes to /dev/hvc0, host reads from the virtio-console device.
3. File-based logging: Guest writes logs to a file on a shared virtiofs mount. Host reads the file.
4. Network logging: Guest sends logs over the network to a logging daemon.

Each approach has different characteristics for reliability, performance, and simplicity.

## Decision

podvm will use the serial console (COM1/UART) for guest output. The guest init writes to stderr, which is connected to /dev/console. The host captures serial output and stores it in container log files.

## Rationale

Serial console is the simplest mechanism. The kernel outputs to serial by default (CONFIG_SERIAL_8250_CONSOLE=y). The guest init's stderr is connected to /dev/console automatically. No additional setup is required.

Serial console works before the filesystem is ready. If the guest fails to mount virtiofs or chroot, serial output is still visible. This is critical for debugging boot failures. Other mechanisms (virtio-console, file logging) require the guest to be partially functional.

The serial device is emulated by all hypervisors. Cloud Hypervisor, QEMU, and Firecracker all support serial console output. There are no compatibility concerns.

Log rotation and storage follow Podman's conventions. Logs are stored in /var/lib/podvm/containers/<id>/ctr.log (similar to Podman's). Log rotation is handled by the daemon with configurable size and count limits.

Performance is acceptable for container output. Serial console throughput is limited (~115200 baud for emulated UART, higher for virtio-console). This is sufficient for stdout/stderr from most applications. High-throughput logging should use file-based logging via virtiofs.

## Implications

Serial console output is line-buffered by default. Applications that write without newlines may see delayed output. The guest kernel's printk buffering also adds latency. For real-time output, the buffer can be reduced or disabled via kernel cmdline.

The serial device must be configured in the hypervisor. Cloud Hypervisor's --serial parameter enables serial output. The host launcher must pass this parameter and capture the output.

Multiple containers produce interleaved serial output if their serial devices share a file. Each container's serial output must be captured to a separate file. The daemon manages this by redirecting each VM's serial output to its own log file.

Guest kernel messages (panic, oops, module loading) appear on the serial console. This provides valuable debugging information without additional configuration. Kernel log level can be adjusted via the cmdline.

## Alternatives Considered

**virtio-console:** Considered. virtio-console provides higher throughput and bidirectional communication. However, it requires the virtio-console driver to be loaded before output is visible. Serial console works earlier in boot. virtio-console is better for exec communication (future) but not for logging.

**File-based logging:** Rejected as primary. Requires virtiofs to be mounted before logging works. If the mount fails, logs are lost. Useful as a secondary mechanism for high-throughput applications.

**journald integration:** Considered. Guest could send logs to the host's journald via syslog forwarding. Adds complexity and dependency on journald. Serial console captures output without additional daemons.

**Network logging (syslog, Fluentd):** Rejected as default. Adds network dependency. Useful for production deployments but too complex for default behavior.
