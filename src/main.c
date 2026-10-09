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
#include "strparse.h"
#include "imgcfg.h"
#include "container.h"

#define MAX_PATH    1024
#define MAX_CMDLINE 4096

/* Published ports: -p [hostip:]hostport:containerport (tcp). The
 * destination is always the guest's slirp address (10.0.2.15). */
#define MAX_PUBLISH 16

/* Configuration */
static struct {
    char kernel[MAX_PATH];
    char initramfs[MAX_PATH];
    char rootfs_dir[MAX_PATH];
    char entrypoint[MAX_PATH];
    char args[256];
    char env[1024];
    char cwd[MAX_PATH];       /* from image WorkingDir, or user override */
    char user[128];           /* from image User ("uid:gid") */
    char name[256];           /* container name; enables persistent state */
    int  net_none;      /* --network=none: no netdev, guest skips eth0 */
    int  read_only;     /* --read-only: serve the lower ro, no upper */
    int  memory;       /* MB */
    int  cpus;
    int  interactive;  /* -i flag */
    int  autoremove;   /* --rm: drop the named state dir when the run ends */
} config = {
    .kernel = "",  /* Empty = use host kernel */
    .initramfs = "/var/lib/podvm/kernel/initramfs.cpio",
    .memory = 128,
    .cpus = 1,
    .interactive = 0,
};

/* Create a directory and all missing parents (like mkdir -p). */
static int mkdir_p(const char *path, mode_t mode) {
    char tmp[MAX_PATH];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

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
/* Guest address under slirp (must match the guest's static config) */
#define GUEST_IP_STR "10.0.2.15"

/* vsock availability on the host (auto-detected) */
static int vsock_available = 0;

/* Podman-storage mode: rootfs is an overlay over podman layer diffs,
 * mounted inside the QEMU child's user namespace (see run_qemu_child). */
#define MAX_CHAIN 64
static StoreLayer store_layers[MAX_CHAIN];
static int store_nlayers = 0;

/* -p/--publish entries (tcp only; validated in publish_add). */
static struct {
    char hostip[64];
    int  hostport;
    int  containerport;
} publishes[MAX_PUBLISH];
static int npublishes = 0;

/*
 * Parse and store one -p spec: [hostip:]hostport:containerport.
 * Numeric ports 1..65535; hostip defaults to 127.0.0.1 (docker's
 * default bind). IPv6 hostips are rejected -- the colon-splitting is
 * unambiguous only without them.
 */
static int publish_add(const char *spec) {
    char buf[128];
    char *cport, *hport;
    const char *ip = "127.0.0.1";

    if (npublishes >= MAX_PUBLISH) {
        fprintf(stderr, "podvm: too many -p entries (max %d)\n", MAX_PUBLISH);
        return -1;
    }
    snprintf(buf, sizeof(buf), "%s", spec);

    cport = strrchr(buf, ':');
    if (!cport) goto bad;
    *cport++ = '\0';
    hport = strrchr(buf, ':');
    if (hport) {
        *hport++ = '\0';
        ip = buf;
    } else {
        hport = buf;
    }

    {
        char *end;
        long hp = strtol(hport, &end, 10);
        if (*end || hp < 1 || hp > 65535) goto bad;
        long cp = strtol(cport, &end, 10);
        if (*end || cp < 1 || cp > 65535) goto bad;
        if (strlen(ip) >= sizeof(publishes[0].hostip)) goto bad;

        snprintf(publishes[npublishes].hostip,
                 sizeof(publishes[0].hostip), "%s", ip);
        publishes[npublishes].hostport = (int)hp;
        publishes[npublishes].containerport = (int)cp;
        npublishes++;
    }
    return 0;

bad:
    fprintf(stderr, "podvm: invalid -p '%s' "
                    "(want [hostip:]hostport:containerport, tcp)\n", spec);
    return -1;
}

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

/* Escape one value for the wire. Falls back to an empty string if it
 * somehow cannot fit (kv_escape reports that by returning (size_t)-1). */
static void escape_value(char *dst, size_t dstsz, const char *src) {
    if (kv_escape(dst, dstsz, src) == (size_t)-1)
        dst[0] = '\0';
}

/*
 * Escape a comma-joined list element-wise. The separators are real
 * commas; each element is escaped so any comma or space *inside* an
 * element survives. Escaping the whole string at once would also escape
 * the separators and collapse the list into one field on the guest.
 */
static void escape_csv(char *dst, size_t dstsz, const char *raw) {
    size_t off = 0;
    dst[0] = '\0';
    if (!raw || !raw[0]) return;

    const char *p = raw;
    while (*p) {
        char elem[512];
        size_t n = 0;
        while (*p && *p != ',' && n + 1 < sizeof(elem)) elem[n++] = *p++;
        elem[n] = '\0';
        if (*p == ',') p++;

        char esc[sizeof(elem) * 2];
        escape_value(esc, sizeof(esc), elem);
        int w = snprintf(dst + off, dstsz - off, "%s%s", off ? "," : "", esc);
        if (w < 0 || (size_t)w >= dstsz - off) break;
        off += (size_t)w;
    }
}

/*
 * Append one KEY=VALUE to the CLI env list. config.env is the raw
 * comma-joined list; build_config_tokens() escapes each element on the
 * way out, so values may contain commas, spaces, and '='.
 * Returns -1 (with a message) on malformed input or overflow.
 */
static int env_add(const char *kv) {
    if (!strchr(kv, '=')) {
        fprintf(stderr, "podvm: env entry must be KEY=VALUE: '%s'\n", kv);
        return -1;
    }
    size_t have = strlen(config.env);
    int w = snprintf(config.env + have, sizeof(config.env) - have,
                     "%s%s", have ? "," : "", kv);
    if (w < 0 || (size_t)w >= sizeof(config.env) - have) {
        fprintf(stderr, "podvm: too many/large -e values (env list is %zu bytes)\n",
                sizeof(config.env));
        return -1;
    }
    return 0;
}

/* --env-file: KEY=VALUE per line; blank lines and #-comments skipped.
 * Each line is one env element, so a value may contain anything but a
 * newline (the encoder escapes the rest). */
static int env_add_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "podvm: cannot open env file %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        if (env_add(line) < 0) {
            fprintf(stderr, "podvm: %s:%d: bad env entry\n", path, lineno);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/*
 * Build the podvm.*= token string, escaping every value so spaces and
 * commas survive the channel. Shared by the kernel cmdline and the vsock
 * payload so the two can never diverge on encoding.
 */
static void build_config_tokens(char *buf, size_t len) {
    char entry[MAX_PATH * 2], args[MAX_CMDLINE], env[MAX_CMDLINE];
    char cwd[MAX_PATH * 2], user[sizeof(config.user) * 2];

    escape_value(entry, sizeof(entry), config.entrypoint);
    escape_csv(args, sizeof(args), config.args[0] ? config.args : "-");

    /* HOME is ensured for images by imgcfg_fold(); do the same here so
     * the bare-rootfs path (and -e with no image) also always has one,
     * as docker guarantees. */
    char envraw[MAX_CMDLINE];
    if (config.env[0])
        snprintf(envraw, sizeof(envraw), "%s%s", config.env,
                 strstr(config.env, "HOME=") ? "" : ",HOME=/root");
    else
        snprintf(envraw, sizeof(envraw), "HOME=/root");
    escape_csv(env, sizeof(env), envraw);
    escape_value(cwd, sizeof(cwd), config.cwd[0] ? config.cwd : "/");
    escape_value(user, sizeof(user), config.user[0] ? config.user : "-");

    snprintf(buf, len,
        "podvm.root=/rootfs "
        "podvm.entrypoint=%s "
        "podvm.args=%s "
        "podvm.env=%s "
        "podvm.cwd=%s "
        "podvm.user=%s",
        entry, args, env, cwd, user);

    /* Only sent when non-default, so ordinary cmdlines (and the e2e
     * assertions on them) are unchanged. */
    if (config.net_none) {
        size_t used = strlen(buf);
        snprintf(buf + used, len - used, " podvm.net=none");
    }
}

/* Build kernel cmdline */
static void build_cmdline(char *buf, size_t len) {
    char tokens[MAX_CMDLINE];
    build_config_tokens(tokens, sizeof(tokens));
    snprintf(buf, len, "console=ttyS0 panic=-1 %s", tokens);
}

/*
 * Fold an image's config (Env/Entrypoint/Cmd/WorkingDir/User) into the
 * run config. The resolution logic is imgcfg_fold() in imgcfg.c (pure,
 * table-tested); this is just the glue between the global config and that
 * function. An image that carries a default command can now run with no
 * command typed on the CLI (ADR-005).
 */
static void apply_image_config(const ImageConfig *ic) {
    /* The user's CLI command + args are the base; env/cwd/user start
     * unset so the image can supply them. */
    RunSpec base;
    RunSpec out;
    memset(&base, 0, sizeof(base));
    snprintf(base.entrypoint, sizeof(base.entrypoint), "%s", config.entrypoint);
    snprintf(base.args, sizeof(base.args), "%s", config.args);
    snprintf(base.env, sizeof(base.env), "%s", config.env);
    snprintf(base.cwd, sizeof(base.cwd), "%s", config.cwd);
    snprintf(base.user, sizeof(base.user), "%s", config.user);

    imgcfg_fold(ic, &base, &out);

    snprintf(config.entrypoint, sizeof(config.entrypoint), "%s", out.entrypoint);
    snprintf(config.args, sizeof(config.args), "%s", out.args);
    snprintf(config.env, sizeof(config.env), "%s", out.env);
    snprintf(config.cwd, sizeof(config.cwd), "%s", out.cwd);
    snprintf(config.user, sizeof(config.user), "%s", out.user);
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

    /* Writable layer: upper + work. With --name (and without
     * --read-only) these live in the container's persistent state dir
     * on the host, so writes survive the VM (write-back, ADR-026).
     * Everything else -- unnamed runs, and --read-only -- uses a
     * private tmpfs upper that dies with the VM. --read-only still
     * needs an upper because overlayfs in a userns requires one
     * (lower-only mounts are init-userns-only, EINVAL); the mount
     * below is made MS_RDONLY, so no write can reach it and nothing
     * persists. Either way 'merged' is the 9p-served rootfs.
     *
     * The overlay is mounted inside the QEMU child's userns (uid 0
     * mapped to the invoking uid), so files created in the persistent
     * upper are owned by the invoking user outside -- no root-owned
     * state. */
    char basedir[STORE_PATH_MAX], updir[STORE_PATH_MAX], wkdir[STORE_PATH_MAX];
    char state[CONTAINER_PATH_MAX];
    /* No state dir (no --name, or no HOME to keep it under) means the
     * throwaway tmpfs upper below. Path building lives in container.c. */
    if (container_state_dir(config.name, state, sizeof(state)) < 0)
        state[0] = '\0';

    if (state[0] && !config.read_only) {
        snprintf(updir, sizeof(updir), "%s/u", state);
        snprintf(wkdir, sizeof(wkdir), "%s/w", state);
        if (mkdir_p(state, 0755) < 0 ||
            (mkdir(updir, 0755) < 0 && errno != EEXIST) ||
            (mkdir(wkdir, 0755) < 0 && errno != EEXIST)) {
            fprintf(stderr, "podvm: cannot create container state %s: %s\n",
                    state, strerror(errno));
            return -1;
        }
        /* stderr, not stdout: this runs in the QEMU child, which
         * execvp's immediately -- buffered stdout would be discarded. */
        fprintf(stderr, "podvm: container '%s' writable layer: %s\n",
                config.name, state);
    } else {
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
    }
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

    if (mount("overlay", config.rootfs_dir, "overlay",
              config.read_only ? MS_RDONLY : 0, opts) < 0) {
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
    char netdev_str[512];

    /* Bounded argv so we can conditionally append the vsock device. */
    const char *qargs[64];
    int qi = 0;

    build_cmdline(cmdline, sizeof(cmdline));
    snprintf(memory_str, sizeof(memory_str), "%dM", config.memory);
    snprintf(cpu_str, sizeof(cpu_str), "%d", config.cpus);
    snprintf(fsdev_str, sizeof(fsdev_str),
             "local,id=fsdev0,path=%s,security_model=none%s",
             config.rootfs_dir, config.read_only ? ",readonly=on" : "");
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
    printf("podvm:   net:       %s\n", config.net_none ? "none" : "slirp (user-mode)");
    for (int i = 0; i < npublishes; i++)
        printf("podvm:   publish:   tcp:%s:%d -> " GUEST_IP_STR ":%d\n",
               publishes[i].hostip, publishes[i].hostport,
               publishes[i].containerport);
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
     * performance phase. -p appends hostfwd rules, one per entry.
     * --network=none skips the netdev entirely; the guest is told via
     * podvm.net=none and does not try to configure eth0. */
    if (!config.net_none) {
        snprintf(netdev_str, sizeof(netdev_str), "user,id=net0");
        size_t noff = strlen(netdev_str);
        for (int i = 0; i < npublishes; i++) {
            int w = snprintf(netdev_str + noff, sizeof(netdev_str) - noff,
                             ",hostfwd=tcp:%s:%d-" GUEST_IP_STR ":%d",
                             publishes[i].hostip, publishes[i].hostport,
                             publishes[i].containerport);
            if (w < 0 || (size_t)w >= sizeof(netdev_str) - noff) {
                fprintf(stderr, "podvm: -p list too long\n");
                return -1;
            }
            noff += (size_t)w;
        }
        qargs[qi++] = "-netdev";  qargs[qi++] = netdev_str;
        qargs[qi++] = "-device";  qargs[qi++] = "virtio-net-pci,netdev=net0";
    }
    /* vsock (only when the host supports it) */
    if (vsock_available) {
        qargs[qi++] = "-device";
        qargs[qi++] = "vhost-vsock-pci,id=vsock0,guest-cid=" GUEST_CID_STR;
    }
    /* Kernel + initrd + cmdline */
    qargs[qi++] = "-kernel";  qargs[qi++] = config.kernel;
    qargs[qi++] = "-initrd";  qargs[qi++] = config.initramfs;
    qargs[qi++] = "-append";  qargs[qi++] = cmdline;
    /* Serial / lifecycle.
     *
     * The guest console is always attached to stdio; -i chooses whether
     * the QEMU monitor shares it:
     *
     *   default:  -serial mon:stdio multiplexes the monitor onto stdio,
     *             so Ctrl-A C drops into the QEMU monitor -- useful for
     *             debugging a wedged guest, but Ctrl-A never reaches it.
     *   -i:       -serial stdio gives the guest stdin exclusively, so
     *             every keystroke (incl. Ctrl-A, a readline binding) goes
     *             to the container. Right for an interactive shell; the
     *             monitor is unavailable and you stop the VM by killing
     *             the process.
     *
     * We use -display none, NOT -nographic: -nographic implicitly wires
     * the default serial to stdio, which conflicts with an explicit
     * `-serial stdio` ("cannot use stdio by multiple character devices").
     * -display none leaves serial entirely under our -serial control. */
    qargs[qi++] = "-display"; qargs[qi++] = "none";
    qargs[qi++] = "-serial";
    qargs[qi++] = config.interactive ? "stdio" : "mon:stdio";
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

/*
 * --rm: discard this named container's state when the run ends, however
 * it ended. Never changes the run's own exit status -- a cleanup failure
 * is reported but does not turn a successful run into a failure.
 */
static void maybe_autoremove(void) {
    char dir[CONTAINER_PATH_MAX];
    if (!config.autoremove) return;
    if (container_state_dir(config.name, dir, sizeof(dir)) < 0 || !dir[0])
        return;   /* unnamed run: nothing was persisted */

    if (container_rm_rf(dir) == 0) {
        printf("podvm: --rm: removed container state %s\n", dir);
    } else if (errno != ENOENT) {
        fprintf(stderr, "podvm: --rm: cannot remove %s: %s\n",
                dir, strerror(errno));
    }
}

/*
 * `podvm rm <name>...` -- delete named containers' persistent state
 * (their writable layer; ADR-026). Missing names are an error, like
 * `docker rm` on a container that does not exist -- silently succeeding
 * would hide typos. Every name is attempted, so one bulk invocation
 * reports all problems; the exit status is non-zero if any failed.
 */
static int cmd_rm(int argc, char **argv) {
    int rc = 0;

    if (argc < 3 || !strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")) {
        fprintf(stderr, "Usage: podvm rm <name> [<name>...]\n");
        fprintf(stderr, "Delete named containers' persistent state "
                        "(their writable layer).\n");
        return argc < 3 ? 1 : 0;
    }

    for (int i = 2; i < argc; i++) {
        const char *name = argv[i];
        char dir[CONTAINER_PATH_MAX];

        if (!container_name_valid(name)) {
            fprintf(stderr, "podvm: invalid container name: '%s'\n", name);
            rc = 1;
            continue;
        }
        if (container_state_dir(name, dir, sizeof(dir)) < 0 || !dir[0]) {
            fprintf(stderr, "podvm: cannot locate the state dir "
                            "(is HOME or XDG_STATE_HOME set?)\n");
            return 1;
        }
        /* Defensive: only ever delete below .../podvm/containers/, even
         * if the state-root logic above changes someday. */
        if (!strstr(dir, "/podvm/containers/")) {
            fprintf(stderr, "podvm: refusing to delete '%s' "
                            "(not a podvm state dir)\n", dir);
            rc = 1;
            continue;
        }
        if (container_rm_rf(dir) < 0) {
            if (errno == ENOENT) {
                fprintf(stderr, "podvm: no such container: %s\n", name);
            } else {
                fprintf(stderr, "podvm: cannot remove %s: %s\n",
                        dir, strerror(errno));
            }
            rc = 1;
            continue;
        }
        printf("podvm: removed container '%s' (%s)\n", name, dir);
    }
    return rc;
}

/* Long-only options: --rm has no short form (-r is taken by --initramfs). */
enum { OPT_RM = 1000, OPT_ENVFILE = 1001, OPT_NETWORK = 1002,
       OPT_READONLY = 1003 };

/* Print usage */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <image|rootfs-dir> [command] [args...]\n", prog);
    fprintf(stderr, "       %s rm <name> [<name>...]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -i, --interactive   Give the guest stdin exclusively\n");
    fprintf(stderr, "                      (default: QEMU monitor shares it; Ctrl-A C)\n");
    fprintf(stderr, "  -e, --env <K=V>     Set an environment variable (repeatable;\n");
    fprintf(stderr, "                      overrides the image's value for the key)\n");
    fprintf(stderr, "      --env-file <f>  Read KEY=VALUE lines from a file\n");
    fprintf(stderr, "  -u, --user <spec>   Run as <name|uid>[:<group|gid>] (resolved\n");
    fprintf(stderr, "                      against the container's /etc/passwd)\n");
    fprintf(stderr, "  -w, --workdir <dir> Working directory inside the container\n");
    fprintf(stderr, "                      (absolute; created if missing)\n");
    fprintf(stderr, "  -p, --publish <spec> Publish a container port to the host:\n");
    fprintf(stderr, "                      [hostip:]hostport:containerport (tcp)\n");
    fprintf(stderr, "      --network <mode>  slirp (default) or none (no network)\n");
    fprintf(stderr, "      --read-only     Mount the container rootfs read-only\n");
    fprintf(stderr, "  -m, --memory <MB>   Memory in MB (default: 128)\n");
    fprintf(stderr, "  -c, --cpus <cpus>   CPUs (default: 1)\n");
    fprintf(stderr, "  -k, --kernel <path> Kernel path\n");
    fprintf(stderr, "  -r, --initramfs <path> Initramfs path\n");
    fprintf(stderr, "  -n, --name <name>   Container name; persists the writable layer\n");
    fprintf(stderr, "                      across runs (write-back). Without it, writes\n");
    fprintf(stderr, "                      are discarded when the VM exits.\n");
    fprintf(stderr, "      --rm            Remove the named container's state when the\n");
    fprintf(stderr, "                      run exits (no-op without --name)\n");
    fprintf(stderr, "  -h, --help          Show this help\n");
    fprintf(stderr, "\nSubcommands:\n");
    fprintf(stderr, "  rm <name>...        Delete named containers' persistent state\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -i debian /bin/sh\n", prog);
    fprintf(stderr, "  %s -i debian echo hello\n", prog);
    fprintf(stderr, "  %s -i /path/to/rootfs /bin/sh\n", prog);
    fprintf(stderr, "  %s -n build -i alpine /bin/sh   (writes survive the run)\n", prog);
    fprintf(stderr, "  %s rm build                     (discard them)\n", prog);
}

int main(int argc, char **argv) {
    int opt;
    int option_index = 0;

    /* Subcommands are dispatched before getopt, so a subcommand name is
     * never consumed as the image positional argument. */
    if (argc > 1 && strcmp(argv[1], "rm") == 0)
        return cmd_rm(argc, argv);
    const char *image_ref = NULL;   /* user's original ref, kept for config */
    
    static struct option long_options[] = {
        {"interactive", no_argument,       0, 'i'},
        {"memory",      required_argument, 0, 'm'},
        {"cpus",        required_argument, 0, 'c'},
        {"kernel",      required_argument, 0, 'k'},
        {"initramfs",   required_argument, 0, 'r'},
        {"name",        required_argument, 0, 'n'},
        {"env",         required_argument, 0, 'e'},
        {"env-file",    required_argument, 0, OPT_ENVFILE},
        {"user",        required_argument, 0, 'u'},
        {"workdir",     required_argument, 0, 'w'},
        {"publish",     required_argument, 0, 'p'},
        {"network",     required_argument, 0, OPT_NETWORK},
        {"read-only",   no_argument,       0, OPT_READONLY},
        {"rm",          no_argument,       0, OPT_RM},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    /* '+' stops getopt at the first non-option (the image/rootfs), so
     * flags meant for the entrypoint (e.g. `podvm img /bin/sh -c ...`)
     * are not stolen as podvm options. */
    while ((opt = getopt_long(argc, argv, "+:ie:u:w:p:m:c:k:r:n:h", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'i':
            config.interactive = 1;
            break;
        case 'e':
            if (env_add(optarg) < 0) return 1;
            break;
        case OPT_ENVFILE:
            if (env_add_file(optarg) < 0) return 1;
            break;
        case 'u':
            if (!optarg[0]) {
                fprintf(stderr, "podvm: --user must not be empty\n");
                return 1;
            }
            snprintf(config.user, sizeof(config.user), "%s", optarg);
            break;
        case 'w':
            if (optarg[0] != '/') {
                fprintf(stderr, "podvm: --workdir must be an absolute path: '%s'\n",
                        optarg);
                return 1;
            }
            snprintf(config.cwd, sizeof(config.cwd), "%s", optarg);
            break;
        case 'p':
            if (publish_add(optarg) < 0) return 1;
            break;
        case OPT_NETWORK:
            if (!strcmp(optarg, "none")) {
                config.net_none = 1;
            } else if (!strcmp(optarg, "slirp")) {
                config.net_none = 0;
            } else {
                fprintf(stderr, "podvm: --network must be slirp or none "
                                "(host/bridge need tap; ADR-009/perf phase)\n");
                return 1;
            }
            break;
        case OPT_READONLY:
            config.read_only = 1;
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
        case 'n':
            if (!container_name_valid(optarg)) {
                fprintf(stderr, "podvm: invalid container name '%s' "
                                "(no '/', not '.' or '..', no leading '-')\n",
                        optarg);
                return 1;
            }
            snprintf(config.name, sizeof(config.name), "%s", optarg);
            break;
        case OPT_RM:
            config.autoremove = 1;
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

    if (config.net_none && npublishes > 0) {
        fprintf(stderr, "podvm: -p/--publish requires networking "
                        "(--network=none disables it)\n");
        return 1;
    }

    /* Get image or rootfs directory */
    strncpy(config.rootfs_dir, argv[optind], MAX_PATH - 1);
    config.rootfs_dir[MAX_PATH - 1] = '\0';
    {
        /* Keep the user's original ref for image-config lookup (the
         * rootfs_dir field is overwritten with the resolved path below). */
        static char image_ref_buf[MAX_PATH];
        snprintf(image_ref_buf, sizeof(image_ref_buf), "%s", config.rootfs_dir);
        image_ref = image_ref_buf;
    }
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
    int used_podman = 0;

    /*
     * Resolution order:
     *   1. directory path -> use as-is
     *   2. podman storage -> layers in place (pull via podman if missing)
     *   3. podvm's own registry pull -> cache
     *
     * PODVM_RESOLVE overrides the storage choice for an image ref:
     *   podvm   -> podman storage only (fail if absent)
     *   native  -> podvm's own registry pull only
     *   auto    -> the default order above
     * Mostly a testing knob (the native pull path is otherwise only
     * reached when podman storage misses), but also lets a user prefer
     * podvm's own pull over podman's storage.
     */
    const char *resolve_pref = getenv("PODVM_RESOLVE");
    int want_native = resolve_pref && !strcmp(resolve_pref, "native");
    int want_podman = resolve_pref && !strcmp(resolve_pref, "podman");
    if (resolve_pref && !want_native && !want_podman) {
        fprintf(stderr, "podvm: PODVM_RESOLVE must be 'auto', 'podman' or 'native'\n");
        return 1;
    }
    if (image_is_path(config.rootfs_dir)) {
        if (image_resolve(config.rootfs_dir, resolved_rootfs,
                          sizeof(resolved_rootfs)) < 0) {
            fprintf(stderr, "podvm: directory not found: %s\n", config.rootfs_dir);
            return 1;
        }
    } else {
        char top[STORE_PATH_MAX];
        int resolved = 0;

        /* 2. podman storage (skipped when the user forced native). */
        if (!want_native &&
            store_resolve(config.rootfs_dir, top, sizeof(top), 1) == 0) {
            store_nlayers = store_chain(top, store_layers, MAX_CHAIN);
            if (store_nlayers <= 0) {
                fprintf(stderr, "podvm: no layer chain for %s\n", config.rootfs_dir);
                return 1;
            }
            printf("podvm: using podman storage: %s (%d layers)\n",
                   config.rootfs_dir, store_nlayers);
            used_podman = 1;
            resolved = 1;
            /* Served dir is created by the QEMU child's overlay mount */
            snprintf(resolved_rootfs, sizeof(resolved_rootfs),
                     "/tmp/podvm-rootfs.%d", getpid());
        } else if (want_podman) {
            fprintf(stderr, "podvm: image '%s' not in podman storage "
                            "(PODVM_RESOLVE=podman)\n", config.rootfs_dir);
            return 1;
        }

        /* 3. podvm's own registry pull (skipped when the user forced
         * podman, which already failed above). */
        if (!resolved && !want_podman &&
            image_resolve(config.rootfs_dir, resolved_rootfs,
                          sizeof(resolved_rootfs)) < 0) {
            fprintf(stderr, "podvm: failed to resolve image/path: %s\n",
                    config.rootfs_dir);
            return 1;
        }
    }
    
    /* Copy resolved path back */
    strncpy(config.rootfs_dir, resolved_rootfs, MAX_PATH - 1);
    config.rootfs_dir[MAX_PATH - 1] = '\0';

    /*
     * Fold the image's own config (Env/Entrypoint/Cmd/WorkingDir/User)
     * into the run config. An image that carries a default command can
     * now run with no command typed on the CLI (ADR-005). Only applies
     * when we resolved an image -- a bare rootfs dir has no config.
     */
    if (!image_is_path(image_ref)) {
        ImageConfig ic;
        image_config_init(&ic);
        int got = used_podman
                ? store_image_config(image_ref, &ic)
                : image_config_load(image_ref, &ic);
        if (got == 0)
            apply_image_config(&ic);
        image_config_free(&ic);
    }

    /* The entrypoint is the one thing the guest cannot invent: if neither
     * the user nor the image supplied a command, there is nothing to exec. */
    if (!config.entrypoint[0]) {
        fprintf(stderr, "podvm: no command given and image '%s' declares none\n",
                image_ref);
        fprintf(stderr, "podvm: usage: podvm [options] <image|rootfs> [command]\n");
        return 1;
    }

    /* Setup signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Detect host vsock support (determines QEMU device + config channel). */
    detect_vsock();

    /* Start QEMU (9p serves the rootfs in-process; no virtiofsd) */
    if (start_qemu() < 0) {
        fprintf(stderr, "podvm: failed to start QEMU\n");
        maybe_autoremove();   /* --rm still drops the name's state */
        return 1;
    }

    /* Send config to guest over vsock (best-effort; cmdline is the
     * authoritative fallback and is always passed via -append above). */
    if (vsock_available) {
        char vsock_config[MAX_CMDLINE];
        build_config_tokens(vsock_config, sizeof(vsock_config));

        printf("podvm: sending config to guest over vsock\n");
        if (send_config_vsock(vsock_config) < 0) {
            fprintf(stderr, "podvm: vsock config failed; guest will use cmdline\n");
        }
    }

    /* Wait for QEMU to exit */
    int rc = wait_qemu();

    /* --rm: this named container's state dies with the run. */
    maybe_autoremove();
    return rc;
}
