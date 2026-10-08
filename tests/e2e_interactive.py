#!/usr/bin/env python3
"""
e2e_interactive.py - prove -i gives the guest stdin exclusively.

QEMU's default serial backend (-serial mon:stdio) multiplexes the QEMU
monitor onto stdio, so Ctrl-A is the monitor escape and never reaches the
guest. podvm's -i selects -serial stdio instead, giving the guest stdin
exclusively.

This boots a VM with -i under a pty, runs /bin/cat, and sends a byte
string containing a raw Ctrl-A (0x01). With -i the guest must receive the
Ctrl-A and cat must echo the whole string back -- proving no monitor
swallowed it. The pty is put in raw mode first so the terminal's own
line discipline cannot echo; anything echoed has round-tripped through
the guest.

Synchronization is explicit: we wait for the guest's "podvm-init: exec
/bin/cat" trace (printed immediately before execvp), then send input.
cat blocks on read, so the bytes are buffered until it runs -- no sleep
or race.

Requires: KVM + an alpine image (busybox cat). Skips cleanly otherwise.

Usage: ./e2e_interactive.py
Exit: 0 = the Ctrl-A reached the guest, 1 = failure.
"""

import os
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import time
import tty

EXEC_MARKER = b"podvm-init: exec /bin/cat"
# A, B, Ctrl-A, C, D, E -- the Ctrl-A must survive to the guest.
PAYLOAD = b"AB\x01CDE\n"
BOOT_TIMEOUT = 90  # seconds


def log(msg):
    print(f"E2E-INTERACTIVE {msg}")


def skip(reason):
    log(f"SKIP: {reason}")
    sys.exit(0)


def fail(reason, buf):
    log(f"FAIL: {reason}")
    tail = buf[-2000:].decode("utf-8", "replace")
    print("--- last guest output ---")
    print(tail)
    sys.exit(1)


def have_kvm():
    return os.path.exists("/dev/kvm")


def have_alpine():
    if not shutil.which("podman"):
        return False
    r = subprocess.run(["podman", "images", "--format", "{{.Repository}}"],
                       capture_output=True, text=True)
    return "alpine" in r.stdout


def main():
    if not have_kvm():
        skip("no /dev/kvm")
    if not have_alpine():
        skip("no alpine image")

    work = tempfile.mkdtemp(prefix="podvm-e2e-interactive.")
    try:
        # Build guest init + initramfs. build-initramfs.sh writes the cpio
        # to the path given as $1 and prints progress to stdout -- so do
        # NOT redirect stdout into the same file (it would corrupt the
        # archive). Capture it separately for diagnostics.
        if subprocess.run(["make", "-s", "-C", "guest"]).returncode != 0:
            fail("guest build", b"")
        initramfs = os.path.join(work, "initramfs.cpio")
        build = subprocess.run(["guest/build-initramfs.sh", initramfs],
                               capture_output=True)
        if build.returncode != 0:
            fail("initramfs build", build.stdout + build.stderr)

        master, slave = pty.openpty()
        # Raw mode: no local echo, no canonical line buffering. Whatever
        # comes back on the master has round-tripped through the guest.
        tty.setraw(slave)

        # start_new_session puts podvm in its own process group so we can
        # kill podvm *and* its QEMU child together. Killing only podvm
        # orphans the QEMU, which stays alive holding vsock CID 3 and
        # breaks every later run ("Address already in use").
        proc = subprocess.Popen(
            ["build/podvm", "-i", "-r", initramfs, "alpine", "/bin/cat"],
            stdin=slave, stdout=slave, stderr=slave, close_fds=True,
            start_new_session=True)
        os.close(slave)

        buf = b""
        sent = False
        got_echo = False
        deadline = time.time() + BOOT_TIMEOUT

        while time.time() < deadline:
            ready, _, _ = select.select([master], [], [], 0.5)
            if ready:
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                buf += chunk

            if not sent and EXEC_MARKER in buf:
                os.write(master, PAYLOAD)
                sent = True
            # Look for the payload echoed back WITH the Ctrl-A intact.
            if sent and PAYLOAD.rstrip(b"\n") in buf:
                got_echo = True
                break

        # Kill the whole process group (podvm + its QEMU child), not just
        # podvm -- an orphaned QEMU would keep holding the vsock CID.
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except OSError:
            pass
        os.close(master)

        if not sent:
            fail("guest never exec'd cat (no exec marker)", buf)
        if not got_echo:
            fail("Ctrl-A did not reach the guest (payload not echoed intact)",
                 buf)

        log("PASS: -i delivered the Ctrl-A byte to the guest intact")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
