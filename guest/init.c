/*
 * podvm guest init
 *
 * Minimal init for podvm guests (runs as PID 1).
 *
 * Config comes from two sources, in priority order:
 *   1. kernel cmdline  (podvm.*=... tokens, always present via -append)
 *   2. host vsock      (overrides cmdline when the host connects and sends)
 *
 * Flow: mount pseudofs -> load 9p modules (if bundled) -> parse cmdline
 * -> try vsock override -> mount rootfs (9p) -> chroot -> set cwd/user
 * -> exec entrypoint.
 *
 * Build: musl-gcc -static -Os (see guest/Makefile)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/vm_sockets.h>

#include "strparse.h"

#define MAX_ARGS      64
#define PATH_SIZE     1024
#define CMDLINE_SIZE  4096
#define MAX_ARG_VAL   256   /* matches the host's args[] buffer */
#define MAX_ENV_VAL   1100  /* matches the host's env[] buffer (1024) */

/* vsock config channel (must match host) */
#define VSOCK_PORT       5000
#define VSOCK_TIMEOUT_MS 3000

/* Configuration from kernel cmdline / host vsock */
typedef struct {
    char root[PATH_SIZE];
    char entrypoint[PATH_SIZE];
    char *args[MAX_ARGS];
    int  argc;
    char *env[MAX_ARGS];   /* "K=V" strings from podvm.env= */
    int  nenv;
    char cwd[PATH_SIZE];
    char hostname[256];
    int  uid;
    int  gid;
} Config;

/* Read /proc/cmdline */
static int read_cmdline(char *buf, size_t len) {
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

/*
 * Parse config tokens from a "podvm.*=..." string (cmdline or vsock payload).
 * Only keys present in the input are set, so a later call layers overrides
 * on top of an earlier one without clearing unrelated fields.
 */
static void parse_config(Config *cfg, const char *text) {
    const char *val;
    /* kv_copy_value passes escape sequences through verbatim; single-valued
     * fields are unescaped here, and list fields (args/env) are unescaped
     * per field by csv_parse. */

    val = kv_find(text, "podvm.root=");
    if (val) { kv_copy_value(cfg->root, PATH_SIZE, val); kv_unescape(cfg->root); }

    val = kv_find(text, "podvm.entrypoint=");
    if (val) { kv_copy_value(cfg->entrypoint, PATH_SIZE, val); kv_unescape(cfg->entrypoint); }

    val = kv_find(text, "podvm.args=");
    if (val) {
        char tok[MAX_ARG_VAL];
        kv_copy_value(tok, sizeof(tok), val);   /* trim at token boundary */
        csv_free(cfg->args, cfg->argc);   /* free any previous args */
        /* The host sends "-" as the empty-args sentinel; treat it as
         * no args, not a literal "-" argument. */
        if (strcmp(tok, "-") == 0) {
            cfg->argc = 0;
        } else {
            cfg->argc = csv_parse(tok, cfg->args, MAX_ARGS);
        }
    }

    val = kv_find(text, "podvm.cwd=");
    if (val) { kv_copy_value(cfg->cwd, PATH_SIZE, val); kv_unescape(cfg->cwd); }

    /* podvm.env= is a comma-separated list of K=V pairs. The host has
     * always sent this; the guest previously ignored it, so image
     * environment variables never reached the entrypoint. */
    val = kv_find(text, "podvm.env=");
    if (val) {
        char tok[MAX_ENV_VAL];
        kv_copy_value(tok, sizeof(tok), val);   /* trim at token boundary */
        csv_free(cfg->env, cfg->nenv);
        if (strcmp(tok, "-") == 0) {
            cfg->nenv = 0;
        } else {
            cfg->nenv = csv_parse(tok, cfg->env, MAX_ARGS);
        }
    }

    val = kv_find(text, "podvm.hostname=");
    if (val) { kv_copy_value(cfg->hostname, 256, val); kv_unescape(cfg->hostname); }

    val = kv_find(text, "podvm.user=");
    if (val) {
        char tok[128];
        kv_copy_value(tok, sizeof(tok), val);
        kv_unescape(tok);
        parse_uid_gid(tok, &cfg->uid, &cfg->gid);
    }
}

/*
 * Listen on vsock and, if the host connects and sends a config line within
 * the timeout, copy it into buf. Returns bytes read, or -1 on
 * unsupported/timeout/error (caller falls back to cmdline-only config).
 */
static int recv_config_vsock(char *buf, size_t len, int timeout_ms) {
    struct sockaddr_vm addr;
    struct pollfd pfd;
    int lfd, cfd;
    size_t n = 0;

    lfd = socket(AF_VSOCK, SOCK_STREAM, 0);
    if (lfd < 0)
        return -1;   /* vsock not available in this guest */

    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = VSOCK_PORT;

    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(lfd, 1) < 0) {
        close(lfd);
        return -1;
    }

    /* Wait (bounded) for the host to connect. */
    pfd.fd = lfd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout_ms) <= 0) {
        close(lfd);   /* timeout or poll error: no config from host */
        return -1;
    }

    cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
        close(lfd);
        return -1;
    }

    /* Read one newline-terminated config line. */
    while (n < len - 1) {
        ssize_t r = read(cfd, buf + n, len - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (memchr(buf, '\n', n)) break;
    }
    buf[n] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    /* ACK so the host knows the config was received. */
    (void)!write(cfd, "OK", 2);

    close(cfd);
    close(lfd);
    return (int)n;
}

/* Create directory recursively */
static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_SIZE];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

/* Mount pseudo-filesystems */
/* Mount pseudo-filesystems under a root prefix. Used twice: once at the
 * initramfs root (before chroot, for init's own use) and once inside the
 * container rootfs after chroot -- chroot hides the initramfs mounts, so
 * /proc /sys /dev must be re-mounted in the new root or tools that read
 * /proc/net, /proc/self, etc. (ifconfig, curl, ps) fail. */
static void mount_pseudofs_at(const char *root) {
    char p[PATH_SIZE];
#define PF(type, dir, opts) do { \
    snprintf(p, sizeof(p), "%s/%s", root, dir); \
    mount(type, p, type, 0, opts); \
} while (0)
    PF("proc", "proc", NULL);
    PF("sysfs", "sys", NULL);
    PF("devtmpfs", "dev", NULL);
    snprintf(p, sizeof(p), "%s/dev/pts", root); mkdir(p, 0755);
    mount("devpts", p, "devpts", 0, NULL);
    snprintf(p, sizeof(p), "%s/tmp", root); mount("tmpfs", p, "tmpfs", 0, NULL);
#undef PF
}

static void mount_pseudofs(void) {
    mount_pseudofs_at("");
}

/*
 * Load guest kernel modules from /modules in the initramfs, if present.
 * 9p: netfs (dep of 9pnet on kernels >= ~6.6), 9pnet, 9pnet_virtio, 9p.
 * vsock: vsock, virtio transport common, virtio transport.
 * fscache is built into the host kernel here, so only netfs is bundled.
 *
 * Needed when booting the host kernel: KVM guests are a separate kernel
 * instance, so the host's loaded modules don't count, and module
 * autoload can't work (no modprobe in the initramfs). The initramfs
 * build script bundles decompressed copies from the running host kernel.
 *
 * The synthetic kernel builds everything in and ships no /modules dir;
 * this is a no-op there.
 */
static void load_guest_modules(void) {
    static const char *order[] = {
        "netfs.ko",
        "9pnet.ko", "9pnet_virtio.ko", "9p.ko",
        "vsock.ko",
        "vmw_vsock_virtio_transport_common.ko",
        "vmw_vsock_virtio_transport.ko",
        "failover.ko", "net_failover.ko", "virtio_net.ko",
        NULL
    };
    char path[PATH_SIZE];

    for (int i = 0; order[i]; i++) {
        snprintf(path, sizeof(path), "/modules/%s", order[i]);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;   /* not bundled: built-in or absent */
        if (syscall(__NR_finit_module, fd, "", 0) < 0 && errno != EEXIST)
            fprintf(stderr, "podvm-init: finit_module %s: %s\n",
                    order[i], strerror(errno));
        close(fd);
    }
}

/*
 * Configure eth0 with a static config matching QEMU slirp's well-known
 * constants (ADR-023): guest 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.
 *
 * Slirp serves DHCP, but there is no DHCP client in the initramfs and the
 * host->guest config channel is one-way, so we set the address statically.
 * Best-effort: if eth0 is absent (no virtio-net device, or a synthetic
 * kernel without netdev) we return quietly and the guest runs without
 * network -- isolation and boot are unaffected.
 */
#define SLIRP_IP      "10.0.2.15"
#define SLIRP_NETMASK "255.255.255.0"
#define SLIRP_GATEWAY "10.0.2.2"
#define SLIRP_DNS     "10.0.2.3"

static int setup_network(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "eth0");

    /* Address */
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, SLIRP_IP, &sin->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        fprintf(stderr, "podvm-init: eth0 absent or unconfigurable: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    /* Netmask */
    inet_pton(AF_INET, SLIRP_NETMASK, &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0)
        fprintf(stderr, "podvm-init: eth0 netmask: %s\n", strerror(errno));

    /* Bring the interface up */
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
            fprintf(stderr, "podvm-init: eth0 up: %s\n", strerror(errno));
    }

    /* Default route via slirp gateway */
    struct rtentry rt;
    memset(&rt, 0, sizeof(rt));
    sin = (struct sockaddr_in *)&rt.rt_dst;
    sin->sin_family = AF_INET;          /* 0.0.0.0 = default */
    sin = (struct sockaddr_in *)&rt.rt_genmask;
    sin->sin_family = AF_INET;          /* 0.0.0.0 mask */
    sin = (struct sockaddr_in *)&rt.rt_gateway;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, SLIRP_GATEWAY, &sin->sin_addr);
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    rt.rt_dev = (char *)ifr.ifr_name;
    if (ioctl(fd, SIOCADDRT, &rt) < 0 && errno != EEXIST)
        fprintf(stderr, "podvm-init: default route: %s\n", strerror(errno));

    close(fd);
    fprintf(stderr, "podvm-init: eth0 up %s via %s\n", SLIRP_IP, SLIRP_GATEWAY);
    return 0;
}

/* Mount container rootfs via 9p */
static int mount_rootfs(const Config *cfg) {
    mkdir_p(cfg->root, 0755);

    /* 9p: the mount *source* is the tag (fs_context conversion, 5.14+).
     * 'tag=' as a data option is no longer accepted (EINVAL). */
    if (mount("rootfs", cfg->root, "9p", 0,
              "trans=virtio,version=9p2000.L") == 0)
        return 0;

    fprintf(stderr, "podvm-init: mount rootfs failed: %s\n", strerror(errno));
    return -1;
}

int main(void) {
    Config cfg;
    char cmdline[CMDLINE_SIZE];
    char vsock_cfg[CMDLINE_SIZE];

    /* Step 1: Mount pseudo-filesystems */
    mount_pseudofs();

    /* Step 1b: Load bundled guest modules (no-op on synthetic kernel) */
    load_guest_modules();

    /* Step 2: Baseline config from kernel cmdline */
    memset(&cfg, 0, sizeof(cfg));
    if (read_cmdline(cmdline, sizeof(cmdline)) < 0) {
        fprintf(stderr, "podvm-init: cannot read /proc/cmdline\n");
        return 1;
    }
    parse_config(&cfg, cmdline);

    /* Step 2b: Host vsock config overrides cmdline (best-effort, bounded). */
    if (recv_config_vsock(vsock_cfg, sizeof(vsock_cfg), VSOCK_TIMEOUT_MS) > 0) {
        parse_config(&cfg, vsock_cfg);
        fprintf(stderr, "podvm-init: received config from host over vsock\n");
    }

    if (cfg.root[0] == '\0') {
        fprintf(stderr, "podvm-init: podvm.root not set\n");
        return 1;
    }
    if (cfg.entrypoint[0] == '\0') {
        fprintf(stderr, "podvm-init: podvm.entrypoint not set\n");
        return 1;
    }

    /* Step 3: Set hostname */
    if (cfg.hostname[0])
        sethostname(cfg.hostname, strlen(cfg.hostname));

    /* Step 3b: Bring up eth0 for slirp networking (best-effort). */
    setup_network();

    /* Step 4: Mount container rootfs */
    if (mount_rootfs(&cfg) < 0)
        return 1;

    /* Step 5: Chroot into container */
    if (chroot(cfg.root) < 0) {
        fprintf(stderr, "podvm-init: chroot failed: %s\n", strerror(errno));
        return 1;
    }

    /* Step 6: Re-mount pseudofs inside the container rootfs. chroot hid
     * the initramfs mounts; /proc /sys /dev must exist in the new root. */
    mount_pseudofs_at("");

    /* Step 6b: Point DNS at slirp's resolver (best-effort; the container
     * rootfs may be read-only or lack /etc). */
    {
        int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)!write(fd, "nameserver " SLIRP_DNS "\n", 22);
            close(fd);
        }
    }

    /* Step 7: Set working directory */
    if (cfg.cwd[0])
        chdir(cfg.cwd);
    else
        chdir("/");

    /* Step 8: Apply environment. The image's env (podvm.env) is merged
     * over the inherited minimal environment, so PATH and friends from the
     * image reach the entrypoint. Best-effort per entry. */
    for (int j = 0; j < cfg.nenv; j++) {
        char *eq = strchr(cfg.env[j], '=');
        if (!eq) continue;
        *eq = '\0';
        setenv(cfg.env[j], eq + 1, 1);
        *eq = '=';   /* keep the string intact for any later use */
    }

    /* Step 9: Switch user */
    if (cfg.gid > 0) setgid(cfg.gid);
    if (cfg.uid > 0) setuid(cfg.uid);

    /* Step 10: Exec entrypoint */
    char *exec_argv[MAX_ARGS + 1];
    exec_argv[0] = cfg.entrypoint;
    for (int j = 0; j < cfg.argc; j++)
        exec_argv[1 + j] = cfg.args[j];
    exec_argv[1 + cfg.argc] = NULL;

    fprintf(stderr, "podvm-init: exec %s\n", cfg.entrypoint);
    execvp(cfg.entrypoint, exec_argv);

    fprintf(stderr, "podvm-init: exec failed: %s\n", strerror(errno));
    return 1;
}
