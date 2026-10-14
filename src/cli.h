/*
 * cli.h - podvm run-command CLI: parse state and the getopt front end
 *
 * The run grammar is docker/podman's: `podvm [run] [FLAGS] IMAGE
 * [CMD [ARGS...]]`. Drop-in compat rules pinned here (and by
 * tests/test_cli.c):
 *   - the explicit `run` keyword is consumed (keyword-wins, docker
 *     semantics);
 *   - flags stop at the first non-option (getopt '+'), so anything
 *     after IMAGE belongs to the container's command;
 *   - boolean long flags accept docker/pflag's `=true|=false` form.
 *
 * cli_parse_run() may shift argv pointers (the `run` keyword); it
 * resets the parse state first, so it is safe to call more than once
 * per process (unit tests do).
 */
#ifndef PODVM_CLI_H
#define PODVM_CLI_H

#include <stddef.h>
#include <limits.h>      /* PATH_MAX */
#include "container.h"      /* CONTAINER_PATH_MAX */

#define MAX_PUBLISH 16
#define MAX_VOLUMES 8
#define MAX_TMPFS   8

/* Run configuration: filled by the CLI (flags), then folded with the
 * image config (env/cwd/user/entrypoint) before the VM starts. */
typedef struct {
    char kernel[PATH_MAX];
    char initramfs[PATH_MAX];
    char rootfs_dir[PATH_MAX];
    char entrypoint[PATH_MAX];
    char args[4096];       /* csv of escaped argv elements (guest tok[] matches) */
    char env[4096];         /* escaped csv of K=V (guest tok[] matches) */
    char cwd[PATH_MAX];       /* from image WorkingDir, or user override */
    char user[128];           /* from image User ("uid:gid") */
    char name[256];           /* container name; enables persistent state */
    char hostname[256];       /* --hostname; sent as podvm.hostname= */
    int  net_none;      /* --network=none: no netdev, guest skips eth0 */
    int  read_only;     /* --read-only: serve the lower ro, no upper */
    int  detach;        /* -d: background the VM; serial goes to <state>/log */
    int  memory;       /* MB */
    int  cpus;
    int  interactive;  /* -i flag */
    int  tty;          /* -t/--tty: no-op today -- the serial console IS
                        * a real guest tty (see cli.c); kept so `-it`
                        * parses like docker. A vsock PTY channel would
                        * make it meaningful (resize, clean stream). */
    int  autoremove;   /* --rm: drop the named state dir when the run ends */
} PodvmConfig;

/* -p/--publish entries (tcp only; validated at parse time). */
typedef struct {
    char hostip[64];
    int  hostport;
    int  containerport;
} PodvmPublish;

/* -v/--volume bind mounts, each served to the guest over its own 9p
 * tag (vol0, vol1, ...) because the rootfs tag is taken. */
typedef struct {
    char host[CONTAINER_PATH_MAX];     /* host dir (exists at parse time) */
    char target[CONTAINER_PATH_MAX];   /* absolute path inside the container */
    int  ro;
} PodvmVolume;

extern PodvmConfig config;
extern PodvmPublish publishes[MAX_PUBLISH];
extern int npublishes;
extern PodvmVolume volumes[MAX_VOLUMES];
extern int nvolumes;
extern char tmpfs_targets[MAX_TMPFS][CONTAINER_PATH_MAX];
extern int ntmpfs;

/* cli_parse_run() outcomes */
enum {
    CLI_PARSE_OK    = 0,    /* config filled; proceed with the run */
    CLI_PARSE_HELP  = 1,    /* usage printed; exit 0 */
    CLI_PARSE_ERROR = 2     /* message printed; exit 1 */
};

/*
 * Parse `podvm [run] [FLAGS] IMAGE [CMD [ARGS...]]` into `config`.
 * Returns CLI_PARSE_*; on OK the image ref the user typed is available
 * via cli_image_ref() (config.rootfs_dir may be overwritten later with
 * the resolved path).
 */
int cli_parse_run(int argc, char **argv);

/* The image/rootfs ref as typed (valid until the next cli_parse_run). */
const char *cli_image_ref(void);

/*
 * Parse `podvm build [FLAGS] [context]` (docker build grammar) into
 * `config` (which -r/-k/-m/-c share with run) plus the out-params.
 *
 * Flags may appear before OR after the single positional context (the
 * hand-rolled loop podvm has always used -- not getopt's stop-at-first-
 * non-option), so `podvm build ctx -t tag` parses like docker's. Exactly
 * one context; -t is required. Returns CLI_PARSE_OK (out-params filled),
 * CLI_PARSE_HELP (usage printed), or CLI_PARSE_ERROR (message printed).
 */
typedef struct {
    const char *dockerfile;   /* -f, or NULL -> "<context>/Dockerfile" */
    const char *tag;          /* -t, required */
    const char *context;      /* positional, or NULL -> "." */
} PodvmBuildArgs;

int cli_parse_build(int argc, char **argv, PodvmBuildArgs *out);

#endif /* PODVM_CLI_H */
