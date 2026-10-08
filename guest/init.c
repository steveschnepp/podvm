/*
 * podvm guest init
 *
 * Minimal init for podvm guests (runs as PID 1).
 *
 * Config comes from two sources, in priority order:
 *   1. kernel cmdline  (podvm.*=... tokens, always present via -append)
 *   2. host vsock      (overrides cmdline when the host connects and sends)
 *
 * Flow: mount pseudofs -> parse cmdline -> try vsock override -> mount
 * rootfs (virtiofs/9p) -> chroot -> set cwd/user -> exec entrypoint.
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
#include <sys/wait.h>
#include <linux/vm_sockets.h>

#include "strparse.h"

#define MAX_ARGS      64
#define PATH_SIZE     1024
#define CMDLINE_SIZE  4096

/* vsock config channel (must match host) */
#define VSOCK_PORT       5000
#define VSOCK_TIMEOUT_MS 3000

/* Configuration from kernel cmdline / host vsock */
typedef struct {
    char root[PATH_SIZE];
    char entrypoint[PATH_SIZE];
    char *args[MAX_ARGS];
    int  argc;
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

    val = kv_find(text, "podvm.root=");
    if (val) kv_copy_value(cfg->root, PATH_SIZE, val);

    val = kv_find(text, "podvm.entrypoint=");
    if (val) kv_copy_value(cfg->entrypoint, PATH_SIZE, val);

    val = kv_find(text, "podvm.args=");
    if (val) {
        csv_free(cfg->args, cfg->argc);   /* free any previous args */
        cfg->argc = csv_parse(val, cfg->args, MAX_ARGS);
    }

    val = kv_find(text, "podvm.cwd=");
    if (val) kv_copy_value(cfg->cwd, PATH_SIZE, val);

    val = kv_find(text, "podvm.hostname=");
    if (val) kv_copy_value(cfg->hostname, 256, val);

    val = kv_find(text, "podvm.user=");
    if (val) parse_uid_gid(val, &cfg->uid, &cfg->gid);
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
static void mount_pseudofs(void) {
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
}

/* Mount container rootfs via virtiofs (fallback: 9p) */
static int mount_rootfs(const Config *cfg) {
    mkdir_p(cfg->root, 0755);

    /* virtiofs: tag is the source argument, not a mount option */
    if (mount("rootfs", cfg->root, "virtiofs", 0, NULL) == 0)
        return 0;

    /* 9p: tag is a mount option */
    if (mount(NULL, cfg->root, "9p", 0,
              "trans=virtio,version=9p2000.L,tag=rootfs") == 0)
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

    /* Step 4: Mount container rootfs */
    if (mount_rootfs(&cfg) < 0)
        return 1;

    /* Step 5: Chroot into container */
    if (chroot(cfg.root) < 0) {
        fprintf(stderr, "podvm-init: chroot failed: %s\n", strerror(errno));
        return 1;
    }

    /* Step 6: Set working directory */
    if (cfg.cwd[0])
        chdir(cfg.cwd);
    else
        chdir("/");

    /* Step 7: Switch user */
    if (cfg.gid > 0) setgid(cfg.gid);
    if (cfg.uid > 0) setuid(cfg.uid);

    /* Step 8: Exec entrypoint */
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
