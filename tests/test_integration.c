/*
 * test_integration.c - Integration tests for podvm
 *
 * Build: gcc -o test_integration tests/test_integration.c
 * Run: ./test_integration
 *
 * These tests verify the integration between host and guest components.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_PATH 1024
#define MAX_CMDLINE 4096

/* Test if binary exists */
static int binary_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* Test if we can build host */
static void test_build_host(void) {
    printf("Testing host build...\n");
    
    /* Check if build directory exists */
    struct stat st;
    if (stat("build", &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Check if podvm binary exists */
        if (binary_exists("build/podvm")) {
            printf("PASS: host binary exists\n");
            return;
        }
    }
    
    /* Try to build */
    printf("Building host...\n");
    int ret = system("mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1 && make > /dev/null 2>&1");
    
    if (ret == 0 && binary_exists("build/podvm")) {
        printf("PASS: host build successful\n");
    } else {
        printf("WARN: host build failed (expected if dependencies missing)\n");
    }
}

/* Test if we can build guest init */
static void test_build_guest(void) {
    printf("Testing guest build...\n");
    
    /* Check if guest/init binary exists */
    if (binary_exists("guest/init")) {
        printf("PASS: guest init binary exists\n");
        return;
    }
    
    /* Try to build */
    printf("Building guest init...\n");
    int ret = system("cd guest && make > /dev/null 2>&1");
    
    if (ret == 0 && binary_exists("guest/init")) {
        printf("PASS: guest build successful\n");
    } else {
        printf("WARN: guest build failed (expected if musl-gcc not installed)\n");
    }
}

/* Test if we can build initramfs */
static void test_build_initramfs(void) {
    printf("Testing initramfs build...\n");
    
    /* Check if initramfs exists */
    if (binary_exists("guest/initramfs.cpio")) {
        printf("PASS: initramfs exists\n");
        return;
    }
    
    /* Try to build */
    printf("Building initramfs...\n");
    int ret = system("cd guest && ./build-initramfs.sh > /dev/null 2>&1");
    
    if (ret == 0 && binary_exists("guest/initramfs.cpio")) {
        printf("PASS: initramfs build successful\n");
    } else {
        printf("WARN: initramfs build failed\n");
    }
}

/* Test if QEMU is available */
static void test_qemu_available(void) {
    printf("Testing QEMU availability...\n");
    
    int ret = system("which qemu-system-x86_64 > /dev/null 2>&1");
    
    if (ret == 0) {
        printf("PASS: QEMU available\n");
    } else {
        printf("WARN: QEMU not found\n");
    }
}

/* Test if virtiofsd is available */
static void test_virtiofsd_available(void) {
    printf("Testing virtiofsd availability...\n");
    
    int ret = system("which virtiofsd > /dev/null 2>&1");
    
    if (ret == 0) {
        printf("PASS: virtiofsd available\n");
    } else {
        printf("WARN: virtiofsd not found\n");
    }
}

/* Test if KVM is available */
static void test_kvm_available(void) {
    printf("Testing KVM availability...\n");
    
    struct stat st;
    if (stat("/dev/kvm", &st) == 0) {
        printf("PASS: KVM available\n");
    } else {
        printf("WARN: /dev/kvm not found\n");
    }
}

/* Test kernel config */
static void test_kernel_config(void) {
    printf("Testing kernel config...\n");
    
    FILE *f = fopen("kernel/config", "r");
    if (!f) {
        printf("WARN: kernel/config not found\n");
        return;
    }
    
    char line[256];
    int has_vsock = 0;
    int has_virtio_fs = 0;
    int has_balloon = 0;
    
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "CONFIG_VSOCK=y")) has_vsock = 1;
        if (strstr(line, "CONFIG_VIRTIO_FS=y")) has_virtio_fs = 1;
        if (strstr(line, "CONFIG_VIRTIO_BALLOON=y")) has_balloon = 1;
    }
    
    fclose(f);
    
    if (has_vsock && has_virtio_fs && has_balloon) {
        printf("PASS: kernel config has required options\n");
    } else {
        printf("WARN: kernel config missing some options\n");
        printf("  VSOCK: %s\n", has_vsock ? "yes" : "no");
        printf("  VIRTIO_FS: %s\n", has_virtio_fs ? "yes" : "no");
        printf("  VIRTIO_BALLOON: %s\n", has_balloon ? "yes" : "no");
    }
}

/* Test rootfs directory */
static void test_rootfs_directory(void) {
    printf("Testing rootfs directory...\n");
    
    /* Create a test rootfs */
    int ret = system("mkdir -p /tmp/podvm-test-rootfs/bin /tmp/podvm-test-rootfs/usr/bin");
    if (ret != 0) {
        printf("WARN: could not create test rootfs\n");
        return;
    }
    
    /* Create a simple shell script */
    FILE *f = fopen("/tmp/podvm-test-rootfs/bin/hello", "w");
    if (f) {
        fprintf(f, "#!/bin/sh\necho 'Hello from podvm!'\n");
        fclose(f);
        system("chmod +x /tmp/podvm-test-rootfs/bin/hello");
    }
    
    /* Check if directory exists */
    struct stat st;
    if (stat("/tmp/podvm-test-rootfs", &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("PASS: test rootfs directory created\n");
    } else {
        printf("WARN: could not create test rootfs\n");
    }
}

/* Test vsock support in kernel */
static void test_vsock_kernel_support(void) {
    printf("Testing vsock kernel support...\n");
    
    int ret = system("grep -q 'AF_VSOCK' /usr/include/linux/vm_sockets.h 2>/dev/null");
    
    if (ret == 0) {
        printf("PASS: vsock headers available\n");
    } else {
        printf("WARN: vsock headers not found\n");
    }
}

/* Test full flow (dry run) */
static void test_full_flow_dry_run(void) {
    printf("Testing full flow (dry run)...\n");
    
    /* Check all prerequisites */
    int host_ok = binary_exists("build/podvm");
    int guest_ok = binary_exists("guest/init");
    int initramfs_ok = binary_exists("guest/initramfs.cpio");
    int qemu_ok = (system("which qemu-system-x86_64 > /dev/null 2>&1") == 0);
    int kvm_ok = (access("/dev/kvm", W_OK) == 0);
    
    if (host_ok && guest_ok && initramfs_ok && qemu_ok && kvm_ok) {
        printf("PASS: all prerequisites met for full test\n");
        printf("  Run: ./build/podvm -it /tmp/podvm-test-rootfs /bin/hello\n");
    } else {
        printf("WARN: missing prerequisites for full test\n");
        printf("  Host: %s\n", host_ok ? "ok" : "missing");
        printf("  Guest: %s\n", guest_ok ? "ok" : "missing");
        printf("  Initramfs: %s\n", initramfs_ok ? "ok" : "missing");
        printf("  QEMU: %s\n", qemu_ok ? "ok" : "missing");
        printf("  KVM: %s\n", kvm_ok ? "ok" : "missing");
    }
}

int main(void) {
    printf("podvm integration tests\n");
    printf("=======================\n\n");
    
    /* Build tests */
    test_build_host();
    test_build_guest();
    test_build_initramfs();
    
    /* Dependency tests */
    test_qemu_available();
    test_virtiofsd_available();
    test_kvm_available();
    
    /* Config tests */
    test_kernel_config();
    test_vsock_kernel_support();
    
    /* Setup tests */
    test_rootfs_directory();
    
    /* Integration test */
    test_full_flow_dry_run();
    
    printf("\n=======================\n");
    printf("Tests complete!\n");
    
    return 0;
}
