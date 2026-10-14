/*
 * cli.c - podvm run-command CLI: state + getopt front end
 *
 * Extracted from main.c so the parser is unit-testable directly
 * (tests/test_cli.c). See cli.h for the compat contract.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>

#include "cli.h"
#include "strparse.h"       /* kv_escape */

/* Run configuration, consumed by main.c (kernel/initramfs resolution,
 * cmdline build, QEMU argv) and cmd_build's RUN-step VMs. */
PodvmConfig config = {
    .kernel = "",       /* Empty = use host kernel */
    .initramfs = "",    /* Empty = auto-resolve (resolve_initramfs) */
    .memory = 128,
    .cpus = 1,
    .interactive = 0,
};

/* -p/--publish entries (tcp only; validated in publish_add). */
PodvmPublish publishes[MAX_PUBLISH];
int npublishes = 0;

/* -v/--volume bind mounts. */
PodvmVolume volumes[MAX_VOLUMES];
int nvolumes = 0;

/* --tmpfs scratch mounts (absolute container paths). */
char tmpfs_targets[MAX_TMPFS][CONTAINER_PATH_MAX];
int ntmpfs = 0;

/* The image/rootfs ref as typed; config.rootfs_dir is overwritten
 * with the resolved path later, so keep the original for config
 * lookup. */
static char image_ref_buf[PATH_MAX];

const char *cli_image_ref(void) {
    return image_ref_buf;
}

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

static void cli_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <image|rootfs-dir> [command] [args...]\n", prog);
    fprintf(stderr, "       %s run [options] <image|rootfs-dir> [command] [args...]\n", prog);
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
    fprintf(stderr, "  -r, --initramfs <path> Initramfs path (default: next to\n");
    fprintf(stderr, "                      the podvm binary, then /var/lib/podvm)\n");
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
    fprintf(stderr, "  save [-o F] IMAGE   Save an image as a docker-archive tar\n");
    fprintf(stderr, "  load [-i F]         Load a docker-archive tar\n");
    fprintf(stderr, "  system prune        Remove unreferenced blobs/layers from the store\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -i debian /bin/sh\n", prog);
    fprintf(stderr, "  %s -i debian echo hello\n", prog);
    fprintf(stderr, "  %s -i /path/to/rootfs /bin/sh\n", prog);
    fprintf(stderr, "  %s -n build -i alpine /bin/sh   (writes survive the run)\n", prog);
    fprintf(stderr, "  %s rm build                     (discard them)\n", prog);
}

enum { OPT_RM = 1000, OPT_ENVFILE = 1001, OPT_NETWORK = 1002,
       OPT_READONLY = 1003, OPT_TMPFS = 1004, OPT_HOSTNAME = 1005 };

/*
 * docker/pflag parity: boolean long flags accept `--flag=VALUE`
 * (strconv.ParseBool: 1,t,T,TRUE,true,True / 0,f,F,FALSE,false,False).
 * getopt_long rejects those outright (no_argument), so accept them
 * here. Returns 1 if the token was a valid bool `=value` form
 * (applied), 2 if it was a `=value` form that was rejected (message
 * printed), 0 if it is not a `=value` form at all (caller errors).
 * Post-loop validations (e.g. --detach rules) still apply.
 */
static int bool_long_with_value(const char *tok) {
    const char *eq = strchr(tok + 2, '=');
    if (!eq) return 0;

    size_t nlen = (size_t)(eq - (tok + 2));
    const char *v = eq + 1;
    int on;
    if (!strcmp(v, "1") || !strcmp(v, "t") || !strcmp(v, "T") ||
        !strcmp(v, "TRUE") || !strcmp(v, "true") || !strcmp(v, "True"))
        on = 1;
    else if (!strcmp(v, "0") || !strcmp(v, "f") || !strcmp(v, "F") ||
             !strcmp(v, "FALSE") || !strcmp(v, "false") || !strcmp(v, "False"))
        on = 0;
    else {
        fprintf(stderr, "podvm: invalid argument \"%s\" for \"--%.*s\" "
                        "flag: want true or false\n",
                v, (int)nlen, tok + 2);
        return 2;
    }

    static const struct { const char *name; int *field; } bools[] = {
        {"interactive", &config.interactive},
        {"detach",      &config.detach},
        {"rm",          &config.autoremove},
        {"read-only",   &config.read_only},
        {NULL, NULL}
    };
    for (int i = 0; bools[i].name; i++) {
        if (strlen(bools[i].name) == nlen &&
            !strncmp(tok + 2, bools[i].name, nlen)) {
            *bools[i].field = on;
            return 1;
        }
    }
    fprintf(stderr, "podvm: unknown option: --%.*s\n", (int)nlen, tok + 2);
    return 2;
}

int cli_parse_run(int argc, char **argv) {
    int opt;
    int option_index = 0;

    /* Fresh parse state: config and the flag lists restart empty on
     * every call (unit tests parse more than once per process). */
    memset(&config, 0, sizeof(config));
    snprintf(config.kernel, sizeof(config.kernel), "%s", "");
    snprintf(config.initramfs, sizeof(config.initramfs), "%s", "");
    config.memory = 128;
    config.cpus = 1;
    npublishes = nvolumes = ntmpfs = 0;
    image_ref_buf[0] = '\0';
    /* glibc: optind == 0 fully reinitializes getopt (incl. internal
     * state); 1 only resets the public counter. */
    optind = 0;

    /* Drop-in compat: docker/podman take an explicit `run` keyword.
     * Consume it (keyword wins, as in docker) so `podvm run --rm img`
     * parses identically to `podvm --rm img`. getopt's '+' would
     * otherwise stop at the non-option `run` and treat it as the
     * image. */
    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        for (int i = 1; i < argc; i++) argv[i] = argv[i + 1];
        argc--;
    }

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
        {"hostname",    required_argument, 0, OPT_HOSTNAME},
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
            if (env_add(optarg) < 0) return CLI_PARSE_ERROR;
            break;
        case OPT_ENVFILE:
            if (env_add_file(optarg) < 0) return CLI_PARSE_ERROR;
            break;
        case 'u':
            if (!optarg[0]) {
                fprintf(stderr, "podvm: --user must not be empty\n");
                return CLI_PARSE_ERROR;
            }
            snprintf(config.user, sizeof(config.user), "%s", optarg);
            break;
        case 'w':
            if (optarg[0] != '/') {
                fprintf(stderr, "podvm: --workdir must be an absolute path: '%s'\n",
                        optarg);
                return CLI_PARSE_ERROR;
            }
            snprintf(config.cwd, sizeof(config.cwd), "%s", optarg);
            break;
        case 'p':
            if (publish_add(optarg) < 0) return CLI_PARSE_ERROR;
            break;
        case OPT_NETWORK:
            if (!strcmp(optarg, "none")) {
                config.net_none = 1;
            } else if (!strcmp(optarg, "slirp")) {
                config.net_none = 0;
            } else {
                fprintf(stderr, "podvm: --network must be slirp or none "
                                "(host/bridge need tap; ADR-009/perf phase)\n");
                return CLI_PARSE_ERROR;
            }
            break;
        case OPT_READONLY:
            config.read_only = 1;
            break;
        case 'v':
            if (volume_add(optarg) < 0) return CLI_PARSE_ERROR;
            break;
        case OPT_TMPFS:
            if (tmpfs_add(optarg) < 0) return CLI_PARSE_ERROR;
            break;
        case OPT_HOSTNAME:
            if (!optarg[0]) {
                fprintf(stderr, "podvm: --hostname must not be empty\n");
                return CLI_PARSE_ERROR;
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
            snprintf(config.kernel, sizeof(config.kernel), "%s", optarg);
            break;
        case 'r':
            snprintf(config.initramfs, sizeof(config.initramfs), "%s", optarg);
            break;
        case 'n':
            if (!container_name_valid(optarg)) {
                fprintf(stderr, "podvm: invalid container name '%s' "
                                "(no '/', not '.' or '..', no leading '-')\n",
                        optarg);
                return CLI_PARSE_ERROR;
            }
            snprintf(config.name, sizeof(config.name), "%s", optarg);
            break;
        case OPT_RM:
            config.autoremove = 1;
            break;
        case 'h':
            cli_usage(argv[0]);
            return CLI_PARSE_HELP;
        case ':':
            fprintf(stderr, "podvm: option '-%c' requires an argument\n", optopt);
            cli_usage(argv[0]);
            return CLI_PARSE_ERROR;
        case '?':
        default: {
            /* Long-option tokens: try docker/pflag's bool `=value`
             * form (--rm=true), else name the actual token (optopt is
             * unreliable for long options: glibc sets it to the
             * option's val, which %c turns into garbage). */
            const char *tok = (optind > 0 && optind <= argc)
                              ? argv[optind - 1] : NULL;
            if (tok && tok[0] == '-' && tok[1] == '-') {
                int b = bool_long_with_value(tok);
                if (b == 1) break;
                if (b == 2) return CLI_PARSE_ERROR;
                fprintf(stderr, "podvm: unknown option: %s\n", tok);
            } else {
                fprintf(stderr, "podvm: invalid option -- '%c'\n", optopt);
            }
            cli_usage(argv[0]);
            return CLI_PARSE_ERROR;
        }
        }
    }

    /* Need at least image/rootfs and command */
    if (optind >= argc) {
        cli_usage(argv[0]);
        return CLI_PARSE_ERROR;
    }

    if (config.net_none && npublishes > 0) {
        fprintf(stderr, "podvm: -p/--publish requires networking "
                        "(--network=none disables it)\n");
        return CLI_PARSE_ERROR;
    }

    /* Detached runs are named and have no supervisor to clean up after
     * them. */
    if (config.detach) {
        if (!config.name[0]) {
            fprintf(stderr, "podvm: --detach requires --name\n");
            return CLI_PARSE_ERROR;
        }
        if (config.autoremove) {
            fprintf(stderr, "podvm: --detach cannot be combined with --rm "
                            "(nothing would clean the state up)\n");
            return CLI_PARSE_ERROR;
        }
        if (config.interactive) {
            fprintf(stderr, "podvm: --detach cannot be combined with "
                            "--interactive\n");
            return CLI_PARSE_ERROR;
        }
    }

    /* Get image or rootfs directory */
    snprintf(config.rootfs_dir, sizeof(config.rootfs_dir), "%s", argv[optind]);
    snprintf(image_ref_buf, sizeof(image_ref_buf), "%s", config.rootfs_dir);
    optind++;

    /* Get entrypoint (if provided) */
    if (optind < argc) {
        snprintf(config.entrypoint, sizeof(config.entrypoint), "%s", argv[optind]);
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
                return CLI_PARSE_ERROR;
            }
            p += n;
        }
        *p = '\0';
    } else {
        /* No command specified, use image entrypoint or default */
        config.entrypoint[0] = '\0';
    }

    return CLI_PARSE_OK;
}
