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

# Archive (uncompressed for faster boot)
( cd "$WORK" && find . -print0 | cpio --null -o -H newc > "$OUT" )

echo "initramfs: $(wc -c < "$OUT") bytes -> $OUT"
