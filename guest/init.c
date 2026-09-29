/*
 * podvm guest init
 *
 * Minimal init for podvm guests.
 * Reads config from kernel cmdline, mounts virtiofs, chroots, execs.
 *
 * Build: musl-gcc -static -Os -o init init.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_ARGS  64
#define MAX_ENVS  128
#define PATH_SIZE 1024
#define CMDLINE_SIZE 4096

/* Configuration from kernel cmdline */
typedef struct {
    char root[PATH_SIZE];
    char entrypoint[PATH_SIZE];
    char args[MAX_ARGS][PATH_SIZE];
    int  argc;
    char envs[MAX_ENVS][PATH_SIZE];
    int  envc;
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

/* Find parameter value in cmdline */
static const char *find_param(const char *cmdline, const char *prefix) {
    const char *p = cmdline;
    size_t plen = strlen(prefix);

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (strncmp(p, prefix, plen) == 0)
            return p + plen;
        while (*p && *p != ' ') p++;
    }
    return NULL;
}

/* Parse comma-separated values */
static int parse_csv(const char *input, char output[][PATH_SIZE], int max) {
    int count = 0;
    const char *p = input;

    while (*p && count < max) {
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t len = p - start;
        if (len >= PATH_SIZE) len = PATH_SIZE - 1;
        memcpy(output[count], start, len);
        output[count][len] = '\0';
        count++;
        if (*p == ',') p++;
    }
    return count;
}

/* Parse uid:gid */
static void parse_user(const char *str, int *uid, int *gid) {
    *uid = *gid = 0;
    if (str) sscanf(str, "%d:%d", uid, gid);
}

/* Parse config from kernel cmdline */
static void parse_config(Config *cfg, const char *cmdline) {
    const char *val;

    memset(cfg, 0, sizeof(*cfg));

    val = find_param(cmdline, "podvm.root=");
    if (val) strncpy(cfg->root, val, PATH_SIZE - 1);

    val = find_param(cmdline, "podvm.entrypoint=");
    if (val) strncpy(cfg->entrypoint, val, PATH_SIZE - 1);

    val = find_param(cmdline, "podvm.args=");
    if (val) cfg->argc = parse_csv(val, cfg->args, MAX_ARGS);

    val = find_param(cmdline, "podvm.cwd=");
    if (val) strncpy(cfg->cwd, val, PATH_SIZE - 1);

    val = find_param(cmdline, "podvm.hostname=");
    if (val) strncpy(cfg->hostname, val, 255);

    val = find_param(cmdline, "podvm.user=");
    parse_user(val, &cfg->uid, &cfg->gid);
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
}

/* Mount container rootfs via virtiofs */
static int mount_rootfs(const Config *cfg) {
    mkdir_p(cfg->root, 0755);

    /* Try virtiofs first, fall back to 9p */
    if (mount(NULL, cfg->root, "virtiofs", 0, "tag=rootfs") == 0)
        return 0;

    if (mount(NULL, cfg->root, "9p", 0,
              "trans=virtio,version=9p2000.L,tag=rootfs") == 0)
        return 0;

    fprintf(stderr, "podvm-init: mount rootfs failed: %s\n", strerror(errno));
    return -1;
}

/* Build argv for execve */
static void build_argv(const Config *cfg, char **argv) {
    int i = 0;
    argv[i++] = cfg->entrypoint;
    for (int j = 0; j < cfg->argc && i < MAX_ARGS - 1; j++)
        argv[i++] = cfg->args[j];
    argv[i] = NULL;
}

/* Build envp for execve */
static void build_envp(const Config *cfg, char **envp) {
    int i = 0;

    for (int j = 0; j < cfg->envc && i < MAX_ENVS - 1; j++)
        envp[i++] = cfg->envs[j];

    /* Ensure PATH */
    int has_path = 0;
    for (int j = 0; j < i; j++)
        if (strncmp(envp[j], "PATH=", 5) == 0) { has_path = 1; break; }
    if (!has_path)
        envp[i++] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

    /* Ensure HOME */
    int has_home = 0;
    for (int j = 0; j < i; j++)
        if (strncmp(envp[j], "HOME=", 5) == 0) { has_home = 1; break; }
    if (!has_home)
        envp[i++] = "HOME=/root";

    envp[i] = NULL;
}

int main(void) {
    Config cfg;
    char cmdline[CMDLINE_SIZE];

    /* Step 1: Mount pseudo-filesystems */
    mount_pseudofs();

    /* Step 2: Read and parse kernel cmdline */
    if (read_cmdline(cmdline, sizeof(cmdline)) < 0) {
        fprintf(stderr, "podvm-init: cannot read /proc/cmdline\n");
        return 1;
    }
    parse_config(&cfg, cmdline);

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
    char *exec_envp[MAX_ENVS + 1];
    build_argv(&cfg, exec_argv);
    build_envp(&cfg, exec_envp);

    fprintf(stderr, "podvm-init: exec %s\n", cfg.entrypoint);
    execvp(cfg.entrypoint, exec_argv);

    fprintf(stderr, "podvm-init: exec failed: %s\n", strerror(errno));
    return 1;
}
