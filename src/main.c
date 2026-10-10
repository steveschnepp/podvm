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
#include <poll.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>

#include "image.h"
#include "store.h"
#include "store_native.h"
#include "strparse.h"
#include "imgcfg.h"
#include "container.h"
#include "build.h"
#include "vsockalloc.h"

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
    char args[4096];       /* csv of escaped argv elements (guest tok[] matches) */
    char env[4096];         /* escaped csv of K=V (guest tok[] matches) */
    char cwd[MAX_PATH];       /* from image WorkingDir, or user override */
    char user[128];           /* from image User ("uid:gid") */
    char name[256];           /* container name; enables persistent state */
    char hostname[256];       /* --hostname; sent as podvm.hostname= */
    int  net_none;      /* --network=none: no netdev, guest skips eth0 */
    int  read_only;     /* --read-only: serve the lower ro, no upper */
    int  detach;        /* -d: background the VM; serial to <state>/log */
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
/* Guest address under slirp (must match the guest's static config) */
#define GUEST_IP_STR "10.0.2.15"

/* vsock availability on the host (auto-detected) */
static int vsock_available = 0;
/* Guest CID and exit-report port, allocated per VM by vsockalloc.c so
 * concurrent runs do not collide (the kernel rejects a duplicate
 * guest-cid, and two listeners cannot share a host vsock port). -1 until
 * allocated; vsock_exit_port is set lazily by start_exit_helper, which
 * only runs when a helper is actually started (i.e. not --detach). */
static int vsock_cid = -1;
static int vsock_exit_port = -1;

/*
 * Exit-code reporting. The guest runs the entrypoint as a child and,
 * when it finishes, connects to the HOST (cid 2, a per-run port) and
 * writes
 * the exit code. QEMU's own exit status is meaningless for this (a
 * guest panic carries no command status), so without this channel
 * 'podvm run cmd' could never tell a script that cmd failed -- and
 * podvm build could not detect a failed RUN.
 *
 * The listener runs in a helper process so the QEMU wait is never
 * blocked; the code comes back over a pipe. One-shot per run/step. The
 * port is allocated by vsockalloc.c (see vsock_exit_port) so concurrent
 * runs' helpers do not collide on the host-global vsock port namespace.
 */
static pid_t exit_helper_pid = -1;
static int exit_pipe[2] = { -1, -1 };

static void start_exit_helper(void) {
    if (!vsock_available || exit_helper_pid > 0) return;
    /* Claim a unique exit port. Allocated once and reused for every
     * step of a build. */
    if (vsock_exit_port <= 0) {
        vsock_exit_port = vsock_alloc_port();
        if (vsock_exit_port <= 0) {
            fprintf(stderr, "podvm: no free vsock exit port (%s); "
                            "this run's exit code will not be reported\n",
                    vsock_lockdir());
            return;
        }
    }
    if (pipe(exit_pipe) < 0) return;

    exit_helper_pid = fork();
    if (exit_helper_pid == 0) {
        close(exit_pipe[0]);
        int lfd = socket(AF_VSOCK, SOCK_STREAM, 0);
        if (lfd < 0) _exit(1);
        struct sockaddr_vm addr;
        memset(&addr, 0, sizeof(addr));
        addr.svm_family = AF_VSOCK;
        addr.svm_cid = VMADDR_CID_ANY;
        addr.svm_port = vsock_exit_port;
        if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(lfd, 1) < 0) {
            close(lfd);
            _exit(1);
        }
        struct pollfd pfd = { lfd, POLLIN, 0 };
        if (poll(&pfd, 1, 30000) <= 0) _exit(1);
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) _exit(1);
        char buf[32];
        ssize_t n = read(cfd, buf, sizeof(buf) - 1);
        close(cfd);
        close(lfd);
        if (n <= 0) _exit(1);
        buf[n] = '\0';
        (void)!dprintf(exit_pipe[1], "%d\n", atoi(buf));
        _exit(0);
    }
    close(exit_pipe[1]);
    exit_pipe[1] = -1;
}

/* After wait_qemu(): collect the reported code. The guest reports
 * before it exits, so by now the helper has either written or never
 * will (old initramfs / no vsock) -- a short grace poll, then kill. */
static int reap_exit_helper(void) {
    if (exit_helper_pid <= 0) return -1;

    int code = -1;
    struct pollfd pfd = { exit_pipe[0], POLLIN, 0 };
    if (poll(&pfd, 1, 200) > 0) {
        char buf[32];
        ssize_t n = read(exit_pipe[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            code = atoi(buf);
        }
    }
    kill(exit_helper_pid, SIGKILL);   /* done, or stale: either way reap */
    waitpid(exit_helper_pid, NULL, 0);
    close(exit_pipe[0]);
    exit_pipe[0] = -1;
    exit_helper_pid = -1;
    return code;
}

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

/* -v/--volume bind mounts, each served to the guest over its own 9p
 * tag (vol0, vol1, ...) because the rootfs tag is taken. */
#define MAX_VOLUMES 8
static struct {
    char host[CONTAINER_PATH_MAX];     /* host dir (exists at parse time) */
    char target[CONTAINER_PATH_MAX];   /* absolute path inside the container */
    int  ro;
} volumes[MAX_VOLUMES];
static int nvolumes = 0;

/* --tmpfs scratch mounts (absolute container paths). */
#define MAX_TMPFS 8
static char tmpfs_targets[MAX_TMPFS][CONTAINER_PATH_MAX];
static int ntmpfs = 0;

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

/*
 * Parse and store one -v spec: host-dir:container-dir[:ro]. Both paths
 * absolute; the host path must be an existing directory (QEMU would
 * otherwise fail late with a worse message). Colons and commas are
 * rejected in both paths: commas break QEMU's fsdev option parsing and
 * colons break the guest-side tag:target:ro split. Docker's volume
 * names (non-path first field) are not supported -- bind mounts only.
 */
static int volume_add(const char *spec) {
    char buf[CONTAINER_PATH_MAX * 2];
    int ro = 0;

    if (nvolumes >= MAX_VOLUMES) {
        fprintf(stderr, "podvm: too many -v entries (max %d)\n", MAX_VOLUMES);
        return -1;
    }
    snprintf(buf, sizeof(buf), "%s", spec);

    {
        char *last = strrchr(buf, ':');
        if (last && !strcmp(last, ":ro")) {
            *last = '\0';
            ro = 1;
        }
        char *target = strrchr(buf, ':');
        if (!target) goto bad;
        *target++ = '\0';

        if (buf[0] != '/' || target[0] != '/') goto bad;
        if (strchr(buf, ':') || strchr(buf, ',') ||
            strchr(target, ':') || strchr(target, ',')) goto bad;
        if (strlen(buf) >= sizeof(volumes[0].host) ||
            strlen(target) >= sizeof(volumes[0].target)) goto bad;

        struct stat st;
        if (stat(buf, &st) < 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "podvm: -v host path is not a directory: %s\n", buf);
            return -1;
        }

        snprintf(volumes[nvolumes].host, sizeof(volumes[0].host), "%s", buf);
        snprintf(volumes[nvolumes].target, sizeof(volumes[0].target), "%s", target);
        volumes[nvolumes].ro = ro;
        nvolumes++;
    }
    return 0;

bad:
    fprintf(stderr, "podvm: invalid -v '%s' (want absolute-host-dir:"
                    "absolute-container-dir[:ro])\n", spec);
    return -1;
}

/* --tmpfs <dir>: absolute container path for a guest tmpfs mount. */
static int tmpfs_add(const char *spec) {
    if (ntmpfs >= MAX_TMPFS) {
        fprintf(stderr, "podvm: too many --tmpfs entries (max %d)\n", MAX_TMPFS);
        return -1;
    }
    if (spec[0] != '/') {
        fprintf(stderr, "podvm: --tmpfs path must be absolute: '%s'\n", spec);
        return -1;
    }
    snprintf(tmpfs_targets[ntmpfs], sizeof(tmpfs_targets[0]), "%s", spec);
    ntmpfs++;
    return 0;
}

/* Detect host vsock support via /dev/vhost-vsock, and claim a unique
 * guest CID for this VM. Without a free CID the vhost-vsock device
 * cannot be created, so we degrade to cmdline-only config (the same
 * no-vsock path) rather than boot a VM whose config channel and exit
 * report would silently vanish. */
static void detect_vsock(void) {
    struct stat st;
    vsock_available = (stat("/dev/vhost-vsock", &st) == 0);
    if (!vsock_available) {
        fprintf(stderr, "podvm: /dev/vhost-vsock not present; "
                        "falling back to cmdline config only\n");
        return;
    }
    vsock_cid = vsock_alloc_cid();
    if (vsock_cid < 0) {
        fprintf(stderr, "podvm: no free vsock CID (%s); "
                        "falling back to cmdline config only\n",
                vsock_lockdir());
        vsock_available = 0;
    }
}

/* Send config to guest via vsock.
 * The guest listens on <vsock_cid>:VSOCK_PORT and reads a single line of
 * space-separated podvm.*=... tokens (same format as the kernel cmdline).
 * Returns 0 on success, -1 on failure (caller falls back to cmdline). */
static int send_config_vsock(const char *config_str) {
    struct sockaddr_vm addr;
    int fd = -1;
    int connected = 0;

    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = vsock_cid;   /* connect to the guest, not CID_ANY */
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
                vsock_cid, VSOCK_PORT, strerror(errno));
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
 * Encode a csv list for the wire. FIELDS MUST ARRIVE ALREADY ESCAPED
 * (kv_escape): this only splits on *unescaped* commas and rejoins with
 * raw commas, so a comma inside a field survives the trip.
 *
 * It deliberately does not escape. Escaping happens where fields are
 * built (the argv join, env_add, imgcfg, -v/--tmpfs lists) -- the only
 * places that know which commas are separators and which are data.
 * Escaping here, after a raw join, could not tell them apart: that is
 * exactly how 'podvm -e X=a,b' lost the ',b' (the guest saw two
 * fields). The guest's csv_parse already splits on unescaped commas
 * and unescapes fields, so the wire format is unchanged for values
 * without special characters.
 */
static void csv_encode(char *dst, size_t dstsz, const char *raw) {
    size_t off = 0;
    dst[0] = '\0';
    const char *p = raw;
    while (*p && off + 1 < dstsz) {
        if (*p == '\\' && p[1] && off + 2 < dstsz) {
            dst[off++] = *p++;          /* escaped pair: verbatim */
            dst[off++] = *p++;
            continue;
        }
        dst[off++] = *p++;              /* field char or separator */
    }
    dst[off] = '\0';
}

/*
 * Append one KEY=VALUE to the CLI env list. config.env is the raw
 * comma-joined list; build_config_tokens() escapes each element on the
 * way out, so values may contain commas, spaces, and '='.
 * Returns -1 (with a message) on malformed input or overflow.
 */
static int env_add(const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq) {
        fprintf(stderr, "podvm: env entry must be KEY=VALUE: '%s'\n", kv);
        return -1;
    }
    /* Escape the VALUE now: this is the last place that knows which
     * commas in the entry are data (csv_encode will not re-escape). */
    char esc[2048];
    if (kv_escape(esc, sizeof(esc), eq + 1) == (size_t)-1) {
        fprintf(stderr, "podvm: env value too long: '%s'\n", kv);
        return -1;
    }
    size_t klen = (size_t)(eq - kv);        /* key, without '=' */
    size_t have = strlen(config.env);
    int w = snprintf(config.env + have, sizeof(config.env) - have,
                     "%s%.*s=%s", have ? "," : "", (int)klen, kv, esc);
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
    {
        /* args/env csv fields are pre-escaped by their builders (argv
         * join, env_add, imgcfg); csv_encode only preserves them. */
        csv_encode(args, sizeof(args), config.args[0] ? config.args : "-");
    }

    /* HOME is ensured for images by imgcfg_fold(); do the same here so
     * the bare-rootfs path (and -e with no image) also always has one,
     * as docker guarantees. */
    char envraw[MAX_CMDLINE];
    if (config.env[0])
        snprintf(envraw, sizeof(envraw), "%s%s", config.env,
                 strstr(config.env, "HOME=") ? "" : ",HOME=/root");
    else
        snprintf(envraw, sizeof(envraw), "HOME=/root");
    csv_encode(env, sizeof(env), envraw);
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
    if (nvolumes) {
        /* tag:target:ro|- per element; escape_csv escapes each element
         * so targets may contain spaces (but not commas/colons --
         * rejected at parse time). */
        char volraw[MAX_CMDLINE], volesc[MAX_CMDLINE];
        size_t voff = 0;
        volraw[0] = '\0';
        for (int i = 0; i < nvolumes; i++) {
            char elem[2048], escelem[4096];
            snprintf(elem, sizeof(elem), "vol%d:%s:%s", i,
                     volumes[i].target, volumes[i].ro ? "ro" : "-");
            escape_value(escelem, sizeof(escelem), elem);
            int w = snprintf(volraw + voff, sizeof(volraw) - voff,
                             "%s%s", i ? "," : "", escelem);
            if (w < 0 || (size_t)w >= sizeof(volraw) - voff) break;
            voff += (size_t)w;
        }
        csv_encode(volesc, sizeof(volesc), volraw);
        size_t used = strlen(buf);
        snprintf(buf + used, len - used, " podvm.vol=%s", volesc);
    }
    if (ntmpfs) {
        char traw[MAX_CMDLINE], tesc[MAX_CMDLINE];
        size_t toff = 0;
        traw[0] = '\0';
        for (int i = 0; i < ntmpfs; i++) {
            char escelem[4096];
            escape_value(escelem, sizeof(escelem), tmpfs_targets[i]);
            int w = snprintf(traw + toff, sizeof(traw) - toff,
                             "%s%s", i ? "," : "", escelem);
            if (w < 0 || (size_t)w >= sizeof(traw) - toff) break;
            toff += (size_t)w;
        }
        csv_encode(tesc, sizeof(tesc), traw);
        size_t used = strlen(buf);
        snprintf(buf + used, len - used, " podvm.tmpfs=%s", tesc);
    }
    if (config.hostname[0]) {
        char hostesc[600];
        escape_value(hostesc, sizeof(hostesc), config.hostname);
        size_t used = strlen(buf);
        snprintf(buf + used, len - used, " podvm.hostname=%s", hostesc);
    }
    /* The exit-report port this run's helper listens on. Sent only when
     * one was allocated (not --detach), so the guest dials the port the
     * host actually bound instead of the old hardcoded 5001. */
    if (vsock_exit_port > 0) {
        size_t used = strlen(buf);
        snprintf(buf + used, len - used, " podvm.exitport=%d", vsock_exit_port);
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
    char vsock_dev[64];
    char netdev_str[512];

    /* Fail early with a diagnosis instead of a silent QEMU exit and no
     * serial output (which looks like a guest bug but is just a
     * missing file). */
    {
        struct stat st;
        if (stat(config.kernel, &st) < 0) {
            fprintf(stderr, "podvm: kernel not found: %s (use -k)\n",
                    config.kernel);
            return -1;
        }
        if (stat(config.initramfs, &st) < 0) {
            fprintf(stderr, "podvm: initramfs not found: %s "
                            "(use -r, or 'make initramfs')\n",
                    config.initramfs);
            return -1;
        }
    }

    /* Bounded argv so we can conditionally append the vsock device and
     * one fsdev+device pair per -v volume. */
    const char *qargs[128];
    int qi = 0;
    char vol_fsdev[MAX_VOLUMES][MAX_PATH + 64];
    char vol_dev[MAX_VOLUMES][64];
    char vol_tag[MAX_VOLUMES][16];

    build_cmdline(cmdline, sizeof(cmdline));
    snprintf(memory_str, sizeof(memory_str), "%dM", config.memory);
    snprintf(cpu_str, sizeof(cpu_str), "%d", config.cpus);
    snprintf(fsdev_str, sizeof(fsdev_str),
             "local,id=fsdev0,path=%s,security_model=none%s",
             config.rootfs_dir, config.read_only ? ",readonly=on" : "");
    snprintf(vsock_cid_str, sizeof(vsock_cid_str), "%d", vsock_cid);

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
    /* Diagnostics must reach a redirected log before QEMU's serial
     * output interleaves; stdout is block-buffered to a file. */
    fflush(stdout);

    /* Machine / resources */
    qargs[qi++] = "qemu-system-x86_64";
    qargs[qi++] = "-machine"; qargs[qi++] = "q35,accel=kvm";
    qargs[qi++] = "-cpu";     qargs[qi++] = "host";
    qargs[qi++] = "-m";       qargs[qi++] = memory_str;
    qargs[qi++] = "-smp";     qargs[qi++] = cpu_str;
    /* 9p filesystem share (in-process; no daemon needed) */
    qargs[qi++] = "-fsdev";   qargs[qi++] = fsdev_str;
    qargs[qi++] = "-device";  qargs[qi++] = "virtio-9p-pci,fsdev=fsdev0,mount_tag=rootfs";
    /* -v volumes: one fsdev + 9p device each, tags vol0..volN. The
     * guest mounts them at the requested targets after chroot. */
    for (int i = 0; i < nvolumes; i++) {
        snprintf(vol_tag[i], sizeof(vol_tag[i]), "vol%d", i);
        snprintf(vol_fsdev[i], sizeof(vol_fsdev[i]),
                 "local,id=%s,path=%s,security_model=none%s",
                 vol_tag[i], volumes[i].host,
                 volumes[i].ro ? ",readonly=on" : "");
        snprintf(vol_dev[i], sizeof(vol_dev[i]),
                 "virtio-9p-pci,fsdev=%s,mount_tag=%s", vol_tag[i], vol_tag[i]);
        qargs[qi++] = "-fsdev";  qargs[qi++] = vol_fsdev[i];
        qargs[qi++] = "-device"; qargs[qi++] = vol_dev[i];
    }
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
    /* vsock (only when the host supports it); the CID is this VM's
     * allocated one, so concurrent VMs never share a guest-cid. */
    if (vsock_available) {
        snprintf(vsock_dev, sizeof(vsock_dev),
                 "vhost-vsock-pci,id=vsock0,guest-cid=%d", vsock_cid);
        qargs[qi++] = "-device";
        qargs[qi++] = vsock_dev;
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
    /* Attached: the monitor may share stdio (mon:stdio) unless -i gave
     * the guest stdin exclusively. Detached: the serial goes to the
     * container's log file -- nobody owns stdio anymore. */
    char serial_backend[STORE_PATH_MAX + 32];
    if (config.detach) {
        char sdir[CONTAINER_PATH_MAX];
        if (container_state_dir(config.name, sdir, sizeof(sdir)) < 0 || !sdir[0]) {
            fprintf(stderr, "podvm: --detach needs a container state dir\n");
            return -1;
        }
        if (mkdir_p(sdir, 0755) < 0) {
            fprintf(stderr, "podvm: cannot create %s: %s\n", sdir, strerror(errno));
            return -1;
        }
        snprintf(serial_backend, sizeof(serial_backend), "file:%s/log", sdir);
    } else {
        snprintf(serial_backend, sizeof(serial_backend), "%s",
                 config.interactive ? "stdio" : "mon:stdio");
    }
    qargs[qi++] = serial_backend;
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
 * would hide typos. A live name is refused (the same flock that
 * serializes runs also serializes rm). Every name is attempted, so one
 * bulk invocation reports all problems; the exit status is non-zero if
 * any failed.
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

        int lockfd = container_lock(name);
        if (lockfd < 0) {
            if (errno == EWOULDBLOCK)
                fprintf(stderr, "podvm: container '%s' is running; "
                                "stop it before removing it\n", name);
            else
                fprintf(stderr, "podvm: cannot lock container '%s': %s\n",
                        name, strerror(errno));
            rc = 1;
            continue;
        }

        /* Hold the lock across the deletion: a run starting in the same
         * instant then fails fast instead of booting a VM whose state
         * dir is being removed underneath it. */
        int rmc = container_rm_rf(dir);
        int rme = errno;
        close(lockfd);
        if (rmc < 0) {
            if (rme == ENOENT) {
                fprintf(stderr, "podvm: no such container: %s\n", name);
            } else {
                fprintf(stderr, "podvm: cannot remove %s: %s\n",
                        dir, strerror(rme));
            }
            rc = 1;
            continue;
        }
        printf("podvm: removed container '%s' (%s)\n", name, dir);
    }
    return rc;
}

/* Long-only options: --rm has no short form (-r is taken by --initramfs;
 * docker's -h hostname clashes with help, so --hostname is long-only). */
enum { OPT_RM = 1000, OPT_ENVFILE = 1001, OPT_NETWORK = 1002,
       OPT_READONLY = 1003, OPT_TMPFS = 1004, OPT_HOSTNAME = 1005 };

/*
 * `podvm images` -- list the image cache (native pulls AND podvm build
 * output; the two share one layout, so run/images/rmi see one cache).
 */
static int cmp_imgent(const void *a, const void *b) {
    const ImageCacheEntry *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return c ? c : strcmp(x->tag, y->tag);
}

static void human_size(unsigned long long b, char *out, size_t n) {
    static const char *unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u + 1 < (int)(sizeof(unit) / sizeof(unit[0]))) {
        v /= 1024.0;
        u++;
    }
    snprintf(out, n, u ? "%.1f%s" : "%.0f%s", v, unit[u]);
}

static int cmd_images(void) {
    enum { MAX_IMGS = 512 };
    static ImageCacheEntry ents[MAX_IMGS];
    int n = image_cache_list(ents, MAX_IMGS);
    if (n < 0) {
        fprintf(stderr, "podvm: cannot list the image cache\n");
        return 1;
    }
    if (n == 0) {
        printf("no images in the podvm cache (%s)\n", image_cache_dir());
        return 0;
    }
    qsort(ents, (size_t)n, sizeof(ents[0]), cmp_imgent);

    printf("%-48s %-12s %-10s %s\n", "NAME", "TAG", "IMAGE ID", "SIZE");
    for (int i = 0; i < n; i++) {
        char sz[32];
        human_size(ents[i].size, sz, sizeof(sz));
        printf("%-48s %-12s %-10s %s\n",
               ents[i].name, ents[i].tag, ents[i].id, sz);
    }
    return 0;
}

/*
 * `podvm rmi <image>...` -- remove cached images. An image a live
 * container is using is refused (shared/exclusive flock, the mirror of
 * the run-side guard). Bulk semantics match `podvm rm`: every ref is
 * attempted; non-zero if any failed.
 */
static int cmd_rmi(int argc, char **argv) {
    if (argc < 3 || !strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")) {
        fprintf(stderr, "Usage: podvm rmi <image> [<image>...]\n");
        fprintf(stderr, "Remove cached images (native pulls and podvm build "
                        "output).\n");
        return argc < 3 ? 1 : 0;
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        const char *ref = argv[i];
        char dir[STORE_PATH_MAX];
        struct stat st;

        if (image_is_path(ref) || image_cache_path(ref, dir, sizeof(dir)) < 0) {
            fprintf(stderr, "podvm: not an image reference: %s\n", ref);
            rc = 1;
            continue;
        }
        if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "podvm: no such image: %s\n", ref);
            rc = 1;
            continue;
        }

        int lockfd = image_lock(ref, 1);
        if (lockfd < 0) {
            if (errno == EWOULDBLOCK)
                fprintf(stderr, "podvm: image %s is in use by a running "
                                "container; stop it first\n", ref);
            else
                fprintf(stderr, "podvm: cannot lock image %s: %s\n",
                        ref, strerror(errno));
            rc = 1;
            continue;
        }

        int rmc = container_rm_rf(dir);
        int rme = errno;
        close(lockfd);
        if (rmc < 0) {
            fprintf(stderr, "podvm: cannot remove %s: %s\n",
                    dir, strerror(rme));
            rc = 1;
            continue;
        }
        printf("podvm: removed image %s\n", ref);
    }
    return rc;
}

/* ───────────────────────── podvm build ───────────────────────── */

/*
 * `podvm build [-f Dockerfile] -t <name:tag> [context]` -- a Dockerfile
 * becomes a podvm-native image in the cache (the same layout pulls
 * use, so run/images/rmi see one cache).
 *
 * Each RUN boots a throwaway VM whose overlay chain is [committed
 * layers, FROM's layers] with a fresh named-container upper; after the
 * VM exits the upper IS the committed layer (rename'd into the build
 * dir -- overlayfs has already applied every change to it, including
 * whiteouts as char 0:0 devices, which the next overlay understands
 * natively). COPY and metadata run host-side. The final image is the
 * merged view, copied out through a host-side userns+overlay mount so
 * the kernel -- not podvm -- assembles the chain.
 *
 * RUN scripts ride to the guest in a file in the step's upper
 * (/.podvm-step.sh) rather than over the config channel: a file has no
 * length or separator limits, so quotes, commas, and long scripts all
 * survive. The file lands in committed layers; harmless at runtime.
 */

/* Escape each word of a raw csv (Dockerfile exec form) into escaped csv. */
static int csv_escape_words(const char *raw, char *out, size_t n) {
    size_t off = 0;
    out[0] = '\0';
    const char *t = raw;
    while (t && *t) {
        char word[BUILD_OP_STR], esc[BUILD_OP_STR * 2];
        size_t w = 0;
        while (*t && *t != ',' && w + 1 < sizeof(word)) word[w++] = *t++;
        word[w] = '\0';
        if (*t == ',') t++; else t = NULL;
        if (kv_escape(esc, sizeof(esc), word) == (size_t)-1) esc[0] = '\0';
        int k = snprintf(out + off, n - off, "%s%s", off ? "," : "", esc);
        if (k < 0 || (size_t)k >= n - off) return -1;
        off += (size_t)k;
    }
    return 0;
}

/* JSON-escape a string (quotes, backslash, control chars). */
static void json_escape(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; s[i] && o + 8 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* Emit an escaped-csv as a JSON string array, unescaping each field. */
static void json_csv_array(FILE *f, const char *label, const char *csv) {
    fprintf(f, "\"%s\":[", label);
    int first = 1;
    const char *t = csv;
    while (t && *t) {
        char field[4096], unesc[4096], esc[8200];
        size_t n = 0;
        while (*t && n + 1 < sizeof(field)) {
            if (*t == '\\' && t[1] && n + 2 < sizeof(field)) {
                field[n++] = *t++;
                field[n++] = *t++;
                continue;
            }
            if (*t == ',') break;
            field[n++] = *t++;
        }
        field[n] = '\0';
        if (*t == ',') t++; else t = NULL;
        snprintf(unesc, sizeof(unesc), "%s", field);
        kv_unescape(unesc);
        json_escape(unesc, esc, sizeof(esc));
        fprintf(f, "%s\"%s\"", first ? "" : ",", esc);
        first = 0;
    }
    fprintf(f, "]");
}

/* Insert a committed layer at the front of the chain (top-first). */
static void chain_insert_front(char chain[MAX_CHAIN][MAX_PATH], int *n,
                               const char *path) {
    if (*n >= MAX_CHAIN - 1) return;      /* bounded; count reported */
    memmove(&chain[1], &chain[0], (size_t)(*n) * sizeof(chain[0]));
    snprintf(chain[0], MAX_PATH, "%s", path);
    (*n)++;
}

static int cmd_build(int argc, char **argv) {
    const char *df_arg = NULL, *tag = NULL, *context = NULL;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) df_arg = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            snprintf(config.initramfs, sizeof(config.initramfs), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc)
            snprintf(config.kernel, sizeof(config.kernel), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "Usage: podvm build [-f Dockerfile] -t <name:tag> "
                            "[-r initramfs] [-k kernel] [context]\n");
            fprintf(stderr, "Build a Dockerfile into a podvm image. Supported: FROM,\n");
            fprintf(stderr, "RUN (shell form), COPY, WORKDIR, ENV, USER, CMD, ENTRYPOINT;\n");
            fprintf(stderr, "EXPOSE/LABEL are accepted and ignored. One stage only.\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "podvm build: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!context) {
            context = argv[i];
        } else {
            fprintf(stderr, "podvm build: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!tag) {
        fprintf(stderr, "podvm build: -t <name:tag> is required\n");
        return 2;
    }
    if (!context) context = ".";
    if (image_is_path(tag)) {
        fprintf(stderr, "podvm build: tag must not be a path: '%s'\n", tag);
        return 2;
    }

    char df_path[MAX_PATH];
    if (df_arg) snprintf(df_path, sizeof(df_path), "%s", df_arg);
    else snprintf(df_path, sizeof(df_path), "%s/Dockerfile", context);

    FILE *df = fopen(df_path, "r");
    if (!df) {
        fprintf(stderr, "podvm build: cannot open %s: %s\n",
                df_path, strerror(errno));
        return 1;
    }
    fseek(df, 0, SEEK_END);
    long dlen = ftell(df);
    rewind(df);
    if (dlen < 0) { fclose(df); return 1; }
    char *text = malloc((size_t)dlen + 1);
    if (!text) { fclose(df); return 1; }
    size_t got = fread(text, 1, (size_t)dlen, df);
    text[got] = '\0';
    fclose(df);

    BuildOp ops[BUILD_OPS_MAX];
    char perr[512];
    int nops = dockerfile_parse(text, ops, BUILD_OPS_MAX, perr, sizeof(perr));
    free(text);
    if (nops < 0) {
        fprintf(stderr, "podvm build: %s: %s\n", df_path, perr);
        return 1;
    }
    if (nops > 1) {
        for (int i = 1; i < nops; i++) {
            if (ops[i].kind == BFROM) {
                fprintf(stderr, "podvm build: line %d: multi-stage builds "
                                "are not supported\n", ops[i].line);
                return 1;
            }
        }
    }

    detect_vsock();
    if (config.kernel[0] == '\0')
        get_host_kernel(config.kernel, sizeof(config.kernel));

    char workdir[MAX_PATH], layersdir[MAX_PATH];
    if (container_build_dir(workdir, sizeof(workdir)) < 0) {
        fprintf(stderr, "podvm build: cannot create the build dir: %s\n",
                strerror(errno));
        return 1;
    }
    char wsuffix[MAX_PATH];
    snprintf(wsuffix, sizeof(wsuffix), "/b%d", (int)getpid());
    strncat(workdir, wsuffix, sizeof(workdir) - strlen(workdir) - 1);
    snprintf(layersdir, sizeof(layersdir), "%s/layers", workdir);
    if (mkdir_p(workdir, 0755) < 0 || mkdir_p(layersdir, 0755) < 0) {
        fprintf(stderr, "podvm build: cannot create %s: %s\n",
                workdir, strerror(errno));
        return 1;
    }

    /* ── FROM: resolve to a layer chain + the image's config ── */
    const char *from = ops[0].a;
    char base_diffs[MAX_CHAIN][MAX_PATH];
    int nbase = 0;
    ImageConfig ic;
    image_config_init(&ic);
    int from_podman = 0, from_lock = -1;
    char layer_chain[MAX_CHAIN][MAX_PATH];
    int nlayers = 0;
    char envlist[4096] = "";
    char cwd[MAX_PATH] = "", user[128] = "";
    char cmdcsv[4096] = "", entrycsv[4096] = "";
    int rc = 0;

    (void)from_podman;
    if (image_is_path(from)) {
        fprintf(stderr, "podvm build: line %d: FROM must be an image, "
                        "not a directory\n", ops[0].line);
        rc = 1;
        goto out;
    }
    {
        char top[STORE_PATH_MAX];
        if (store_resolve(from, top, sizeof(top), 1) == 0) {
            StoreLayer lay[MAX_CHAIN];
            int c = store_chain(top, lay, MAX_CHAIN);
            if (c <= 0) {
                fprintf(stderr, "podvm build: no layer chain for %s\n", from);
                rc = 1;
                goto out;
            }
            for (int l = 0; l < c && l < MAX_CHAIN; l++)
                snprintf(base_diffs[l], MAX_PATH, "%s", lay[l].diff);
            nbase = c;
            store_image_config(from, &ic);
            from_podman = 1;
        } else {
            char root[STORE_PATH_MAX];
            if (image_resolve(from, root, sizeof(root)) < 0) {
                fprintf(stderr, "podvm build: cannot resolve FROM image %s\n", from);
                rc = 1;
                goto out;
            }
            snprintf(base_diffs[0], MAX_PATH, "%s", root);
            nbase = 1;
            image_config_load(from, &ic);
            /* The base rootfs is served to every step VM: hold a
             * shared lock so rmi cannot delete it mid-build. */
            from_lock = image_lock(from, 0);
            if (from_lock < 0) {
                fprintf(stderr, "podvm build: cannot lock base image %s: %s\n",
                        from, strerror(errno));
                rc = 1;
                goto out;
            }
        }
    }
    printf("podvm: build from %s (%d base layer%s)\n", from, nbase,
           nbase == 1 ? "" : "s");

    /* Seed metadata from the base image (docker: FROM inherits config). */
    snprintf(cwd, sizeof(cwd), "%s", ic.cwd);
    snprintf(user, sizeof(user), "%s", ic.user);
    for (int e = 0; e < ic.nenv; e++) {
        const char *eq = strchr(ic.env[e], '=');
        size_t klen = eq ? (size_t)(eq - ic.env[e]) : strlen(ic.env[e]);
        const char *val = eq ? eq + 1 : ic.env[e] + klen;
        char enc[BUILD_OP_STR * 2], pair[BUILD_OP_STR * 2];
        if (kv_escape(enc, sizeof(enc), val) == (size_t)-1) enc[0] = '\0';
        snprintf(pair, sizeof(pair), "%.*s=%s", (int)klen, ic.env[e], enc);
        if (build_env_set(envlist, sizeof(envlist), pair) < 0) break;
    }
    {
        char raw[8192];
        size_t off = 0;
        for (int c = 0; c < ic.ncmd; c++) {
            int w = snprintf(raw + off, sizeof(raw) - off, "%s%s",
                             off ? "," : "", ic.cmd[c]);
            if (w < 0 || (size_t)w >= sizeof(raw) - off) break;
            off += (size_t)w;
        }
        if (csv_escape_words(raw, cmdcsv, sizeof(cmdcsv)) < 0) cmdcsv[0] = '\0';
        raw[0] = '\0'; off = 0;
        for (int c = 0; c < ic.nentry; c++) {
            int w = snprintf(raw + off, sizeof(raw) - off, "%s%s",
                             off ? "," : "", ic.entrypoint[c]);
            if (w < 0 || (size_t)w >= sizeof(raw) - off) break;
            off += (size_t)w;
        }
        if (csv_escape_words(raw, entrycsv, sizeof(entrycsv)) < 0) entrycsv[0] = '\0';
    }

    /* ── the op loop ── */
    for (int i = 1; i < nops; i++) {
        BuildOp *op = &ops[i];
        switch (op->kind) {
        case BENV: {
            /* "K=V K2=V2": split on spaces; quoting not supported (v1). */
            char tmp[BUILD_OP_STR];
            snprintf(tmp, sizeof(tmp), "%s", op->a);
            for (char *tok = strtok(tmp, " \t"); tok; tok = strtok(NULL, " \t")) {
                char *eq = strchr(tok, '=');
                if (!eq) {
                    fprintf(stderr, "podvm build: line %d: ENV needs KEY=VALUE\n",
                            op->line);
                    rc = 1;
                    goto out;
                }
                char enc[BUILD_OP_STR * 2], pair[BUILD_OP_STR * 2];
                if (kv_escape(enc, sizeof(enc), eq + 1) == (size_t)-1) enc[0] = '\0';
                *eq = '\0';
                snprintf(pair, sizeof(pair), "%s=%s", tok, enc);
                if (build_env_set(envlist, sizeof(envlist), pair) < 0) {
                    fprintf(stderr, "podvm build: line %d: env list overflow\n",
                            op->line);
                    rc = 1;
                    goto out;
                }
            }
            break;
        }
        case BWORKDIR:
            if (op->a[0] != '/') {
                fprintf(stderr, "podvm build: line %d: WORKDIR must be absolute\n",
                        op->line);
                rc = 1;
                goto out;
            }
            snprintf(cwd, sizeof(cwd), "%s", op->a);
            break;
        case BUSER:
            snprintf(user, sizeof(user), "%s", op->a);
            break;
        case BCMD:
            if (op->json_form) {
                if (csv_escape_words(op->a, cmdcsv, sizeof(cmdcsv)) < 0) {
                    fprintf(stderr, "podvm build: line %d: CMD too long\n", op->line);
                    rc = 1;
                    goto out;
                }
            } else {
                /* shell form: docker stores ["sh","-c",raw] */
                char enc[BUILD_OP_STR * 2];
                if (kv_escape(enc, sizeof(enc), op->a) == (size_t)-1) enc[0] = '\0';
                snprintf(cmdcsv, sizeof(cmdcsv), "sh,-c,%s", enc);
            }
            break;
        case BENTRYPOINT:
            if (op->json_form) {
                if (csv_escape_words(op->a, entrycsv, sizeof(entrycsv)) < 0) {
                    fprintf(stderr, "podvm build: line %d: ENTRYPOINT too long\n",
                            op->line);
                    rc = 1;
                    goto out;
                }
            } else {
                char enc[BUILD_OP_STR * 2];
                if (kv_escape(enc, sizeof(enc), op->a) == (size_t)-1) enc[0] = '\0';
                snprintf(entrycsv, sizeof(entrycsv), "sh,-c,%s", enc);
            }
            break;
        case BCOPY: {
            char ldir[MAX_PATH], srcabs[MAX_PATH], cerr[512];
            if (op->a[0] == '/' || strstr(op->a, "..")) {
                fprintf(stderr, "podvm build: line %d: COPY src must be a "
                                "context-relative path without '..': '%s'\n",
                        op->line, op->a);
                rc = 1;
                goto out;
            }
            snprintf(ldir, sizeof(ldir), "%s/c%03d", layersdir, i);
            if (mkdir_p(ldir, 0755) < 0) {
                fprintf(stderr, "podvm build: cannot create %s: %s\n",
                        ldir, strerror(errno));
                rc = 1;
                goto out;
            }
            snprintf(srcabs, sizeof(srcabs), "%s/%s", context, op->a);
            if (build_copy_path(srcabs, ldir, op->b, cerr, sizeof(cerr)) < 0) {
                fprintf(stderr, "podvm build: line %d: %s\n", op->line, cerr);
                rc = 1;
                goto out;
            }
            printf("podvm: step %d: COPY %s %s\n", i, op->a, op->b);
            chain_insert_front(layer_chain, &nlayers, ldir);
            break;
        }
        case BRUN: {
            char stepname[64], sdir[CONTAINER_PATH_MAX];
            char upath[MAX_PATH], spath[MAX_PATH], ldir[MAX_PATH];
            snprintf(stepname, sizeof(stepname), "pb%dp%d-s%d",
                     (int)getpid(), (int)getppid(), i);

            /* Step chain: committed layers (top-first), then the base. */
            store_nlayers = 0;
            for (int l = 0; l < nlayers && store_nlayers < MAX_CHAIN; l++)
                snprintf(store_layers[store_nlayers++].diff, STORE_PATH_MAX,
                         "%s", layer_chain[l]);
            for (int b = 0; b < nbase && store_nlayers < MAX_CHAIN; b++)
                snprintf(store_layers[store_nlayers++].diff, STORE_PATH_MAX,
                         "%s", base_diffs[b]);

            snprintf(config.name, sizeof(config.name), "%s", stepname);
            snprintf(config.rootfs_dir, sizeof(config.rootfs_dir),
                     "/tmp/podvm-build-ovl.%d.%d", (int)getpid(), i);
            snprintf(config.entrypoint, sizeof(config.entrypoint), "/bin/sh");
            snprintf(config.args, sizeof(config.args), "%s", "/.podvm-step.sh");
            snprintf(config.env, sizeof(config.env), "%s", envlist);
            snprintf(config.cwd, sizeof(config.cwd), "%s", cwd);
            snprintf(config.user, sizeof(config.user), "%s", user);
            config.interactive = 0;
            config.read_only = 0;
            config.autoremove = 0;

            /* The script rides in the step's upper: no channel limits. */
            if (container_state_dir(stepname, sdir, sizeof(sdir)) < 0 || !sdir[0]) {
                fprintf(stderr, "podvm build: cannot locate the state dir\n");
                rc = 1;
                goto out;
            }
            snprintf(upath, sizeof(upath), "%s/u", sdir);
            if (mkdir_p(upath, 0755) < 0) {
                fprintf(stderr, "podvm build: cannot create %s: %s\n",
                        upath, strerror(errno));
                rc = 1;
                goto out;
            }
            snprintf(spath, sizeof(spath), "%s/.podvm-step.sh", upath);
            FILE *sf = fopen(spath, "w");
            if (!sf) {
                fprintf(stderr, "podvm build: cannot write %s: %s\n",
                        spath, strerror(errno));
                rc = 1;
                goto out;
            }
            fprintf(sf, "%s\n", op->a);
            fclose(sf);
            chmod(spath, 0755);

            printf("podvm: step %d: RUN %s\n", i, op->a);
            start_exit_helper();
            if (start_qemu() < 0) {
                fprintf(stderr, "podvm build: cannot start the step VM\n");
                container_rm_rf(sdir);
                container_lock_unlink(stepname);
                rc = 1;
                goto out;
            }
            if (vsock_available) {
                char vcfg[MAX_CMDLINE];
                build_config_tokens(vcfg, sizeof(vcfg));
                if (send_config_vsock(vcfg) < 0)
                    fprintf(stderr, "podvm: vsock config failed; "
                                    "step uses the cmdline\n");
            }
            int st = wait_qemu();
            int gc = reap_exit_helper();
            if (gc >= 0) {
                st = gc;
            } else if (st == 0) {
                fprintf(stderr, "podvm build: warning: no exit report from "
                                "the step VM (no vsock?); assuming success\n");
            }
            if (st != 0) {
                fprintf(stderr, "podvm build: line %d: step failed "
                                "(exit %d)\n", op->line, st);
                container_rm_rf(sdir);
                container_lock_unlink(stepname);
                rc = 1;
                goto out;
            }

            /* Commit: the upper IS the new layer. */
            snprintf(ldir, sizeof(ldir), "%s/l%03d", layersdir, i);
            if (rename(upath, ldir) < 0) {
                fprintf(stderr, "podvm build: cannot commit the step layer: %s\n",
                        strerror(errno));
                container_rm_rf(sdir);
                container_lock_unlink(stepname);
                rc = 1;
                goto out;
            }
            container_rm_rf(sdir);
            container_lock_unlink(stepname);
            chain_insert_front(layer_chain, &nlayers, ldir);
            break;
        }
        default:
            break;
        }
    }

    /* ── assemble the merged view into the cache ── */
    {
        char final[STORE_PATH_MAX];
        if (image_cache_path(tag, final, sizeof(final)) < 0) {
            fprintf(stderr, "podvm build: cannot place image %s in the cache\n", tag);
            rc = 1;
            goto out;
        }
        if (container_rm_rf(final) < 0 && errno != ENOENT) {
            fprintf(stderr, "podvm build: cannot replace %s: %s\n",
                    final, strerror(errno));
            rc = 1;
            goto out;
        }
        if (mkdir_p(final, 0755) < 0) {
            fprintf(stderr, "podvm build: cannot create %s: %s\n",
                    final, strerror(errno));
            rc = 1;
            goto out;
        }

        store_nlayers = 0;
        for (int l = 0; l < nlayers && store_nlayers < MAX_CHAIN; l++)
            snprintf(store_layers[store_nlayers++].diff, STORE_PATH_MAX,
                     "%s", layer_chain[l]);
        for (int b = 0; b < nbase && store_nlayers < MAX_CHAIN; b++)
            snprintf(store_layers[store_nlayers++].diff, STORE_PATH_MAX,
                     "%s", base_diffs[b]);

        pid_t apid = fork();
        if (apid == 0) {
            /* Child: userns overlay over the full chain (the kernel
             * assembles whiteouts/opaque dirs), then cp the merged
             * view into the cache dir. */
            snprintf(config.rootfs_dir, sizeof(config.rootfs_dir),
                     "/tmp/podvm-build-merge.%d", (int)getpid());
            config.name[0] = '\0';
            config.read_only = 0;
            if (setup_userns_overlay() < 0) _exit(1);
            pid_t cpid = fork();
            if (cpid == 0) {
                char srcdot[MAX_PATH + 4], dst[MAX_PATH + 4];
                snprintf(srcdot, sizeof(srcdot), "%s/.", config.rootfs_dir);
                snprintf(dst, sizeof(dst), "%s/", final);
                /* --no-preserve=ownership: root-owned files are not
                 * mapped into our userns (chown -> EINVAL), and the
                 * pull path's untar never preserved ownership either.
                 * Everything lands owned by the invoking user. */
                execlp("cp", "cp", "-a", "--no-preserve=ownership",
                       srcdot, dst, (char *)NULL);
                _exit(127);
            }
            int st = 0;
            waitpid(cpid, &st, 0);
            _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
        }
        int ast = 0;
        waitpid(apid, &ast, 0);
        if (!WIFEXITED(ast) || WEXITSTATUS(ast) != 0) {
            fprintf(stderr, "podvm build: assembling the image failed\n");
            rc = 1;
            goto out;
        }

        /* The OCI-shaped config blob the run path already reads. */
        char cfgpath[MAX_PATH + 32];
        snprintf(cfgpath, sizeof(cfgpath), "%s/.podvm-config.json", final);
        FILE *cf = fopen(cfgpath, "w");
        if (!cf) {
            fprintf(stderr, "podvm build: cannot write %s: %s\n",
                    cfgpath, strerror(errno));
            rc = 1;
            goto out;
        }
        char escwd[STORE_PATH_MAX], escuser[300];
        json_escape(cwd, escwd, sizeof(escwd));
        json_escape(user, escuser, sizeof(escuser));
        fprintf(cf, "{\"architecture\":\"amd64\",\"os\":\"linux\",\"config\":{");
        json_csv_array(cf, "Env", envlist);
        fprintf(cf, ",");
        json_csv_array(cf, "Entrypoint", entrycsv);
        fprintf(cf, ",");
        json_csv_array(cf, "Cmd", cmdcsv);
        fprintf(cf, ",\"WorkingDir\":\"%s\",\"User\":\"%s\"}}\n",
                escwd, escuser);
        fclose(cf);

        printf("podvm: built %s (%d layer%s) at %s\n",
               tag, nlayers + nbase, (nlayers + nbase) == 1 ? "" : "s", final);
    }

out:
    container_rm_rf(workdir);
    if (from_lock >= 0) close(from_lock);
    image_config_free(&ic);
    return rc;
}

/*
 * `podvm stop <name>...` -- stop a detached container. The pidfile is
 * checked against /proc (the cmdline must be this container's QEMU, via
 * its serial log path) so a recycled pid is never signalled. Then
 * SIGTERM, a bounded wait for the VM's name lock to drop (the lock is
 * the ground truth for "the upper is free"), then SIGKILL.
 */
static int cmd_stop(int argc, char **argv) {
    if (argc < 3 || !strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")) {
        fprintf(stderr, "Usage: podvm stop <name> [<name>...]\n");
        fprintf(stderr, "Stop detached containers.\n");
        return argc < 3 ? 1 : 0;
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        const char *name = argv[i];
        char sdir[CONTAINER_PATH_MAX];
        char pidpath[CONTAINER_PATH_MAX + 16], logarg[CONTAINER_PATH_MAX + 32];
        struct stat st;

        if (!container_name_valid(name)) {
            fprintf(stderr, "podvm: invalid container name: '%s'\n", name);
            rc = 1;
            continue;
        }
        if (container_state_dir(name, sdir, sizeof(sdir)) < 0 ||
            stat(sdir, &st) < 0) {
            fprintf(stderr, "podvm: no such container: %s\n", name);
            rc = 1;
            continue;
        }
        snprintf(pidpath, sizeof(pidpath), "%s/pid", sdir);
        FILE *pf = fopen(pidpath, "r");
        if (!pf) {
            fprintf(stderr, "podvm: container '%s' is not running\n", name);
            rc = 1;
            continue;
        }
        int pid = 0;
        (void)fscanf(pf, "%d", &pid);
        fclose(pf);

        snprintf(logarg, sizeof(logarg), "file:%s/log", sdir);
        int stale = (pid <= 0);
        if (!stale) {
            char procpath[64], cl[4096];
            snprintf(procpath, sizeof(procpath), "/proc/%d/cmdline", pid);
            int fd = open(procpath, O_RDONLY);
            if (fd < 0) {
                stale = 1;
            } else {
                ssize_t n = read(fd, cl, sizeof(cl) - 1);
                close(fd);
                if (n <= 0) stale = 1;
                else {
                    cl[n] = '\0';
                    /* /proc cmdline is NUL-separated argv; turn NULs
                     * into spaces so strstr can see every argument. */
                    for (ssize_t k = 0; k < n; k++)
                        if (cl[k] == '\0') cl[k] = ' ';
                    if (!strstr(cl, "qemu-system") || !strstr(cl, logarg))
                        stale = 1;
                }
            }
        }
        if (stale) {
            fprintf(stderr, "podvm: container '%s' is not running "
                            "(stale pidfile)\n", name);
            unlink(pidpath);
            rc = 1;
            continue;
        }

        kill(pid, SIGTERM);
        int stopped = 0;
        for (int t = 0; t < 40 && !stopped; t++) {   /* 10s */
            if (container_locked(name) == 0) stopped = 1;
            else usleep(250000);
        }
        if (!stopped) {
            kill(pid, SIGKILL);
            for (int t = 0; t < 20 && !stopped; t++) {   /* +5s */
                if (container_locked(name) == 0) stopped = 1;
                else usleep(250000);
            }
        }
        if (!stopped) {
            fprintf(stderr, "podvm: container '%s' did not stop\n", name);
            rc = 1;
            continue;
        }
        unlink(pidpath);
        printf("podvm: stopped %s\n", name);
    }
    return rc;
}

/*
 * `podvm logs <name>` -- print a detached container's serial log.
 * (Attached runs write their serial to the terminal; there is no file
 * to read.)
 */
static int cmd_logs(int argc, char **argv) {
    if (argc < 3 || !strcmp(argv[2], "-h") || !strcmp(argv[2], "--help")) {
        fprintf(stderr, "Usage: podvm logs <name>\n");
        fprintf(stderr, "Print a container's serial log (detached runs).\n");
        return argc < 3 ? 1 : 0;
    }
    if (argc > 3) {
        fprintf(stderr, "podvm logs: takes a single container name\n");
        return 1;
    }

    const char *name = argv[2];
    char sdir[CONTAINER_PATH_MAX], logpath[CONTAINER_PATH_MAX + 16];
    if (!container_name_valid(name)) {
        fprintf(stderr, "podvm: invalid container name: '%s'\n", name);
        return 1;
    }
    if (container_state_dir(name, sdir, sizeof(sdir)) < 0) {
        fprintf(stderr, "podvm: cannot locate the state dir\n");
        return 1;
    }
    snprintf(logpath, sizeof(logpath), "%s/log", sdir);

    FILE *f = fopen(logpath, "r");
    if (!f) {
        fprintf(stderr, "podvm: no log for container '%s' "
                        "(not started with --detach?)\n", name);
        return 1;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
}

/* Print usage */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <image|rootfs-dir> [command] [args...]\n", prog);
    fprintf(stderr, "       %s rm <name> [<name>...]\n", prog);
    fprintf(stderr, "       %s images\n", prog);
    fprintf(stderr, "       %s rmi <image> [<image>...]\n", prog);
    fprintf(stderr, "       %s build [-f Dockerfile] -t <name:tag> [context]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -i, --interactive   Give the guest stdin exclusively\n");
    fprintf(stderr, "                      (default: QEMU monitor shares it; Ctrl-A C)\n");
    fprintf(stderr, "  -d, --detach        Run in the background (requires --name;\n");
    fprintf(stderr, "                      serial goes to <state>/log)\n");
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
    fprintf(stderr, "  -v, --volume <h:t[:ro]>  Bind-mount a host directory into\n");
    fprintf(stderr, "                      the container (both paths absolute)\n");
    fprintf(stderr, "      --tmpfs <dir>   Mount a tmpfs at <dir> in the container\n");
    fprintf(stderr, "      --hostname <name>  Set the container hostname\n");
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
    fprintf(stderr, "  stop <name>...      Stop detached containers\n");
    fprintf(stderr, "  logs <name>         Print a detached container's serial log\n");
    fprintf(stderr, "  images              List cached images\n");
    fprintf(stderr, "  rmi <image>...      Remove cached images (refused while in use)\n");
    fprintf(stderr, "  build               Build a Dockerfile into a cached image\n");
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
    if (argc > 1 && strcmp(argv[1], "images") == 0)
        return cmd_images();
    if (argc > 1 && strcmp(argv[1], "rmi") == 0)
        return cmd_rmi(argc, argv);
    if (argc > 1 && strcmp(argv[1], "build") == 0)
        return cmd_build(argc, argv);
    if (argc > 1 && strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);
    if (argc > 1 && strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);
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
        {"tmpfs",       required_argument, 0, OPT_TMPFS},
        {"hostname",     required_argument, 0, OPT_HOSTNAME},
        {"volume",      required_argument, 0, 'v'},
        {"rm",          no_argument,       0, OPT_RM},
        {"detach",      no_argument,       0, 'd'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    /* '+' stops getopt at the first non-option (the image/rootfs), so
     * flags meant for the entrypoint (e.g. `podvm img /bin/sh -c ...`)
     * are not stolen as podvm options. */
    while ((opt = getopt_long(argc, argv, "+:die:u:w:p:v:m:c:k:r:n:h", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'i':
            config.interactive = 1;
            break;
        case 'd':
            config.detach = 1;
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
        case 'v':
            if (volume_add(optarg) < 0) return 1;
            break;
        case OPT_TMPFS:
            if (tmpfs_add(optarg) < 0) return 1;
            break;
        case OPT_HOSTNAME:
            if (!optarg[0]) {
                fprintf(stderr, "podvm: --hostname must not be empty\n");
                return 1;
            }
            snprintf(config.hostname, sizeof(config.hostname), "%s", optarg);
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

    /* Detached runs are named and have no supervisor to clean up after
     * them. */
    if (config.detach) {
        if (!config.name[0]) {
            fprintf(stderr, "podvm: --detach requires --name\n");
            return 1;
        }
        if (config.autoremove) {
            fprintf(stderr, "podvm: --detach cannot be combined with --rm "
                            "(nothing would clean the state up)\n");
            return 1;
        }
        if (config.interactive) {
            fprintf(stderr, "podvm: --detach cannot be combined with "
                            "--interactive\n");
            return 1;
        }
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

        /* Build args string. Each argv element is escaped HERE -- the
         * last place that knows which commas are separators -- so an
         * argument containing a comma reaches the guest intact
         * (csv_encode will not re-escape). */
        char *p = config.args;
        char *end = config.args + sizeof(config.args) - 1;
        for (int j = optind; j < argc && p < end; j++) {
            if (j > optind) *p++ = ',';
            char esc[4096];
            if (kv_escape(esc, sizeof(esc), argv[j]) == (size_t)-1)
                snprintf(esc, sizeof(esc), "");
            int n = snprintf(p, (size_t)(end - p), "%s", esc);
            if (n < 0 || p + n >= end) {
                fprintf(stderr, "podvm: command too long for the config channel\n");
                return 1;
            }
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
    int used_native = 0;

    /*
     * Resolution order:
     *   1. directory path -> use as-is
     *   2. podvm's native layered store (ADR-027) -> layers in place
     *   3. podman storage -> layers in place (pull via podman if missing)
     *   4. podvm's own registry pull -> cache
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

        /* 2. podvm's native layered store (ADR-027); skipped when the
         * user forced podman. */
        if (!want_podman && native_has(config.rootfs_dir) == 0) {
            NativeLayer nl[MAX_CHAIN];
            int nn = native_chain(config.rootfs_dir, nl, MAX_CHAIN);
            if (nn <= 0) {
                fprintf(stderr, "podvm: no native layer chain for %s\n",
                        config.rootfs_dir);
                return 1;
            }
            store_nlayers = 0;
            for (int i = 0; i < nn && store_nlayers < MAX_CHAIN; i++)
                snprintf(store_layers[store_nlayers++].diff, STORE_PATH_MAX,
                         "%s", nl[i].diff);
            printf("podvm: using native store: %s (%d layers)\n",
                   config.rootfs_dir, store_nlayers);
            used_native = 1;
            resolved = 1;
            snprintf(resolved_rootfs, sizeof(resolved_rootfs),
                     "/tmp/podvm-rootfs.%d", getpid());
        }
        /* 3. podman storage (skipped when the user forced native). */
        else if (!want_native &&
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

        /* 4. podvm's own registry pull (skipped when the user forced
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
        int got = used_native
                ? native_config(image_ref, &ic)
                : used_podman
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

    /*
     * One live container per name. The write-back upper cannot serve
     * two writers (overlayfs), so a named run on the overlay path takes
     * an exclusive, non-blocking lock before booting; a second run
     * fails fast. The QEMU child inherits the fd (no CLOEXEC), so the
     * lock lives exactly as long as the VM -- even if this process dies
     * first. Unnamed runs share nothing and --read-only uses a
     * throwaway tmpfs upper, so neither needs the lock.
     */
    int state_lock_fd = -1;
    if (store_nlayers > 0 && config.name[0] && !config.read_only) {
        state_lock_fd = container_lock(config.name);
        if (state_lock_fd < 0) {
            if (errno == EWOULDBLOCK)
                fprintf(stderr, "podvm: container '%s' is already running "
                                "(one live run per name)\n", config.name);
            else
                fprintf(stderr, "podvm: cannot lock container '%s': %s\n",
                        config.name, strerror(errno));
            return 1;
        }
    }

    /* A cached rootfs is served to the VM over 9p: hold a shared lock
     * so `podvm rmi` cannot delete it under a live container. Podman
     * storage and plain directories are not podvm's cache, so they are
     * not locked here. The QEMU child inherits the fd. */
    int img_lock_fd = -1;
    if (!used_podman && !image_is_path(image_ref)) {
        img_lock_fd = image_lock(image_ref, 0);
        if (img_lock_fd < 0) {
            fprintf(stderr, "podvm: cannot lock image %s: %s\n",
                    image_ref, strerror(errno));
            return 1;
        }
    }

    /* Setup signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Detect host vsock support (determines QEMU device + config channel). */
    detect_vsock();
    if (!config.detach)
        start_exit_helper();

    /* Start QEMU (9p serves the rootfs in-process; no virtiofsd) */
    if (start_qemu() < 0) {
        fprintf(stderr, "podvm: failed to start QEMU\n");
        maybe_autoremove();   /* --rm still drops the name's state */
        return 1;
    }

    /* Detached: hand the VM over. QEMU inherited the state and image
     * locks, so they are held exactly as long as it lives; the serial
     * is already going to <state>/log. Without the vsock exchange the
     * guest uses the kernel cmdline config -- warn if it may be
     * truncated (x86 cmdline limit ~2048). */
    if (config.detach) {
        char cmdline[MAX_CMDLINE];
        char sdir[CONTAINER_PATH_MAX], pidpath[CONTAINER_PATH_MAX + 16];
        build_cmdline(cmdline, sizeof(cmdline));
        if (strlen(cmdline) > 2000)
            fprintf(stderr, "podvm: warning: config is %zu bytes; detached "
                            "runs cannot use the vsock channel and the "
                            "kernel cmdline may truncate it\n",
                    strlen(cmdline));
        if (container_state_dir(config.name, sdir, sizeof(sdir)) < 0) {
            fprintf(stderr, "podvm: cannot locate the state dir\n");
            kill(qemu_pid, SIGTERM);
            return 1;
        }
        snprintf(pidpath, sizeof(pidpath), "%s/pid", sdir);
        FILE *pf = fopen(pidpath, "w");
        if (!pf) {
            fprintf(stderr, "podvm: cannot write %s: %s\n",
                    pidpath, strerror(errno));
            kill(qemu_pid, SIGTERM);
            return 1;
        }
        fprintf(pf, "%d\n", (int)qemu_pid);
        fclose(pf);
        printf("podvm: %s is running in the background (pid %d)\n",
               config.name, (int)qemu_pid);
        printf("podvm:   logs: podvm logs %s\n", config.name);
        printf("podvm:   stop:  podvm stop %s\n", config.name);
        return 0;
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

    /* The command's own exit code, when the guest reported it (vsock). */
    int guest_code = reap_exit_helper();

    /* --rm: this named container's state dies with the run. */
    maybe_autoremove();
    return guest_code >= 0 ? guest_code : rc;
}
