# POC Implementation

> Minimal viable podvm with vsock IPC.

---

## What Changed

### 1. Host (`src/main.c`)

**Added:**
- vsock client to send config to guest
- Uncompressed initramfs support
- Dynamic memory backend
- Non-blocking QEMU start

**Key functions:**
```c
send_config_vsock()   // Send config to guest via vsock
start_qemu()          // Non-blocking QEMU start
wait_qemu()           // Wait for QEMU exit
```

**Flow:**
```
1. Start virtiofsd
2. Start QEMU (non-blocking)
3. Send config to guest via vsock
4. Wait for QEMU to exit
5. Cleanup
```

---

### 2. Guest Init (`guest/init.c`)

**Added:**
- vsock server to receive config from host
- Fallback to kernel cmdline if vsock fails

**Key functions:**
```c
receive_config_vsock()  // Wait for host connection, receive config
parse_cmdline()         // Parse config string (same format as cmdline)
```

**Flow:**
```
1. Mount pseudo-fs
2. Wait for vsock connection
3. Receive config from host
4. Fallback to cmdline if vsock fails
5. Mount rootfs
6. Chroot + exec
```

---

### 3. Kernel Config (`kernel/config`)

**Added:**
- `VSOCK=y` - host↔guest IPC
- `VIRTIO_VSOCKETS_COMMON=y`
- `NAMESPACES` - container isolation
- `SECCOMP` - syscall filtering
- `SECURITY_CAPABILITIES`
- `KERNEL_UNCOMPRESSED` - faster boot
- Security hardening (KASLR, stack protector, etc.)

**Removed:**
- `BPF` - attack surface
- `VIRTIO_MMIO/BLK/NET/RNG` - not needed
- `VT/VT_CONSOLE` - use serial
- `CRYPTO_*` - uncompressed initrd
- `MODULES` - built-in only
- Debug features (FTRACE, KGDB, etc.)

---

### 4. Build Scripts

**Changed:**
- Uncompressed initramfs (`initramfs.cpio` not `.gz`)
- Faster boot, less kernel code

---

## Building

### Host
```bash
mkdir build && cd build
cmake ..
make
```

### Guest Init
```bash
cd guest
make
./build-initramfs.sh
```

### Kernel
```bash
cd linux-x86_64
make defconfig
cp ../kernel/config .config
make olddefconfig
make -j$(nproc) bzImage
```

---

## Running

```bash
# Basic
./build/podvm /path/to/rootfs /bin/sh

# Interactive
./build/podvm -it /path/to/rootfs /bin/sh

# With options
./build/podvm -m 256 -c 2 /path/to/rootfs /bin/sh
```

---

## Vsock Protocol

**Port:** 5000

**Host → Guest:**
```
podvm.root=/rootfs podvm.entrypoint=/bin/sh podvm.args=- podvm.env=HOME=/root
```

**Guest → Host:**
```
OK
```

---

## Attack Surface (Minimal)

**Kernel:**
- Virtio PCI (bus)
- Virtio Console (serial)
- Virtio FS (rootfs)
- Virtio Balloon (memory)
- Vsock (IPC)
- Namespaces (isolation)
- Seccomp (filtering)
- Serial console

**Removed:**
- BPF, modules, tracing, debug, audit, security modules, power management, networking stack, block devices, USB, sound, GPU, etc.

---

## Next Steps

1. **Test vsock communication** - Verify host↔guest config transfer
2. **Add network module** - Tap device + bridge for guest networking
3. **Add OCI image support** - Pull and unpack images
4. **Add container lifecycle** - stop, kill, exec
5. **Add daemon mode** - Long-running podvmd
