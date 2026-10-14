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
#include <stdarg.h>
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
#include "cli.h"

#include <limits.h>    /* PATH_MAX */

#define MAX_CMDLINE 4096

/* Run-CLI parse state (config, publishes, volumes, tmpfs) lives in
 * cli.c; main.c consumes it below. */

/* Resolve the initramfs when -r is omitted. Order:
 *   1. next to the running binary (dev tree: build/initramfs.cpio) --
 *      the binary you run owns the initramfs you get;
 *   2. the installed copy (/var/lib/podvm/kernel/initramfs.cpio),
 *      which is software payload of a system-wide install.
 * Returns 1 if config.initramfs was filled, 0 if nothing was found. */
static int resolve_initramfs(void) {
    static const char *installed = "/var/lib/podvm/kernel/initramfs.cpio";
    char exe[PATH_MAX], cand[PATH_MAX];
    struct stat st;
    ssize_t n;
    char *slash;

    if (config.initramfs[0]) return 1;      /* -r given */

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        int cn = snprintf(cand, sizeof(cand), "%s/initramfs.cpio", exe);
        /* A too-long candidate is an invalid path -- skip it (fall
         * through to the installed copy) rather than stat a truncated one. */
        if (cn >= 0 && (size_t)cn < sizeof(cand) && stat(cand, &st) == 0) {
            snprintf(config.initramfs, sizeof(config.initramfs), "%s", cand);
            return 1;
        }
    }
    if (stat(installed, &st) == 0) {
        snprintf(config.initramfs, sizeof(config.initramfs), "%s", installed);
        return 1;
    }
    return 0;
}

/* Create a directory and all missing parents (like mkdir -p). */
static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
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

/* Append a fixed suffix (e.g. "/u") to a path. Returns -1 if it would not
 * fit -- a truncated overlay upper/workdir path is a broken mount, so
 * callers must treat overflow as fatal rather than continue with a
 * silently-shortened path. */
static int path_append(char *dst, size_t dstsz, const char *base,
                       const char *suffix) {
    int n = snprintf(dst, dstsz, "%s%s", base, suffix);
    return (n < 0 || (size_t)n >= dstsz) ? -1 : 0;
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

/* Fill store_layers from the native store's chain (top-first, overlayfs
 * lowerdir order) and print the diagnostic. Returns the layer count, or
 * -1 on error. */
static int fill_native_chain(const char *img) {
    NativeLayer nl[MAX_CHAIN];
    int nn = native_chain(img, nl, MAX_CHAIN);
    if (nn <= 0) return -1;
    store_nlayers = 0;
    for (int i = 0; i < nn && store_nlayers < MAX_CHAIN; i++)
        snprintf(store_layers[store_nlayers++].diff, STORE_PATH_MAX,
                 "%s", nl[i].diff);
    printf("podvm: using native store: %s (%d layers)\n", img, store_nlayers);
    return store_nlayers;
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
 * Append printf-formatted text to the config token buffer at *used,
 * advancing *used. Sets *trunc and stops writing if the value would not
 * fit: a truncated config token string is MISPARSED by the guest (it
 * reads podvm.args=... as podvm.env=...), so once anything is dropped the
 * whole config is invalid -- callers fail hard rather than launch with a
 * silently-corrupt cmdline.
 */
static void tok_append(char *buf, size_t len, size_t *used, int *trunc,
                       const char *fmt, ...) {
    if (*trunc) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *used, len - *used, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= len - *used) { *trunc = 1; return; }
    *used += (size_t)w;
}

/*
 * Build the podvm.*= token string, escaping every value so spaces and
 * commas survive the channel. Shared by the kernel cmdline and the vsock
 * payload so the two can never diverge on encoding.
 *
 * Returns 0, or -1 if the assembled config would exceed len (a truncated
 * config is unusable -- the guest must never see a partial token string).
 */
static int build_config_tokens(char *buf, size_t len) {
    char entry[PATH_MAX * 2], args[MAX_CMDLINE];
    char cwd[PATH_MAX * 2], user[sizeof(config.user) * 2];
    size_t used = 0;
    int trunc = 0;

    escape_value(entry, sizeof(entry), config.entrypoint);
    {
        /* args/env csv fields are pre-escaped by their builders (argv
         * join, env_add, imgcfg); csv_encode only preserves them. */
        csv_encode(args, sizeof(args), config.args[0] ? config.args : "-");
    }

    /* HOME is ensured for images by imgcfg_fold(); do the same here so
     * the bare-rootfs path (and -e with no image) also always has one,
     * as docker guarantees. envraw/env must be able to hold config.env
     * PLUS the ",HOME=/root" suffix -- sizing them to config.env alone
     * silently truncates the tail. Any real wire overflow is still
     * caught by tok_append below. */
    char envraw[sizeof(config.env) + 16], env[sizeof(config.env) + 16];
    if (config.env[0])
        snprintf(envraw, sizeof(envraw), "%s%s", config.env,
                 strstr(config.env, "HOME=") ? "" : ",HOME=/root");
    else
        snprintf(envraw, sizeof(envraw), "HOME=/root");
    csv_encode(env, sizeof(env), envraw);
    escape_value(cwd, sizeof(cwd), config.cwd[0] ? config.cwd : "/");
    escape_value(user, sizeof(user), config.user[0] ? config.user : "-");

    tok_append(buf, len, &used, &trunc,
        "podvm.root=/rootfs "
        "podvm.entrypoint=%s "
        "podvm.args=%s "
        "podvm.env=%s "
        "podvm.cwd=%s "
        "podvm.user=%s",
        entry, args, env, cwd, user);

    /* Only sent when non-default, so ordinary cmdlines (and the e2e
     * assertions on them) are unchanged. */
    if (config.net_none)
        tok_append(buf, len, &used, &trunc, " podvm.net=none");
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
        tok_append(buf, len, &used, &trunc, " podvm.vol=%s", volesc);
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
        tok_append(buf, len, &used, &trunc, " podvm.tmpfs=%s", tesc);
    }
    if (config.hostname[0]) {
        char hostesc[600];
        escape_value(hostesc, sizeof(hostesc), config.hostname);
        tok_append(buf, len, &used, &trunc, " podvm.hostname=%s", hostesc);
    }
    /* The exit-report port this run's helper listens on. Sent only when
     * one was allocated (not --detach), so the guest dials the port the
     * host actually bound instead of the old hardcoded 5001. */
    if (vsock_exit_port > 0)
        tok_append(buf, len, &used, &trunc, " podvm.exitport=%d", vsock_exit_port);

    return trunc ? -1 : 0;
}

/* Build kernel cmdline. Returns 0, or -1 if the config does not fit. */
static int build_cmdline(char *buf, size_t len) {
    char tokens[MAX_CMDLINE];
    if (build_config_tokens(tokens, sizeof(tokens)) < 0) return -1;
    int w = snprintf(buf, len, "console=ttyS0 panic=-1 %s", tokens);
    return (w < 0 || (size_t)w >= len) ? -1 : 0;
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
        if (path_append(updir, sizeof(updir), state, "/u") < 0 ||
            path_append(wkdir, sizeof(wkdir), state, "/w") < 0) {
            fprintf(stderr, "podvm: container state path too long: %s\n", state);
            return -1;
        }
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
        if (path_append(updir, sizeof(updir), basedir, "/u") < 0 ||
            path_append(wkdir, sizeof(wkdir), basedir, "/w") < 0) {
            fprintf(stderr, "podvm: overlay base path too long: %s\n", basedir);
            return -1;
        }

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
    int ow = snprintf(opts, sizeof(opts),
                      "lowerdir=%s,upperdir=%s,workdir=%s,userxattr",
                      lowers, updir, wkdir);
    /* lowers+updir+wkdir can exceed opts; a truncated mount-option string
     * is a broken mount, so fail rather than mount with a cut path. */
    if (ow < 0 || (size_t)ow >= sizeof(opts)) {
        fprintf(stderr, "podvm: overlay options too long\n");
        return -1;
    }

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
    char fsdev_str[PATH_MAX + 64];
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
        if (!resolve_initramfs()) {
            fprintf(stderr, "podvm: initramfs not found: build it "
                            "('make initramfs' -> build/initramfs.cpio), "
                            "install it ('make install'), or pass -r\n");
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
    char vol_fsdev[MAX_VOLUMES][PATH_MAX + 64];
    char vol_dev[MAX_VOLUMES][64];
    char vol_tag[MAX_VOLUMES][16];

    if (build_cmdline(cmdline, sizeof(cmdline)) < 0) {
        fprintf(stderr, "podvm: config too large for the kernel cmdline "
                        "(> %d bytes); reduce args/env\n", MAX_CMDLINE);
        return -1;
    }
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
        int fw = snprintf(vol_fsdev[i], sizeof(vol_fsdev[i]),
                          "local,id=%s,path=%s,security_model=none%s",
                          vol_tag[i], volumes[i].host,
                          volumes[i].ro ? ",readonly=on" : "");
        int dw = snprintf(vol_dev[i], sizeof(vol_dev[i]),
                          "virtio-9p-pci,fsdev=%s,mount_tag=%s",
                          vol_tag[i], vol_tag[i]);
        /* A truncated -fsdev path would mount the wrong host directory;
         * fail rather than start QEMU with a silently-cut volume. */
        if (fw < 0 || (size_t)fw >= sizeof(vol_fsdev[i]) ||
            dw < 0 || (size_t)dw >= sizeof(vol_dev[i])) {
            fprintf(stderr, "podvm: volume %d path too long for QEMU\n", i);
            return -1;
        }
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


/*
 * `podvm images` -- list the podvm store (pulls and build output).
 */
static int cmp_natimg(const void *a, const void *b) {
    const NativeImage *x = a, *y = b;
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
    static NativeImage nents[MAX_IMGS];
    int nn = native_list(nents, MAX_IMGS);
    if (nn == 0) {
        printf("no images (native store %s)\n", native_store_dir());
        return 0;
    }
    qsort(nents, (size_t)nn, sizeof(nents[0]), cmp_natimg);
    printf("%-48s %-12s %-7s %s\n", "NAME", "TAG", "LAYERS", "SIZE");
    for (int i = 0; i < nn; i++) {
        char sz[32], lay[16];
        human_size(nents[i].size, sz, sizeof(sz));
        snprintf(lay, sizeof(lay), "%d", nents[i].nlayers);
        printf("%-48s %-12s %-7s %s\n", nents[i].name, nents[i].tag, lay, sz);
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
        if (image_is_path(ref)) {
            fprintf(stderr, "podvm: not an image reference: %s\n", ref);
            rc = 1;
            continue;
        }
        if (native_has(ref) != 0) {
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

        int rmc = native_rmi(ref);
        int rme = errno;
        close(lockfd);
        if (rmc < 0) {
            fprintf(stderr, "podvm: cannot remove %s: %s\n", ref, strerror(rme));
            rc = 1;
            continue;
        }
        printf("podvm: removed image %s\n", ref);
    }
    return rc;
}

/* ───────────────────── podvm save / load ───────────────────── */

/*
 * `podvm save [-o FILE] IMAGE` -- write IMAGE as a docker-archive tar
 * (the `podman save` counterpart; loadable by podman/docker load).
 * stdout when -o is omitted.
 */
static int cmd_save(int argc, char **argv) {
    const char *out = NULL, *image = NULL;
    int format = 0;   /* docker-archive */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--format") && i + 1 < argc) {
            const char *f = argv[++i];
            if (strcmp(f, "docker-archive") != 0 && strcmp(f, "v2s2") != 0) {
                fprintf(stderr, "podvm save: unsupported format '%s'\n", f);
                return 2;
            }
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "Usage: podvm save [-o FILE] IMAGE\n");
            fprintf(stderr, "Save an image as a docker-archive tar "
                            "(podman/docker load compatible).\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "podvm save: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!image) image = argv[i];
        else { fprintf(stderr, "podvm save: unexpected argument '%s'\n", argv[i]); return 2; }
    }
    if (!image) { fprintf(stderr, "podvm save: an image is required\n"); return 2; }

    if (out) return native_save(image, out, format) < 0 ? 1 : 0;
    /* stdout: save to a temp file, then copy out. */
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/podvm-save-%d.tar", (int)getpid());
    if (native_save(image, tmp, format) < 0) return 1;
    FILE *f = fopen(tmp, "rb");
    if (!f) { unlink(tmp); return 1; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
    unlink(tmp);
    return 0;
}

/*
 * `podvm load [-i FILE]` -- ingest a docker-archive tar (the `podman
 * load` counterpart). Reads stdin when -i is omitted.
 */
static int cmd_load(int argc, char **argv) {
    const char *in = NULL;
    char tmp[64];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "Usage: podvm load [-i FILE]\n");
            fprintf(stderr, "Load a docker-archive tar into the podvm store.\n");
            return 0;
        } else {
            fprintf(stderr, "podvm load: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    int owned = 0;
    if (!in) {
        /* stdin -> temp file */
        snprintf(tmp, sizeof(tmp), "/tmp/podvm-load-%d.tar", (int)getpid());
        FILE *f = fopen(tmp, "wb");
        if (!f) { fprintf(stderr, "podvm load: cannot write temp\n"); return 1; }
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) fwrite(buf, 1, n, f);
        fclose(f);
        in = tmp;
        owned = 1;
    }
    int rc = native_load(in);
    if (owned) unlink(tmp);
    return rc < 0 ? 1 : 0;
}

/*
 * `podvm system prune` -- garbage-collect the native store: drop blobs
 * and layer dirs no manifest references (the `podman system prune`
 * counterpart, scoped to images).
 */
static int cmd_system(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[2], "prune") != 0) {
        fprintf(stderr, "Usage: podvm system prune\n");
        fprintf(stderr, "Remove unreferenced blobs/layers from the podvm store.\n");
        return argc < 3 ? 1 : 2;
    }
    int freed = 0;
    if (native_gc(&freed) < 0) return 1;
    printf("podvm: pruned %d unreferenced store entr%s\n",
           freed, freed == 1 ? "y" : "ies");
    return 0;
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
static void chain_insert_front(char chain[MAX_CHAIN][PATH_MAX], int *n,
                               const char *path) {
    if (*n >= MAX_CHAIN - 1) return;      /* bounded; count reported */
    memmove(&chain[1], &chain[0], (size_t)(*n) * sizeof(chain[0]));
    snprintf(chain[0], PATH_MAX, "%s", path);
    (*n)++;
}

/* ─────────────── multi-stage build state (podvm build) ─────────────── */

/*
 * Everything one FROM ... [next FROM] owns. Each stage builds its own
 * layer chain on top of its own base; the LAST stage is the image.
 * Stage-sized arrays make this too big for the stack, so cmd_build
 * heap-allocates an array of these.
 */
typedef struct {
    char name[128];                          /* "AS name", may be empty */
    char base_diffs[MAX_CHAIN][PATH_MAX];    /* base chain, top-first */
    int  nbase;
    char base_dig_bottom[MAX_CHAIN][NATIVE_DIGEST_MAX]; /* bottom-first */
    int  nbase_dig;
    char layer_chain[MAX_CHAIN][PATH_MAX];   /* committed step layers, top-first */
    int  nlayers;
    char step_dig[MAX_CHAIN][NATIVE_DIGEST_MAX]; /* step digests, build order */
    int  nstepdig;
    char envlist[4096];
    char cwd[PATH_MAX];
    char user[128];
    char cmdcsv[4096];
    char entrycsv[4096];
} BuildStage;

/*
 * Resolve an image ref (podman store first, then the native layered
 * store) into its layer chain. Fills diffs (top-first) and, when
 * digs_bottom is non-NULL, the digests (bottom-first; podman layers are
 * committed to the native store first). ic, when non-NULL, receives the
 * image config. Returns the layer count, or -1 with a message in err.
 */
static int resolve_image_chain(const char *ref,
                               char diffs[MAX_CHAIN][PATH_MAX],
                               char digs_bottom[MAX_CHAIN][NATIVE_DIGEST_MAX],
                               ImageConfig *ic,
                               char *err, size_t errsz) {
    char top[STORE_PATH_MAX];
    if (store_resolve(ref, top, sizeof(top), 1) == 0) {
        StoreLayer lay[MAX_CHAIN];
        int c = store_chain(top, lay, MAX_CHAIN);
        if (c <= 0) {
            snprintf(err, errsz, "no layer chain for %s", ref);
            return -1;
        }
        for (int l = 0; l < c && l < MAX_CHAIN; l++)
            memcpy(diffs[l], lay[l].diff, sizeof(lay[l].diff));
        if (digs_bottom) {
            /* Commit each base layer into the native store (bottom-first
             * for the manifest). Copy-to-native, simple; optimize later. */
            for (int l = 0; l < c && l < MAX_CHAIN; l++) {
                if (native_commit_diff(lay[l].diff,
                                       digs_bottom[c - 1 - l],
                                       NATIVE_DIGEST_MAX) < 0) {
                    snprintf(err, errsz, "cannot commit base layer");
                    return -1;
                }
            }
        }
        if (ic) store_image_config(ref, ic);
        return c;
    }

    char root[STORE_PATH_MAX];
    if (image_resolve(ref, root, sizeof(root)) != IMAGE_RESOLVED_NATIVE) {
        snprintf(err, errsz, "cannot resolve FROM image %s", ref);
        return -1;
    }
    /* Base lives in the layered store: use its chain. */
    NativeLayer nl[MAX_CHAIN];
    int c = native_chain(ref, nl, MAX_CHAIN);
    if (c <= 0) {
        snprintf(err, errsz, "no native chain for %s", ref);
        return -1;
    }
    for (int l = 0; l < c && l < MAX_CHAIN; l++)
        snprintf(diffs[l], PATH_MAX, "%s", nl[l].diff);
    /* Reuse the base's existing digests (bottom-first). */
    if (digs_bottom) {
        for (int l = 0; l < c && l < MAX_CHAIN; l++)
            memcpy(digs_bottom[c - 1 - l], nl[l].digest, sizeof(nl[l].digest));
    }
    if (ic) native_config(ref, ic);
    return c;
}

/* Seed a stage's metadata from a base image's config (docker: FROM
 * inherits the base image's config). */
static void stage_seed_from_config(BuildStage *st, const ImageConfig *ic) {
    snprintf(st->cwd, sizeof(st->cwd), "%s", ic->cwd);
    snprintf(st->user, sizeof(st->user), "%s", ic->user);
    for (int e = 0; e < ic->nenv; e++) {
        const char *eq = strchr(ic->env[e], '=');
        size_t klen = eq ? (size_t)(eq - ic->env[e]) : strlen(ic->env[e]);
        const char *val = eq ? eq + 1 : ic->env[e] + klen;
        char enc[BUILD_OP_STR * 2], pair[BUILD_OP_STR * 2];
        if (kv_escape(enc, sizeof(enc), val) == (size_t)-1) enc[0] = '\0';
        snprintf(pair, sizeof(pair), "%.*s=%s", (int)klen, ic->env[e], enc);
        if (build_env_set(st->envlist, sizeof(st->envlist), pair) < 0) break;
    }
    char raw[8192];
    size_t off = 0;
    for (int c = 0; c < ic->ncmd; c++) {
        int w = snprintf(raw + off, sizeof(raw) - off, "%s%s",
                         off ? "," : "", ic->cmd[c]);
        if (w < 0 || (size_t)w >= sizeof(raw) - off) break;
        off += (size_t)w;
    }
    if (csv_escape_words(raw, st->cmdcsv, sizeof(st->cmdcsv)) < 0)
        st->cmdcsv[0] = '\0';
    raw[0] = '\0'; off = 0;
    for (int c = 0; c < ic->nentry; c++) {
        int w = snprintf(raw + off, sizeof(raw) - off, "%s%s",
                         off ? "," : "", ic->entrypoint[c]);
        if (w < 0 || (size_t)w >= sizeof(raw) - off) break;
        off += (size_t)w;
    }
    if (csv_escape_words(raw, st->entrycsv, sizeof(st->entrycsv)) < 0)
        st->entrycsv[0] = '\0';
}

/* dst of a COPY: with multiple sources docker requires a directory
 * target and each file lands at dst/<basename>. */
static void copy_target(const char *dstabs, const char *src, int multi,
                        char *out, size_t n) {
    if (!multi) {
        snprintf(out, n, "%s", dstabs);
        return;
    }
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') bl--;      /* "dir/" -> "dir" */
    snprintf(out, n, "%s/%.*s", dstabs, (int)bl, base);
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
        else if (!strcmp(argv[i], "-m") && i + 1 < argc)
            config.memory = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc)
            config.cpus = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr, "Usage: podvm build [-f Dockerfile] -t <name:tag> "
                            "[-r initramfs] [-k kernel] [-m MB] [-c CPUS] [context]\n");
            fprintf(stderr, "Build a Dockerfile into a podvm image. Supported: FROM\n");
            fprintf(stderr, "(multi-stage: 'FROM x AS name'), RUN (shell form), COPY\n");
            fprintf(stderr, "(context-relative, or '--from=<stage|image>'), WORKDIR,\n");
            fprintf(stderr, "ENV, USER, CMD, ENTRYPOINT; EXPOSE/LABEL are accepted\n");
            fprintf(stderr, "and ignored. -m/-c size the RUN-step VMs.\n");
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

    char df_path[PATH_MAX];
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

    detect_vsock();
    if (config.kernel[0] == '\0')
        get_host_kernel(config.kernel, sizeof(config.kernel));

    char workdir[PATH_MAX], layersdir[PATH_MAX];
    if (container_build_dir(workdir, sizeof(workdir)) < 0) {
        fprintf(stderr, "podvm build: cannot create the build dir: %s\n",
                strerror(errno));
        return 1;
    }
    char wsuffix[PATH_MAX];
    snprintf(wsuffix, sizeof(wsuffix), "/b%d", (int)getpid());
    strncat(workdir, wsuffix, sizeof(workdir) - strlen(workdir) - 1);
    snprintf(layersdir, sizeof(layersdir), "%s/layers", workdir);
    if (mkdir_p(workdir, 0755) < 0 || mkdir_p(layersdir, 0755) < 0) {
        fprintf(stderr, "podvm build: cannot create %s: %s\n",
                workdir, strerror(errno));
        return 1;
    }

    /* ── stages: each FROM starts a new one; the last one is the image ── */
    BuildStage *stages = calloc(BUILD_STAGES_MAX, sizeof(*stages));
    if (!stages) {
        fprintf(stderr, "podvm build: out of memory\n");
        return 1;
    }
    int nstages = 0;
    BuildStage *cur = NULL;
    int rc = 0;

    /* ── the op loop ── */
    for (int i = 0; i < nops; i++) {
        BuildOp *op = &ops[i];

        if (op->kind == BFROM) {
            char perr[512];
            if (nstages >= BUILD_STAGES_MAX) {
                fprintf(stderr, "podvm build: too many stages (max %d)\n",
                        BUILD_STAGES_MAX);
                rc = 1;
                goto out;
            }
            for (int s = 0; s < nstages; s++) {
                if (op->b[0] && !strcmp(stages[s].name, op->b)) {
                    fprintf(stderr, "podvm build: line %d: duplicate stage "
                                    "name '%s'\n", op->line, op->b);
                    rc = 1;
                    goto out;
                }
            }
            BuildStage *st = &stages[nstages];
            if (op->b[0])
                snprintf(st->name, sizeof(st->name), "%s", op->b);

            if (image_is_path(op->a)) {
                fprintf(stderr, "podvm build: line %d: FROM must be an image, "
                                "not a directory\n", op->line);
                rc = 1;
                goto out;
            }
            int src_stage = -1;
            for (int s = 0; s < nstages; s++)
                if (stages[s].name[0] && !strcmp(stages[s].name, op->a))
                    src_stage = s;
            if (src_stage >= 0) {
                /* FROM <prior stage>: that stage's whole chain becomes
                 * our base, and its final config is inherited. */
                BuildStage *src = &stages[src_stage];
                for (int l = 0; l < src->nlayers && st->nbase < MAX_CHAIN; l++)
                    snprintf(st->base_diffs[st->nbase++], PATH_MAX, "%s",
                             src->layer_chain[l]);
                for (int b = 0; b < src->nbase && st->nbase < MAX_CHAIN; b++)
                    snprintf(st->base_diffs[st->nbase++], PATH_MAX, "%s",
                             src->base_diffs[b]);
                for (int b = 0; b < src->nbase_dig && st->nbase_dig < MAX_CHAIN;
                     b++)
                    snprintf(st->base_dig_bottom[st->nbase_dig++],
                             NATIVE_DIGEST_MAX, "%s", src->base_dig_bottom[b]);
                for (int s2 = 0; s2 < src->nstepdig && st->nbase_dig < MAX_CHAIN;
                     s2++)
                    snprintf(st->base_dig_bottom[st->nbase_dig++],
                             NATIVE_DIGEST_MAX, "%s", src->step_dig[s2]);
                /* st is stages[nstages], src is stages[src_stage<nstages]:
                 * distinct elements, but both index the same stages array so
                 * gcc cannot prove it and warns -Wrestrict on snprintf(dst,
                 * ..., src). These are identical fixed-size fields, so a full
                 * memcpy is the exact copy (no truncation possible) and is
                 * aliasing-safe by construction. */
                memcpy(st->envlist, src->envlist, sizeof(st->envlist));
                memcpy(st->cwd, src->cwd, sizeof(st->cwd));
                memcpy(st->user, src->user, sizeof(st->user));
                memcpy(st->cmdcsv, src->cmdcsv, sizeof(st->cmdcsv));
                memcpy(st->entrycsv, src->entrycsv, sizeof(st->entrycsv));
            } else {
                ImageConfig ic;
                image_config_init(&ic);
                int c = resolve_image_chain(op->a, st->base_diffs,
                                            st->base_dig_bottom, &ic,
                                            perr, sizeof(perr));
                if (c < 0) {
                    fprintf(stderr, "podvm build: line %d: %s\n", op->line, perr);
                    image_config_free(&ic);
                    rc = 1;
                    goto out;
                }
                st->nbase = c;
                st->nbase_dig = c;
                stage_seed_from_config(st, &ic);
                image_config_free(&ic);
            }
            nstages++;
            cur = st;
            printf("podvm: stage %d: FROM %s (%d base layer%s)\n",
                   nstages, op->a, cur->nbase, cur->nbase == 1 ? "" : "s");
            continue;
        }

        if (!cur) {   /* the parser requires FROM first; belt + braces */
            fprintf(stderr, "podvm build: line %d: instruction before FROM\n",
                    op->line);
            rc = 1;
            goto out;
        }

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
                if (build_env_set(cur->envlist, sizeof(cur->envlist), pair) < 0) {
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
            snprintf(cur->cwd, sizeof(cur->cwd), "%s", op->a);
            break;
        case BUSER:
            snprintf(cur->user, sizeof(cur->user), "%s", op->a);
            break;
        case BCMD:
            if (op->json_form) {
                if (csv_escape_words(op->a, cur->cmdcsv, sizeof(cur->cmdcsv)) < 0) {
                    fprintf(stderr, "podvm build: line %d: CMD too long\n", op->line);
                    rc = 1;
                    goto out;
                }
            } else {
                /* shell form: docker stores ["sh","-c",raw] */
                char enc[BUILD_OP_STR * 2];
                if (kv_escape(enc, sizeof(enc), op->a) == (size_t)-1) enc[0] = '\0';
                snprintf(cur->cmdcsv, sizeof(cur->cmdcsv), "sh,-c,%s", enc);
            }
            break;
        case BENTRYPOINT:
            if (op->json_form) {
                if (csv_escape_words(op->a, cur->entrycsv, sizeof(cur->entrycsv)) < 0) {
                    fprintf(stderr, "podvm build: line %d: ENTRYPOINT too long\n",
                            op->line);
                    rc = 1;
                    goto out;
                }
            } else {
                char enc[BUILD_OP_STR * 2];
                if (kv_escape(enc, sizeof(enc), op->a) == (size_t)-1) enc[0] = '\0';
                snprintf(cur->entrycsv, sizeof(cur->entrycsv), "sh,-c,%s", enc);
            }
            break;
        case BCOPY: {
            char ldir[PATH_MAX], srcabs[PATH_MAX], dstabs[PATH_MAX], cerr[512];
            char fromdiffs[MAX_CHAIN][PATH_MAX];
            const char *chain[MAX_CHAIN];
            int nchain = 0;

            /* docker: a relative COPY dst is relative to WORKDIR */
            {
                int dw = op->b[0] == '/'
                    ? snprintf(dstabs, sizeof(dstabs), "%s", op->b)
                    : snprintf(dstabs, sizeof(dstabs), "%s/%s",
                               cur->cwd[0] ? cur->cwd : "/", op->b);
                /* A truncated COPY destination is the wrong path -- fail
                 * rather than copy into a mangled directory. */
                if (dw < 0 || (size_t)dw >= sizeof(dstabs)) {
                    fprintf(stderr, "podvm build: line %d: COPY destination "
                                    "path too long\n", op->line);
                    rc = 1;
                    goto out;
                }
            }

            if (op->from[0]) {
                /* --from=<stage|image>: src is a path INSIDE that
                 * image's merged rootfs, not a context path. */
                int found = -1;
                for (int s = 0; s < nstages; s++)
                    if (stages[s].name[0] && !strcmp(stages[s].name, op->from))
                        found = s;
                if (found >= 0) {
                    BuildStage *src = &stages[found];
                    for (int l = 0; l < src->nlayers && nchain < MAX_CHAIN; l++)
                        chain[nchain++] = src->layer_chain[l];
                    for (int b = 0; b < src->nbase && nchain < MAX_CHAIN; b++)
                        chain[nchain++] = src->base_diffs[b];
                } else {
                    char perr2[512];
                    if (image_is_path(op->from)) {
                        fprintf(stderr, "podvm build: line %d: COPY --from "
                                "must be a stage name or an image: '%s'\n",
                                op->line, op->from);
                        rc = 1;
                        goto out;
                    }
                    int c = resolve_image_chain(op->from, fromdiffs, NULL, NULL,
                                                perr2, sizeof(perr2));
                    if (c < 0) {
                        fprintf(stderr, "podvm build: line %d: COPY --from: %s\n",
                                op->line, perr2);
                        rc = 1;
                        goto out;
                    }
                    for (int l = 0; l < c && nchain < MAX_CHAIN; l++)
                        chain[nchain++] = fromdiffs[l];
                }
            } else if (op->a[0] == '/' || strstr(op->a, "..")) {
                fprintf(stderr, "podvm build: line %d: COPY src must be a "
                                "context-relative path without '..': '%s'\n",
                        op->line, op->a);
                rc = 1;
                goto out;
            }
            /* split the (possibly multi-) source list; dst is a
             * directory when there is more than one source */
            char srcbuf[BUILD_OP_STR];
            char *srcs[64];
            int nsrc = 0;
            snprintf(srcbuf, sizeof(srcbuf), "%s", op->a);
            for (char *tok = strtok(srcbuf, " \t"); tok && nsrc < 64;
                 tok = strtok(NULL, " \t"))
                srcs[nsrc++] = tok;
            if (!op->from[0]) {
                for (int s = 0; s < nsrc; s++) {
                    if (srcs[s][0] == '/' || strstr(srcs[s], "..")) {
                        fprintf(stderr, "podvm build: line %d: COPY src must "
                                "be a context-relative path without '..': '%s'\n",
                                op->line, srcs[s]);
                        rc = 1;
                        goto out;
                    }
                }
            }
            {
                int lw = snprintf(ldir, sizeof(ldir), "%s/c%03d", layersdir, i);
                /* A truncated layer dir is the wrong dir -- fail. */
                if (lw < 0 || (size_t)lw >= sizeof(ldir)) {
                    fprintf(stderr, "podvm build: layer dir path too long\n");
                    rc = 1;
                    goto out;
                }
            }
            if (mkdir_p(ldir, 0755) < 0) {
                fprintf(stderr, "podvm build: cannot create %s: %s\n",
                        ldir, strerror(errno));
                rc = 1;
                goto out;
            }
            if (op->from[0]) {
                for (int s = 0; s < nsrc; s++) {
                    char target[PATH_MAX];
                    copy_target(dstabs, srcs[s], nsrc > 1, target, sizeof(target));
                    if (build_copy_from_chain((const char *const *)chain, nchain,
                                              srcs[s], ldir, target,
                                              cerr, sizeof(cerr)) < 0) {
                        fprintf(stderr, "podvm build: line %d: %s\n", op->line, cerr);
                        rc = 1;
                        goto out;
                    }
                }
                printf("podvm: step %d: COPY --from=%s %s %s\n",
                       i, op->from, op->a, op->b);
            } else {
                for (int s = 0; s < nsrc; s++) {
                    char target[PATH_MAX];
                    copy_target(dstabs, srcs[s], nsrc > 1, target, sizeof(target));
                    snprintf(srcabs, sizeof(srcabs), "%s/%s", context, srcs[s]);
                    if (build_copy_path(srcabs, ldir, target, cerr, sizeof(cerr)) < 0) {
                        fprintf(stderr, "podvm build: line %d: %s\n", op->line, cerr);
                        rc = 1;
                        goto out;
                    }
                }
                printf("podvm: step %d: COPY %s %s\n", i, op->a, op->b);
            }
            {
                char dig[NATIVE_DIGEST_MAX], ndir[PATH_MAX];
                if (native_commit_diff(ldir, dig, sizeof(dig)) < 0) {
                    fprintf(stderr, "podvm build: cannot commit COPY layer\n");
                    rc = 1;
                    goto out;
                }
                native_diff_path(dig, ndir, sizeof(ndir));
                chain_insert_front(cur->layer_chain, &cur->nlayers, ndir);
                snprintf(cur->step_dig[cur->nstepdig],
                         sizeof(cur->step_dig[0]), "%s", dig);
                cur->nstepdig++;
            }
            break;
        }
        case BRUN: {
            char stepname[64], sdir[CONTAINER_PATH_MAX];
            char upath[PATH_MAX], spath[PATH_MAX];
            snprintf(stepname, sizeof(stepname), "pb%dp%d-s%d",
                     (int)getpid(), (int)getppid(), i);

            /* Step chain: committed layers (top-first), then the base. */
            store_nlayers = 0;
            for (int l = 0; l < cur->nlayers && store_nlayers < MAX_CHAIN; l++)
                snprintf(store_layers[store_nlayers++].diff, STORE_PATH_MAX,
                         "%s", cur->layer_chain[l]);
            for (int b = 0; b < cur->nbase && store_nlayers < MAX_CHAIN; b++)
                snprintf(store_layers[store_nlayers++].diff, STORE_PATH_MAX,
                         "%s", cur->base_diffs[b]);

            snprintf(config.name, sizeof(config.name), "%s", stepname);
            snprintf(config.rootfs_dir, sizeof(config.rootfs_dir),
                     "/tmp/podvm-build-ovl.%d.%d", (int)getpid(), i);
            snprintf(config.entrypoint, sizeof(config.entrypoint), "/bin/sh");
            snprintf(config.args, sizeof(config.args), "%s", "/.podvm-step.sh");
            snprintf(config.env, sizeof(config.env), "%s", cur->envlist);
            snprintf(config.cwd, sizeof(config.cwd), "%s", cur->cwd);
            snprintf(config.user, sizeof(config.user), "%s", cur->user);
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
            {
                int sw = snprintf(spath, sizeof(spath), "%s/.podvm-step.sh", upath);
                /* A truncated step-script path writes to the wrong file. */
                if (sw < 0 || (size_t)sw >= sizeof(spath)) {
                    fprintf(stderr, "podvm build: step script path too long\n");
                    rc = 1;
                    goto out;
                }
            }
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
                if (build_config_tokens(vcfg, sizeof(vcfg)) < 0) {
                    fprintf(stderr, "podvm: config too large for the vsock "
                                    "channel; step uses the cmdline\n");
                } else if (send_config_vsock(vcfg) < 0) {
                    fprintf(stderr, "podvm: vsock config failed; "
                                    "step uses the cmdline\n");
                }
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

            /* Commit: the step upper becomes a native layer (whiteouts
             * preserved as char 0:0 by the userns unpack). */
            {
                char dig[NATIVE_DIGEST_MAX], ndir[PATH_MAX];
                if (native_commit_diff(upath, dig, sizeof(dig)) < 0) {
                    fprintf(stderr, "podvm build: cannot commit the step layer\n");
                    container_rm_rf(sdir);
                    container_lock_unlink(stepname);
                    rc = 1;
                    goto out;
                }
                native_diff_path(dig, ndir, sizeof(ndir));
                chain_insert_front(cur->layer_chain, &cur->nlayers, ndir);
                snprintf(cur->step_dig[cur->nstepdig],
                         sizeof(cur->step_dig[0]), "%s", dig);
                cur->nstepdig++;
            }
            container_rm_rf(sdir);
            container_lock_unlink(stepname);
            break;
        }
        default:
            break;
        }
    }

    /* ── write the image manifest + config to the native store ── */
    {
        /* Manifest layers, bottom-first: base (bottom) then steps in
         * build order (top). */
        const char *aptr[MAX_CHAIN * 2];
        int nall = 0;
        for (int b = 0; b < cur->nbase_dig; b++)
            aptr[nall++] = cur->base_dig_bottom[b];
        for (int s = 0; s < cur->nstepdig; s++)
            aptr[nall++] = cur->step_dig[s];

        /* The OCI-shaped config blob the run path already reads. */
        char *cfgjson = NULL;
        size_t cfglen = 0;
        FILE *cf = open_memstream(&cfgjson, &cfglen);
        if (!cf) {
            fprintf(stderr, "podvm build: cannot build the config\n");
            rc = 1;
            goto out;
        }
        char escwd[STORE_PATH_MAX], escuser[300];
        json_escape(cur->cwd, escwd, sizeof(escwd));
        json_escape(cur->user, escuser, sizeof(escuser));
        fprintf(cf, "{\"architecture\":\"amd64\",\"os\":\"linux\",\"config\":{");
        json_csv_array(cf, "Env", cur->envlist);
        fprintf(cf, ",");
        json_csv_array(cf, "Entrypoint", cur->entrycsv);
        fprintf(cf, ",");
        json_csv_array(cf, "Cmd", cur->cmdcsv);
        fprintf(cf, ",\"WorkingDir\":\"%s\",\"User\":\"%s\"}", escwd, escuser);
        /* rootfs + history: podman/docker load validate against
         * rootfs.diff_ids (the layer digests, bottom-first). */
        fprintf(cf, ",\"rootfs\":{\"type\":\"layers\",\"diff_ids\":[");
        for (int i = 0; i < nall; i++)
            fprintf(cf, "%s\"%s\"", i ? "," : "", aptr[i]);
        fprintf(cf, "]}");
        fprintf(cf, ",\"history\":[");
        for (int i = 0; i < nall; i++)
            fprintf(cf, "%s{\"created_by\":\"layer %d\"}", i ? "," : "", i);
        fprintf(cf, "]}");
        fclose(cf);   /* flushes cfgjson/cfglen */

        char cfg_digest[NATIVE_DIGEST_MAX];
        if (native_blob_put(cfgjson, cfglen, cfg_digest, sizeof(cfg_digest)) < 0) {
            fprintf(stderr, "podvm build: cannot store the config blob\n");
            free(cfgjson);
            rc = 1;
            goto out;
        }
        free(cfgjson);

        if (native_manifest_put(tag, cfg_digest, aptr, nall, NULL, 0) < 0) {
            fprintf(stderr, "podvm build: cannot write the manifest for %s\n", tag);
            rc = 1;
            goto out;
        }

        printf("podvm: built %s (%d layers)\n", tag, nall);
    }

out:
    container_rm_rf(workdir);
    free(stages);
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


int main(int argc, char **argv) {

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
    if (argc > 1 && strcmp(argv[1], "save") == 0)
        return cmd_save(argc, argv);
    if (argc > 1 && strcmp(argv[1], "load") == 0)
        return cmd_load(argc, argv);
    if (argc > 1 && strcmp(argv[1], "system") == 0)
        return cmd_system(argc, argv);
    if (argc > 1 && strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);
    if (argc > 1 && strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    /* Run-argv parsing lives in cli.c (unit-tested; docker grammar:
     * `run` keyword consumed, flags stop at the image). */
    switch (cli_parse_run(argc, argv)) {
    case CLI_PARSE_HELP:
        return 0;
    case CLI_PARSE_ERROR:
        return 1;
    }
    const char *image_ref = cli_image_ref();   /* user's original ref */

    /* Resolve kernel path */
    if (config.kernel[0] == '\0') {
        get_host_kernel(config.kernel, sizeof(config.kernel));
        printf("podvm: using host kernel: %s\n", config.kernel);
    }
    
    /* Resolve rootfs/image */
    char resolved_rootfs[PATH_MAX];
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
            if (fill_native_chain(config.rootfs_dir) <= 0) {
                fprintf(stderr, "podvm: no native layer chain for %s\n",
                        config.rootfs_dir);
                return 1;
            }
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
         * podman, which already failed above). A pull now lands in the
         * layered store, so a native resolution stacks the chain. */
        if (!resolved && !want_podman) {
            int r = image_resolve(config.rootfs_dir, resolved_rootfs,
                                  sizeof(resolved_rootfs));
            if (r == IMAGE_RESOLVED_NATIVE) {
                if (fill_native_chain(config.rootfs_dir) <= 0) {
                    fprintf(stderr, "podvm: no native layer chain for %s\n",
                            config.rootfs_dir);
                    return 1;
                }
                used_native = 1;
                snprintf(resolved_rootfs, sizeof(resolved_rootfs),
                         "/tmp/podvm-rootfs.%d", getpid());
            } else if (r < 0) {
                fprintf(stderr, "podvm: failed to resolve image/path: %s\n",
                        config.rootfs_dir);
                return 1;
            }
        }
    }
    
    /* Copy resolved path back */
    strncpy(config.rootfs_dir, resolved_rootfs, PATH_MAX - 1);
    config.rootfs_dir[PATH_MAX - 1] = '\0';

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
                : store_image_config(image_ref, &ic);
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
        if (build_cmdline(cmdline, sizeof(cmdline)) < 0) {
            fprintf(stderr, "podvm: config too large for the kernel cmdline "
                            "(> %d bytes); detached runs have no vsock "
                            "fallback\n", MAX_CMDLINE);
            kill(qemu_pid, SIGTERM);
            return 1;
        }
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
        if (build_config_tokens(vsock_config, sizeof(vsock_config)) < 0) {
            fprintf(stderr, "podvm: config too large for the vsock channel; "
                            "guest will use the cmdline\n");
        } else {
            printf("podvm: sending config to guest over vsock\n");
            if (send_config_vsock(vsock_config) < 0) {
                fprintf(stderr, "podvm: vsock config failed; guest will use cmdline\n");
            }
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
