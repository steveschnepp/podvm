# Uncompressed initrd

> Skip the compression. Reduce attack surface.

---

## Current State

```bash
# build-initramfs.sh
find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > initramfs.cpio.gz
#                                                                     ^^^^^^^^
#                                                                     compressed
```

**Kernel needs**: `CRYPTO_LZ4` (or GZIP/ZSTD) to decompress at boot.

---

## Proposal: Uncompressed initrd

```bash
# build-initramfs.sh
find . -print0 | cpio --null -o -H newc > initramfs.cpio
#                                          ^^^^^^^^^^^
#                                          uncompressed
```

**Kernel needs**: Nothing. QEMU loads it directly into memory.

---

## QEMU Handles Both

```bash
# Compressed (current)
qemu-system-x86_64 -initrd initramfs.cpio.gz ...

# Uncompressed (proposed)
qemu-system-x86_64 -initrd initramfs.cpio ...
```

QEMU doesn't care. It loads the file into guest memory. Kernel detects format:
- `.gz` → decompress with gzip
- raw cpio → use directly

---

## Benefits

| Aspect | Compressed | Uncompressed |
|--------|------------|--------------|
| Boot speed | Slower (decompress) | Faster |
| Kernel config | Needs CRYPTO_LZ4 | Nothing |
| Attack surface | +1 crypto driver | None |
| Disk size | Smaller | Larger (who cares) |
| Memory usage | Same after boot | Same after boot |

---

## Size Impact

```bash
# Our init binary: ~5KB
# Our initramfs: ~10KB compressed, ~12KB uncompressed

# Difference: 2KB
# Who cares?
```

For a minimal init with one static binary, compression is pointless overhead.

---

## Kernel Config Change

```bash
# REMOVE these
# CONFIG_CRYPTO_LZ4 is not set
# CONFIG_CRYPTO_ZSTD is not set
# CONFIG_CRYPTO_GZIP is not set
```

**Savings**: ~50KB kernel code + removed attack surface.

---

## Updated build-initramfs.sh

```bash
#!/bin/sh
# Build minimal initramfs for podvm guest
# Usage: ./build-initramfs.sh

set -e

INITRAMFS_DIR=$(mktemp -d)
INIT=init

# Check if init binary exists
if [ ! -f "$INIT" ]; then
    echo "Building init binary..."
    make
fi

# Create directory structure
mkdir -p "$INITRAMFS_DIR"/{bin,proc,sys,dev,rootfs,tmp,etc}

# Copy init
cp "$INIT" "$INITRAMFS_DIR/bin/init"

# Create /init wrapper
cat > "$INITRAMFS_DIR/init" << 'EOF'
#!/bin/sh
exec /bin/init "$@"
EOF
chmod +x "$INITRAMFS_DIR/init"

# Create UNCOMPRESSED initramfs
cd "$INITRAMFS_DIR"
find . -print0 | cpio --null -o -H newc > ../initramfs.cpio
cd ..

# Cleanup
rm -rf "$INITRAMFS_DIR"

echo "initramfs: $(ls -la initramfs.cpio | awk '{print $5}') bytes"
```

---

## Host-Side Change

In `src/main.c`:

```c
// Current
"-initrd", "/var/lib/podvm/kernel/initramfs.cpio.gz",

// Proposed
"-initrd", "/var/lib/podvm/kernel/initramfs.cpio",
```

---

## Summary

| Change | Impact |
|--------|--------|
| Remove compression from build script | Simpler |
| Remove CRYPTO_* from kernel config | Less attack surface |
| Rename .cpio.gz → .cpio | Clearer |
| Update QEMU args | Match filename |

**Trade-off**: 2KB larger file for less kernel code and faster boot.
