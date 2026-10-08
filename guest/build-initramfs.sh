#!/bin/sh
# Build minimal initramfs for podvm guest
# Usage: ./build-initramfs.sh [output-path]
#
# The init binary is placed directly at /init (it is PID 1). There is no
# shell in the initramfs, so /init must be the real binary, not a script.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

OUT="${1:-$SCRIPT_DIR/initramfs.cpio}"
INIT="$SCRIPT_DIR/init"

# Build init if missing
if [ ! -f "$INIT" ]; then
    echo "Building init binary..."
    make -C "$SCRIPT_DIR"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Directory structure (POSIX-safe: no brace expansion under dash)
for d in bin proc sys dev rootfs tmp etc; do
    mkdir -p "$WORK/$d"
done

# init is PID1 — no shell wrapper, place the real binary at /init
cp "$INIT" "$WORK/init"

# Bundle guest kernel modules (decompressed) for host-kernel boots.
# A KVM guest is a separate kernel instance: the host's loaded modules
# don't count, and there's no modprobe in the initramfs to autoload.
# The synthetic kernel builds everything in and ignores /modules.
# 9p: netfs (dep of 9pnet), 9pnet, 9pnet_virtio, 9p
# vsock: vsock, virtio transport common, virtio transport
KREL=$(uname -r)
MODSRC="/lib/modules/$KREL"
if [ -d "$MODSRC" ]; then
    mkdir -p "$WORK/modules"
    for m in \
        fs/netfs/netfs \
        net/9p/9pnet net/9p/9pnet_virtio fs/9p/9p \
        net/vmw_vsock/vsock \
        net/vmw_vsock/vmw_vsock_virtio_transport_common \
        net/vmw_vsock/vmw_vsock_virtio_transport; do
        for f in "$MODSRC/kernel/$m.ko.xz" "$MODSRC/kernel/$m.ko.gz" "$MODSRC/kernel/$m.ko"; do
            [ -f "$f" ] || continue
            base=$(basename "$f")
            base=${base%.xz}
            base=${base%.gz}
            case "$f" in
                *.xz)   xz -dc "$f" > "$WORK/modules/$base" ;;
                *.gz)   gzip -dc "$f" > "$WORK/modules/$base" ;;
                *)      cp "$f" "$WORK/modules/$base" ;;
            esac
            break
        done
    done
    if [ -z "$(ls -A "$WORK/modules" 2>/dev/null)" ]; then
        rmdir "$WORK/modules"
    else
        echo "initramfs: bundled guest modules: $(ls "$WORK/modules")"
    fi
fi

# Archive (uncompressed for faster boot)
( cd "$WORK" && find . -print0 | cpio --null -o -H newc > "$OUT" )

echo "initramfs: $(wc -c < "$OUT") bytes -> $OUT"
