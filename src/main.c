/*
 * podvm - VM-based container runtime
 *
 * PoC launcher: starts QEMU with 9p for container filesystem sharing.
 * (virtiofs is deferred until the performance phase; see ADR-022.)
 * Uses vsock for host↔guest communication.
 *
 * Usage: podvm [options] <image|rootfs-dir> [command] [args...]
 *
 * Build: gcc -O2 -o podvm src/main.c
 */

#define _GNU_SOURCE   /* unshare, CLONE_NEWUSER */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>

#include "image.h"
#include "store.h"

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
    .kernel = "",  /* Empty = use host kernel */
    .initramfs = "/var/lib/podvm/kernel/initramfs.cpio",
    .memory = 128,
    .cpus = 1,
    .interactive = 0,
};

/* Get host kernel path */
static void get_host_kernel(char *path, size_t path_size) {
    /* Try /boot/vmlinuz-$(uname -r) */
    FILE *f = popen("uname -r", "r");
    if (f) {
        char release[256];
        if (fgets(release, sizeof(release), f)) {
            /* Remove newline */
            release[strcspn(release, "\n")] = '\0';
            snprintf(path, path_size, "/boot/vmlinuz-%s", release);
            
            struct stat st;
            if (stat(path, &st) == 0) {
                pclose(f);
                return;
            }
        }
        pclose(f);
    }
    
    /* Fallback */
    strcpy(path, "/boot/vmlinuz");
}

/* QEMU process */
static pid_t qemu_pid = 0;

/* Signal handler for cleanup */
static volatile int running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* vsock port (must match guest listener) */
#define VSOCK_PORT  5000
/* Guest CID (must match -device vhost-vsock-pci,guest-cid=) */
#define GUEST_CID_STR "3"
#define GUEST_CID     3

/* vsock availability on the host (auto-detected) */
static int vsock_available = 0;

/* Podman-storage mode: rootfs is an overlay over podman layer diffs,
 * mounted inside the QEMU child's user namespace (see run_qemu_child). */
#define MAX_CHAIN 64
static StoreLayer store_layers[MAX_CHAIN];
static int store_nlayers = 0;

/* Detect host vsock support via /dev/vhost-vsock */
static void detect_vsock(void) {
    struct stat st;
    vsock_available = (stat("/dev/vhost-vsock", &st) == 0);
    if (!vsock_available) {
        fprintf(stderr, "podvm: /dev/vhost-vsock not present; "
                        "falling back to cmdline config only\n");
    }
}

/* Send config to guest via vsock.
 * The guest listens on GUEST_CID:VSOCK_PORT and reads a single line of
 * space-separated podvm.*=... tokens (same format as the kernel cmdline).
 * Returns 0 on success, -1 on failure (caller falls back to cmdline). */
static int send_config_vsock(const char *config_str) {
    struct sockaddr_vm addr;
    int fd = -1;
    int connected = 0;

    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = GUEST_CID;   /* connect to the guest, not CID_ANY */
    addr.svm_port = VSOCK_PORT;

    /* Retry until the guest listener comes up (guest boots async).
     * A fresh socket is required per attempt: a failed connect() leaves
     * the socket in an undefined state. */
    for (int i = 0; i < 100 && !connected; i++) {
        fd = socket(AF_VSOCK, SOCK_STREAM, 0);
        if (fd < 0) {
            fprintf(stderr, "podvm: vsock socket failed: %s\n", strerror(errno));
            return -1;
        }

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            connected = 1;
            break;
        }

        /* Only retry on transient errors; a hard error is fatal. */
        if (errno != ECONNRESET && errno != EAGAIN && errno != EINTR &&
            errno != ENODEV && errno != EOPNOTSUPP && errno != ETIMEDOUT) {
            close(fd);
            fd = -1;
            break;
        }
        close(fd);
        fd = -1;
        usleep(20000);  /* 20ms */
    }

    if (!connected) {
        fprintf(stderr, "podvm: vsock connect to cid %d port %d failed: %s\n",
                GUEST_CID, VSOCK_PORT, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }

    /* Send config as one newline-terminated line. */
    char line[MAX_CMDLINE + 2];
    int n = snprintf(line, sizeof(line), "%s\n", config_str);
    if (write(fd, line, (size_t)n) != (ssize_t)n) {
        fprintf(stderr, "podvm: vsock write failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Read ACK (best-effort). */
    char buf[8];
    read(fd, buf, sizeof(buf));

    fprintf(stderr, "podvm: config sent to guest over vsock\n");

    close(fd);
    return 0;
}

/* Build kernel cmdline */
static void build_cmdline(char *buf, size_t len) {
    snprintf(buf, len,
        "console=ttyS0 "
        "panic=-1 "              /* reboot on panic; -no-reboot exits QEMU */
        "podvm.root=/rootfs "
        "podvm.entrypoint=%s "
        "podvm.args=%s "
        "podvm.env=%s",
        config.entrypoint,
        config.args[0] ? config.args : "-",
        config.env[0] ? config.env : "HOME=/root");
}

/*
 * Enter a user+mount namespace and mount the podman layer chain as an
 * overlay at config.rootfs_dir. Runs in the QEMU child after fork().
 *
 * Rootless podman storage is only a valid rootfs inside a userns: the
 * diff dirs are plain dirs, and the union needs CAP_SYS_ADMIN in a
 * namespace to mount overlay. The VM's isolation boundary is the VM
 * (ADR-001); staging the files through a userns changes nothing about
 * that (ADR-024).
 *
 * uid_map: map our uid to 0 inside the ns so the overlay mount is
 * permitted; 9p then serves the files as the invoking user outside.
 */
static int setup_userns_overlay(void) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    char buf[64];

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0) {
        fprintf(stderr, "podvm: unshare user+mnt failed: %s\n", strerror(errno));
        return -1;
    }

    /* Deny setgroups, then map uid/gid. Must be done in this order. */
    int fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) { (void)!write(fd, "deny", 4); close(fd); }

    snprintf(buf, sizeof(buf), "0 %u 1", uid);
    fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        fprintf(stderr, "podvm: uid_map failed: %s\n", strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);

    snprintf(buf, sizeof(buf), "0 %u 1", gid);
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        fprintf(stderr, "podvm: gid_map failed: %s\n", strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);

    /* Stop mount propagation so our tmpfs/overlay stay private. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        fprintf(stderr, "podvm: make-rprivate failed: %s\n", strerror(errno));
        return -1;
    }

    /* upper + work on a private tmpfs; merged is the 9p-served rootfs */
    char basedir[STORE_PATH_MAX], updir[STORE_PATH_MAX], wkdir[STORE_PATH_MAX];
    snprintf(basedir, sizeof(basedir), "/tmp/podvm-ovl.%d", getpid());
    snprintf(updir, sizeof(updir), "%s/u", basedir);
    snprintf(wkdir, sizeof(wkdir), "%s/w", basedir);

    if (mkdir(basedir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "podvm: mkdir %s failed: %s\n", basedir, strerror(errno));
        return -1;
    }
    if (mount("tmpfs", basedir, "tmpfs", 0, "mode=0755") < 0) {
        fprintf(stderr, "podvm: tmpfs for upper failed: %s\n", strerror(errno));
        return -1;
    }
    mkdir(updir, 0755);
    mkdir(wkdir, 0755);
    if (mkdir(config.rootfs_dir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "podvm: mkdir %s failed: %s\n",
                config.rootfs_dir, strerror(errno));
        return -1;
    }

    /* Build lowerdir chain: top-first, colon-separated */
    char lowers[STORE_PATH_MAX * 8];
    size_t off = 0;
    for (int i = 0; i < store_nlayers; i++) {
        int w = snprintf(lowers + off, sizeof(lowers) - off, "%s%s",
                         i ? ":" : "", store_layers[i].diff);
        if (w < 0 || (size_t)w >= sizeof(lowers) - off) {
            fprintf(stderr, "podvm: lowerdir chain too long\n");
            return -1;
        }
        off += (size_t)w;
    }

    char opts[STORE_PATH_MAX * 9];
    snprintf(opts, sizeof(opts),
             "lowerdir=%s,upperdir=%s,workdir=%s,userxattr",
             lowers, updir, wkdir);

    if (mount("overlay", config.rootfs_dir, "overlay", 0, opts) < 0) {
        fprintf(stderr, "podvm: overlay mount failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Start QEMU (non-blocking) */
static int start_qemu(void) {
    char cmdline[MAX_CMDLINE];
    char memory_str[32];
    char cpu_str[32];
    char fsdev_str[MAX_PATH + 64];
    char vsock_cid_str[32];

    /* Bounded argv so we can conditionally append the vsock device. */
    const char *qargs[64];
    int qi = 0;

    build_cmdline(cmdline, sizeof(cmdline));
    snprintf(memory_str, sizeof(memory_str), "%dM", config.memory);
    snprintf(cpu_str, sizeof(cpu_str), "%d", config.cpus);
    snprintf(fsdev_str, sizeof(fsdev_str),
             "local,id=fsdev0,path=%s,security_model=none",
             config.rootfs_dir);
    snprintf(vsock_cid_str, sizeof(vsock_cid_str), "%d", GUEST_CID);

    printf("podvm: starting QEMU\n");
    printf("podvm:   kernel:    %s\n", config.kernel);
    printf("podvm:   initramfs: %s\n", config.initramfs);
    printf("podvm:   rootfs:    %s (9p)\n", config.rootfs_dir);
    printf("podvm:   memory:    %s\n", memory_str);
    printf("podvm:   cpus:      %s\n", cpu_str);
    printf("podvm:   vsock:     %s (cid %s)\n",
           vsock_available ? "yes" : "no",
           vsock_available ? vsock_cid_str : "-");
    printf("podvm:   cmdline:   %s\n", cmdline);

    /* Machine / resources */
    qargs[qi++] = "qemu-system-x86_64";
    qargs[qi++] = "-machine"; qargs[qi++] = "q35,accel=kvm";
    qargs[qi++] = "-cpu";     qargs[qi++] = "host";
    qargs[qi++] = "-m";       qargs[qi++] = memory_str;
    qargs[qi++] = "-smp";     qargs[qi++] = cpu_str;
    /* 9p filesystem share (in-process; no daemon needed) */
    qargs[qi++] = "-fsdev";   qargs[qi++] = fsdev_str;
    qargs[qi++] = "-device";  qargs[qi++] = "virtio-9p-pci,fsdev=fsdev0,mount_tag=rootfs";
    /* User-mode networking (slirp): in-process, no daemon, no root,
     * no host state (ADR-023). Guest gets DHCP 10.0.2.15, DNS at
     * 10.0.2.3, gateway 10.0.2.2. Bridge/tap returns in the
     * performance phase. */
    qargs[qi++] = "-netdev";  qargs[qi++] = "user,id=net0";
    qargs[qi++] = "-device";  qargs[qi++] = "virtio-net-pci,netdev=net0";
    /* vsock (only when the host supports it) */
    if (vsock_available) {
        qargs[qi++] = "-device";
        qargs[qi++] = "vhost-vsock-pci,id=vsock0,guest-cid=" GUEST_CID_STR;
    }
    /* Kernel + initrd + cmdline */
    qargs[qi++] = "-kernel";  qargs[qi++] = config.kernel;
    qargs[qi++] = "-initrd";  qargs[qi++] = config.initramfs;
    qargs[qi++] = "-append";  qargs[qi++] = cmdline;
    /* Serial / lifecycle */
    qargs[qi++] = "-serial";  qargs[qi++] = "mon:stdio";
    qargs[qi++] = "-nographic";
    qargs[qi++] = "-no-reboot";
    qargs[qi] = NULL;

    qemu_pid = fork();
    if (qemu_pid == 0) {
        /* Child: stage the rootfs (podman layers via userns overlay),
         * then exec QEMU inside that namespace. */
        if (store_nlayers > 0 && setup_userns_overlay() < 0)
            _exit(1);
        execvp("qemu-system-x86_64", (char *const *)qargs);
        fprintf(stderr, "podvm: exec qemu failed: %s\n", strerror(errno));
        _exit(1);
    }
    if (qemu_pid < 0) {
        fprintf(stderr, "podvm: fork qemu failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/* Wait for QEMU to exit */
static int wait_qemu(void) {
    int status;
    waitpid(qemu_pid, &status, 0);
    qemu_pid = 0;

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

/* Print usage */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <image|rootfs-dir> [command] [args...]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -i, --interactive   Interactive (attach stdin/stdout)\n");
    fprintf(stderr, "  -m, --memory <MB>   Memory in MB (default: 128)\n");
    fprintf(stderr, "  -c, --cpus <cpus>   CPUs (default: 1)\n");
    fprintf(stderr, "  -k, --kernel <path> Kernel path\n");
    fprintf(stderr, "  -r, --initramfs <path> Initramfs path\n");
    fprintf(stderr, "  -h, --help          Show this help\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -i debian /bin/sh\n", prog);
    fprintf(stderr, "  %s -i debian echo hello\n", prog);
    fprintf(stderr, "  %s -i /path/to/rootfs /bin/sh\n", prog);
}

int main(int argc, char **argv) {
    int opt;
    int option_index = 0;
    
    static struct option long_options[] = {
        {"interactive", no_argument,       0, 'i'},
        {"memory",      required_argument, 0, 'm'},
        {"cpus",        required_argument, 0, 'c'},
        {"kernel",      required_argument, 0, 'k'},
        {"initramfs",   required_argument, 0, 'r'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    /* '+' stops getopt at the first non-option (the image/rootfs), so
     * flags meant for the entrypoint (e.g. `podvm img /bin/sh -c ...`)
     * are not stolen as podvm options. */
    while ((opt = getopt_long(argc, argv, "+:im:c:k:r:h", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'i':
            config.interactive = 1;
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
        case 'r':
            strncpy(config.initramfs, optarg, MAX_PATH - 1);
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        case ':':
            fprintf(stderr, "podvm: option '-%c' requires an argument\n", optopt);
            usage(argv[0]);
            return 1;
        case '?':
        default:
            fprintf(stderr, "podvm: invalid option -- '%c'\n", optopt);
            usage(argv[0]);
            return 1;
        }
    }

    /* Need at least image/rootfs and command */
    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    /* Get image or rootfs directory */
    strncpy(config.rootfs_dir, argv[optind], MAX_PATH - 1);
    optind++;

    /* Get entrypoint (if provided) */
    if (optind < argc) {
        strncpy(config.entrypoint, argv[optind], MAX_PATH - 1);
        optind++;

        /* Build args string */
        char *p = config.args;
        char *end = config.args + sizeof(config.args) - 1;
        for (int j = optind; j < argc && p < end; j++) {
            if (j > optind) *p++ = ',';
            int n = snprintf(p, end - p, "%s", argv[j]);
            p += n;
        }
        *p = '\0';
    } else {
        /* No command specified, use image entrypoint or default */
        config.entrypoint[0] = '\0';
    }

    /* Resolve kernel path */
    if (config.kernel[0] == '\0') {
        get_host_kernel(config.kernel, sizeof(config.kernel));
        printf("podvm: using host kernel: %s\n", config.kernel);
    }
    
    /* Resolve rootfs/image */
    char resolved_rootfs[MAX_PATH];

    /*
     * Resolution order:
     *   1. directory path -> use as-is
     *   2. podman storage -> layers in place (pull via podman if missing)
     *   3. podvm's own registry pull -> cache
     */
    if (image_is_path(config.rootfs_dir)) {
        if (image_resolve(config.rootfs_dir, resolved_rootfs,
                          sizeof(resolved_rootfs)) < 0) {
            fprintf(stderr, "podvm: directory not found: %s\n", config.rootfs_dir);
            return 1;
        }
    } else {
        char top[STORE_PATH_MAX];
        if (store_resolve(config.rootfs_dir, top, sizeof(top), 1) == 0) {
            store_nlayers = store_chain(top, store_layers, MAX_CHAIN);
            if (store_nlayers <= 0) {
                fprintf(stderr, "podvm: no layer chain for %s\n", config.rootfs_dir);
                return 1;
            }
            printf("podvm: using podman storage: %s (%d layers)\n",
                   config.rootfs_dir, store_nlayers);
            /* Served dir is created by the QEMU child's overlay mount */
            snprintf(resolved_rootfs, sizeof(resolved_rootfs),
                     "/tmp/podvm-rootfs.%d", getpid());
        } else if (image_resolve(config.rootfs_dir, resolved_rootfs,
                                 sizeof(resolved_rootfs)) < 0) {
            fprintf(stderr, "podvm: failed to resolve image/path: %s\n",
                    config.rootfs_dir);
            return 1;
        }
    }
    
    /* Copy resolved path back */
    strncpy(config.rootfs_dir, resolved_rootfs, MAX_PATH - 1);
    config.rootfs_dir[MAX_PATH - 1] = '\0';

    /* Setup signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Detect host vsock support (determines QEMU device + config channel). */
    detect_vsock();

    /* Start QEMU (9p serves the rootfs in-process; no virtiofsd) */
    if (start_qemu() < 0) {
        fprintf(stderr, "podvm: failed to start QEMU\n");
        return 1;
    }

    /* Send config to guest over vsock (best-effort; cmdline is the
     * authoritative fallback and is always passed via -append above). */
    if (vsock_available) {
        char vsock_config[MAX_CMDLINE];
        snprintf(vsock_config, sizeof(vsock_config),
                 "podvm.root=%s "
                 "podvm.entrypoint=%s "
                 "podvm.args=%s "
                 "podvm.env=%s",
                 "/rootfs",
                 config.entrypoint,
                 config.args[0] ? config.args : "-",
                 config.env[0] ? config.env : "HOME=/root");

        printf("podvm: sending config to guest over vsock\n");
        if (send_config_vsock(vsock_config) < 0) {
            fprintf(stderr, "podvm: vsock config failed; guest will use cmdline\n");
        }
    }

    /* Wait for QEMU to exit */
    return wait_qemu();
}
