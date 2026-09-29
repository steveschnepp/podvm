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

# Create initramfs
cd "$INITRAMFS_DIR"
find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 > ../initramfs.cpio.gz
cd ..

# Cleanup
rm -rf "$INITRAMFS_DIR"

echo "initramfs: $(ls -la initramfs.cpio.gz | awk '{print $5}') bytes"
