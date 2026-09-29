# ADR-006: Kernel Command Line for Guest Configuration

**Date:** 2024-09-29

**Status:** Accepted

## Context

The guest init needs configuration: which directory to chroot into, what command to execute, what environment variables to set, and what user to run as. This configuration must be passed from the host to the guest at boot time.

Options for passing configuration include:

1. Kernel command line: Parameters passed via the kernel cmdline at boot. Readable from /proc/cmdline.
2. Device tree: Structured data passed to the kernel. Standard in ARM, less common on x86.
3. virtiofs shared config: A configuration file mounted via virtiofs. Requires the filesystem to be ready before init runs.
4. virtio-console: Host sends configuration over virtio-console after boot. Requires synchronization.

Each approach has different reliability, complexity, and ordering characteristics.

## Decision

Configuration will be passed via kernel command line parameters with the prefix `podvm.`. The guest init reads /proc/cmdline and parses these parameters.

## Rationale

Kernel command line is the most reliable mechanism. The parameters are available the instant init runs. There is no filesystem dependency, no synchronization needed, and no additional device setup. The kernel has already parsed the cmdline before init executes.

The format is simple: key=value pairs separated by spaces. `podvm.root=/rootfs podvm.entrypoint=/bin/sh podvm.args=-c,echo,hello`. This is easy to generate on the host and parse in C with standard string functions.

Virtual machine monitors (Cloud Hypervisor, QEMU) accept kernel cmdline directly. No additional configuration mechanism is needed. The host launcher constructs the cmdline string and passes it as a boot parameter.

The `podvm.` prefix avoids collisions with kernel parameters. Standard kernel parameters use names without dots. The prefix clearly identifies podvm-specific configuration and allows future extension without breaking existing deployments.

Multi-value parameters use comma separation. Environment variables, arguments, and paths can contain multiple values. `podvm.env=PATH=/usr/bin:/bin podvm.args=-c,echo,hello`. The comma separator is unambiguous in cmdline context.

## Implications

Command line length is limited by the kernel (typically 2048 bytes on x86). Containers with many environment variables or long argument lists may hit this limit. Workaround: use a small number of env vars or pass a config file via virtiofs and reference it from the cmdline.

Parameters are visible in /proc/cmdline inside the guest. Sensitive values (passwords, tokens) should not be passed via cmdline. For sensitive configuration, use virtiofs-shared config files with appropriate permissions.

The cmdline format is fixed. Changing parameter names or formats requires a new init version. The `podvm.` prefix provides namespace protection, but parameter semantics should be versioned if they change.

Debugging is straightforward. `cat /proc/cmdline` inside the guest (via serial console) shows the full configuration. This simplifies troubleshooting boot failures.

## Alternatives Considered

**virtio-console dynamic config:** Considered but rejected. Requires synchronization between host sending config and guest reading it. Adds state machine complexity to the init. Cmdline is simpler and available immediately.

**Device tree:** Rejected for x86. Device tree is standard on ARM but not on x86_64. Cloud Hypervisor supports device tree on ARM but not consistently on x86. Cmdline is universal.

**virtiofs config file:** Considered but rejected. Requires virtiofs daemon to be ready before init reads config. Adds ordering dependency. Cmdline avoids this entirely.
