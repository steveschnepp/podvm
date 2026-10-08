/*
 * test_host_args.c - Test host argument parsing
 *
 * Build: gcc -o test_host_args tests/test_host_args.c -Isrc
 * Run: ./test_host_args
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Mock the config struct from main.c */
#define MAX_PATH 1024

static struct {
    char kernel[MAX_PATH];
    char initramfs[MAX_PATH];
    char rootfs_dir[MAX_PATH];
    char entrypoint[MAX_PATH];
    char args[256];
    char env[1024];
    int  memory;
    int  cpus;
    int  interactive;
} config = {
    .kernel = "/var/lib/podvm/kernel/bzImage",
    .initramfs = "/var/lib/podvm/kernel/initramfs.cpio",
    .memory = 128,
    .cpus = 1,
    .interactive = 0,
};

/* Parse arguments (same logic as main.c) */
static int parse_args(int argc, char **argv) {
    int i = 1;

    /* Parse options */
    while (i < argc && argv[i][0] == '-') {
        const char *opt = argv[i];

        /* Handle -it flag */
        if (strcmp(opt, "-it") == 0) {
            config.interactive = 1;
            i++;
        }
        /* Handle -i (interactive) */
        else if (strcmp(opt, "-i") == 0) {
            config.interactive = 1;
            i++;
        }
        /* Handle -m <MB> */
        else if (strcmp(opt, "-m") == 0 && i + 1 < argc) {
            config.memory = atoi(argv[i + 1]);
            i += 2;
        }
        /* Handle -c <cpus> */
        else if (strcmp(opt, "-c") == 0 && i + 1 < argc) {
            config.cpus = atoi(argv[i + 1]);
            i += 2;
        }
        /* Handle -k <path> */
        else if (strcmp(opt, "-k") == 0 && i + 1 < argc) {
            strncpy(config.kernel, argv[i + 1], MAX_PATH - 1);
            i += 2;
        }
        /* Handle -r <path> */
        else if (strcmp(opt, "-r") == 0 && i + 1 < argc) {
            strncpy(config.initramfs, argv[i + 1], MAX_PATH - 1);
            i += 2;
        }
        /* Handle -h */
        else if (strcmp(opt, "-h") == 0) {
            return -1;  /* Help requested */
        }
        /* Unknown option */
        else {
            return -2;  /* Invalid option */
        }
    }

    return i;
}

/* Test -it flag */
static void test_it_flag(void) {
    /* Reset */
    config.interactive = 0;
    config.memory = 128;
    
    /* Test -it */
    char *argv[] = {"podvm", "-it", "/rootfs", "/bin/sh", NULL};
    int argc = 4;
    
    int ret = parse_args(argc, argv);
    assert(ret == 2);  /* Should stop at rootfs */
    assert(config.interactive == 1);
    
    /* Test -i */
    config.interactive = 0;
    char *argv2[] = {"podvm", "-i", "/rootfs", "/bin/sh", NULL};
    ret = parse_args(argc, argv2);
    assert(ret == 2);
    assert(config.interactive == 1);
    
    printf("PASS: test_it_flag\n");
}

/* Test -m flag */
static void test_m_flag(void) {
    /* Reset */
    config.memory = 128;
    config.interactive = 0;
    
    /* Test -m 256 */
    char *argv[] = {"podvm", "-m", "256", "/rootfs", "/bin/sh", NULL};
    int argc = 5;
    
    int ret = parse_args(argc, argv);
    assert(ret == 3);  /* Should stop at rootfs */
    assert(config.memory == 256);
    assert(config.interactive == 0);
    
    printf("PASS: test_m_flag\n");
}

/* Test -c flag */
static void test_c_flag(void) {
    /* Reset */
    config.cpus = 1;
    
    /* Test -c 4 */
    char *argv[] = {"podvm", "-c", "4", "/rootfs", "/bin/sh", NULL};
    int argc = 5;
    
    int ret = parse_args(argc, argv);
    assert(ret == 3);
    assert(config.cpus == 4);
    
    printf("PASS: test_c_flag\n");
}

/* Test combined flags */
static void test_combined_flags(void) {
    /* Reset */
    config.memory = 128;
    config.cpus = 1;
    config.interactive = 0;
    
    /* Test -it -m 256 -c 2 */
    char *argv[] = {"podvm", "-it", "-m", "256", "-c", "2", "/rootfs", "/bin/sh", NULL};
    int argc = 8;
    
    int ret = parse_args(argc, argv);
    assert(ret == 6);  /* Should stop at rootfs */
    assert(config.interactive == 1);
    assert(config.memory == 256);
    assert(config.cpus == 2);
    
    printf("PASS: test_combined_flags\n");
}

/* Test invalid option */
static void test_invalid_option(void) {
    /* Reset */
    config.interactive = 0;
    
    /* Test -x */
    char *argv[] = {"podvm", "-x", "/rootfs", "/bin/sh", NULL};
    int argc = 4;
    
    int ret = parse_args(argc, argv);
    assert(ret == -2);  /* Invalid option */
    
    printf("PASS: test_invalid_option\n");
}

/* Test config building */
static void test_build_config(void) {
    /* Simulate building vsock config string */
    char vsock_config[4096];
    char entrypoint[] = "/bin/sh";
    char args[] = "-l";
    char env[] = "HOME=/root";
    
    snprintf(vsock_config, sizeof(vsock_config),
             "podvm.root=/rootfs "
             "podvm.entrypoint=%s "
             "podvm.args=%s "
             "podvm.env=%s",
             entrypoint,
             args[0] ? args : "-",
             env[0] ? env : "HOME=/root");
    
    assert(strstr(vsock_config, "podvm.root=/rootfs") != NULL);
    assert(strstr(vsock_config, "podvm.entrypoint=/bin/sh") != NULL);
    assert(strstr(vsock_config, "podvm.args=-l") != NULL);
    assert(strstr(vsock_config, "podvm.env=HOME=/root") != NULL);
    
    /* Test with empty args */
    args[0] = '\0';
    snprintf(vsock_config, sizeof(vsock_config),
             "podvm.root=/rootfs "
             "podvm.entrypoint=%s "
             "podvm.args=%s "
             "podvm.env=%s",
             entrypoint,
             args[0] ? args : "-",
             env[0] ? env : "HOME=/root");
    
    assert(strstr(vsock_config, "podvm.args=-") != NULL);
    
    printf("PASS: test_build_config\n");
}

/* Test memory backend building */
static void test_build_memory_backend(void) {
    char memory_backend[64];
    int memory = 512;
    
    snprintf(memory_backend, sizeof(memory_backend),
             "memory-backend-memfd,id=mem,size=%dM,share=on", memory);
    
    assert(strcmp(memory_backend, "memory-backend-memfd,id=mem,size=512M,share=on") == 0);
    
    /* Test different sizes */
    memory = 1024;
    snprintf(memory_backend, sizeof(memory_backend),
             "memory-backend-memfd,id=mem,size=%dM,share=on", memory);
    
    assert(strcmp(memory_backend, "memory-backend-memfd,id=mem,size=1024M,share=on") == 0);
    
    printf("PASS: test_build_memory_backend\n");
}

int main(void) {
    printf("Running host argument tests...\n\n");
    
    test_it_flag();
    test_m_flag();
    test_c_flag();
    test_combined_flags();
    test_invalid_option();
    test_build_config();
    test_build_memory_backend();
    
    printf("\nAll tests passed!\n");
    return 0;
}
