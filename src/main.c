/*
 * podvm - VM-based container runtime
 *
 * PoC launcher: starts QEMU with virtiofs for container filesystem.
 *
 * Usage: podvm run [-it] <image-dir> <command> [args...]
 *
 * Build: gcc -O2 -o podvm src/main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>

#define MAX_PATH    1024
#define MAX_CMDLINE 4096

/* Configuration */
static struct {
    char kernel[MAX_PATH];
    char initramfs[MAX_PATH];
    char rootfs_dir[MAX_PATH];
    char entrypoint[MAX_PATH];
    char args[256];
    char env[1024];
    int  memory;       /* MB */
    int  cpus;
    int  interactive;  /* -it flag */
} config = {
    .kernel = "/var/lib/podvm/kernel/bzImage",
    .initramfs = "/var/lib/podvm/kernel/initramfs.cpio.gz",
    .memory = 128,
    .cpus = 1,
    .interactive = 0,
};

/* virtiofsd process */
static pid_t virtiofsd_pid = 0;

/* Signal handler for cleanup */
static volatile int running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* Build kernel cmdline */
static void build_cmdline(char *buf, size_t len) {
    snprintf(buf, len,
        "podvm.root=/rootfs "
        "podvm.entrypoint=%s "
        "podvm.args=%s "
        "podvm.env=%s",
        config.entrypoint,
        config.args[0] ? config.args : "-",
        config.env[0] ? config.env : "HOME=/root");
}

/* Start virtiofsd */
static int start_virtiofsd(const char *socket_path, const char *shared_dir) {
    virtiofsd_pid = fork();
    if (virtiofsd_pid == 0) {
        /* Child: exec virtiofsd */
        execlp("virtiofsd", "virtiofsd",
               "--socket-path", socket_path,
               "--shared-dir", shared_dir,
               "--cache", "auto",
               "--sandbox", "none",
               NULL);
        fprintf(stderr, "podvm: exec virtiofsd failed: %s\n", strerror(errno));
        _exit(1);
    }
    if (virtiofsd_pid < 0) {
        fprintf(stderr, "podvm: fork virtiofsd failed: %s\n", strerror(errno));
        return -1;
    }

    /* Wait for socket to appear */
    for (int i = 0; i < 50; i++) {
        struct stat st;
        if (stat(socket_path, &st) == 0)
            return 0;
        usleep(10000); /* 10ms */
    }

    fprintf(stderr, "podvm: virtiofsd socket did not appear\n");
    return -1;
}

/* Stop virtiofsd */
static void stop_virtiofsd(void) {
    if (virtiofsd_pid > 0) {
        kill(virtiofsd_pid, SIGTERM);
        waitpid(virtiofsd_pid, NULL, 0);
        virtiofsd_pid = 0;
    }
}

/* Start QEMU */
static int start_qemu(const char *virtiofs_socket) {
    char cmdline[MAX_CMDLINE];
    char memory_str[32];
    char cpu_str[32];

    build_cmdline(cmdline, sizeof(cmdline));
    snprintf(memory_str, sizeof(memory_str), "%dM", config.memory);
    snprintf(cpu_str, sizeof(cpu_str), "%d", config.cpus);

    printf("podvm: starting QEMU\n");
    printf("podvm:   kernel:    %s\n", config.kernel);
    printf("podvm:   initramfs: %s\n", config.initramfs);
    printf("podvm:   rootfs:    %s\n", config.rootfs_dir);
    printf("podvm:   cmdline:   %s\n", cmdline);

    pid_t qemu_pid = fork();
    if (qemu_pid == 0) {
        /* Child: exec QEMU */
        execlp("qemu-system-x86_64", "qemu-system-x86_64",
               /* Machine */
               "-machine", "q35,accel=kvm",
               "-cpu", "host",
               /* Resources */
               "-m", memory_str,
               "-smp", cpu_str,
               /* Memory backend (required for virtiofs) */
               "-object", "memory-backend-memfd,id=mem,size=128M,share=on",
               "-numa", "node,memdev=mem",
               /* virtiofs */
               "-chardev", "socket,id=char0,path=/run/podvm/virtiofs-rootfs.sock",
               "-device", "vhost-user-fs-pci,chardev=char0,tag=rootfs",
               /* Kernel */
               "-kernel", config.kernel,
               "-initrd", config.initramfs,
               "-append", cmdline,
               /* Serial */
               "-serial", "mon:stdio",
               "-nographic",
               "-no-reboot",
               NULL);
        fprintf(stderr, "podvm: exec qemu failed: %s\n", strerror(errno));
        _exit(1);
    }
    if (qemu_pid < 0) {
        fprintf(stderr, "podvm: fork qemu failed: %s\n", strerror(errno));
        return -1;
    }

    /* Wait for QEMU to exit */
    int status;
    waitpid(qemu_pid, &status, 0);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

/* Print usage */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <rootfs-dir> <command> [args...]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -it          Interactive (attach stdin/stdout)\n");
    fprintf(stderr, "  -m <MB>      Memory in MB (default: 128)\n");
    fprintf(stderr, "  -c <cpus>    CPUs (default: 1)\n");
    fprintf(stderr, "  -k <path>    Kernel path\n");
    fprintf(stderr, "  -i <path>    Initramfs path\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s -it /var/lib/containers/storage/overlay/layer/abc123 /bin/sh\n", prog);
}

int main(int argc, char **argv) {
    int opt;

    while ((opt = getopt(argc, argv, "im:c:k:i:h")) != -1) {
        switch (opt) {
        case 'i':
            /* Check if this is -it or -i (initramfs) */
            if (optind < argc && argv[optind][0] != '-') {
                /* -i <path>: initramfs */
                strncpy(config.initramfs, optarg, MAX_PATH - 1);
            } else {
                /* -it: interactive */
                config.interactive = 1;
            }
            break;
        case 'm':
            config.memory = atoi(optarg);
            break;
        case 'c':
            config.cpus = atoi(optarg);
            break;
        case 'k':
            strncpy(config.kernel, optarg, MAX_PATH - 1);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Need at least rootfs-dir and command */
    if (optind + 1 >= argc) {
        usage(argv[0]);
        return 1;
    }

    /* Get rootfs directory */
    strncpy(config.rootfs_dir, argv[optind], MAX_PATH - 1);
    optind++;

    /* Get entrypoint */
    strncpy(config.entrypoint, argv[optind], MAX_PATH - 1);
    optind++;

    /* Build args string */
    char *p = config.args;
    char *end = config.args + sizeof(config.args) - 1;
    for (int i = optind; i < argc && p < end; i++) {
        if (i > optind) *p++ = ',';
        int n = snprintf(p, end - p, "%s", argv[i]);
        p += n;
    }
    *p = '\0';

    /* Validate rootfs directory */
    struct stat st;
    if (stat(config.rootfs_dir, &st) < 0) {
        fprintf(stderr, "podvm: rootfs directory not found: %s\n", config.rootfs_dir);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "podvm: rootfs is not a directory: %s\n", config.rootfs_dir);
        return 1;
    }

    /* Setup signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Create socket directory */
    mkdir("/run/podvm", 0755);

    /* Start virtiofsd */
    const char *socket_path = "/run/podvm/virtiofs-rootfs.sock";
    printf("podvm: starting virtiofsd\n");
    if (start_virtiofsd(socket_path, config.rootfs_dir) < 0) {
        fprintf(stderr, "podvm: failed to start virtiofsd\n");
        return 1;
    }

    /* Start QEMU */
    int ret = start_qemu(socket_path);

    /* Cleanup */
    stop_virtiofsd();
    unlink(socket_path);

    return ret;
}
