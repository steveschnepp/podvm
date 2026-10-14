/*
 * test_cli.c - podvm run-CLI parser tests
 *
 * Links the real src/cli.c (no mock: cli_matrix.sh's history with a
 * drifted mock parser is the reason). Pins the docker/podman compat
 * contract:
 *   - `run` keyword consumed (keyword-wins)
 *   - attached/short-grouped/=value flag forms
 *   - flags stop at the image; everything after it is the command
 *   - pflag-style `--bool=VALUE` on long boolean flags
 *   - cross-flag validations (--detach rules, --network=none vs -p)
 *
 * Build: gcc -o test_cli tests/test_cli.c src/cli.c src/container.c src/strparse.c -Isrc
 * Run: ./test_cli
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include "cli.h"

static int failures = 0;

/* Most cases here are expected parse ERRORS, and the parser prints
 * diagnostics + usage to stderr. Park stderr on /dev/null for the run
 * so the report is just the CHECK results. */
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stdout, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); \
        failures++; \
    } \
} while (0)

/* cli_parse_run() may shift argv (the `run` keyword), so hand it a
 * writable copy -- argv[0] must be a real string (usage() prints it). */
static int parse(char **argv) {
    int argc = 0;
    while (argv[argc]) argc++;
    return cli_parse_run(argc, argv);
}

#define PARSE(...) parse((char *[]){ __VA_ARGS__, NULL })

/* ---- the `run` keyword ---- */

static void test_run_keyword(void) {
    int rc = PARSE("podvm", "run", "--rm", "alpine", "/bin/sh");
    CHECK(rc == CLI_PARSE_OK, "run keyword: rc=%d", rc);
    CHECK(config.autoremove == 1, "--rm after `run` not applied");
    CHECK(!strcmp(config.rootfs_dir, "alpine"), "image: %s", config.rootfs_dir);
    CHECK(!strcmp(config.entrypoint, "/bin/sh"), "entrypoint: %s", config.entrypoint);
    CHECK(!strcmp(cli_image_ref(), "alpine"), "image_ref: %s", cli_image_ref());

    /* identical without the keyword */
    rc = PARSE("podvm", "--rm", "alpine", "/bin/sh");
    CHECK(rc == CLI_PARSE_OK && config.autoremove == 1,
          "keyword-less form differs from `run` form");
}

/* ---- docker flag-form grammar ---- */

static void test_flag_forms(void) {
    /* attached short value */
    int rc = PARSE("podvm", "-eFOO=bar", "img");
    CHECK(rc == CLI_PARSE_OK && !strcmp(config.env, "FOO=bar"),
          "-eFOO=bar env: '%s'", config.env);

    /* --opt=value */
    rc = PARSE("podvm", "--env=FOO=bar", "img");
    CHECK(rc == CLI_PARSE_OK && !strcmp(config.env, "FOO=bar"),
          "--env=FOO=bar env: '%s'", config.env);

    /* attached to a numeric short */
    rc = PARSE("podvm", "-m512", "img");
    CHECK(rc == CLI_PARSE_OK && config.memory == 512, "-m512: %d", config.memory);

    /* grouped shorts: -im 512 == -i -m 512 */
    rc = PARSE("podvm", "-im", "512", "img");
    CHECK(rc == CLI_PARSE_OK && config.interactive == 1 && config.memory == 512,
          "-im 512: interactive=%d memory=%d", config.interactive, config.memory);

    /* multi -e accumulates, csv-joined */
    rc = PARSE("podvm", "-e", "A=1", "-e", "B=2", "img");
    CHECK(rc == CLI_PARSE_OK && !strcmp(config.env, "A=1,B=2"),
          "multi -e: '%s'", config.env);

    /* env value with a comma is escaped on the wire */
    rc = PARSE("podvm", "-e", "X=a,b", "img");
    CHECK(rc == CLI_PARSE_OK && !strcmp(config.env, "X=a\\,b"),
          "comma in env value: '%s'", config.env);

    /* long-only flags */
    rc = PARSE("podvm", "--hostname", "build-host", "--read-only",
               "--network=none", "img");
    CHECK(rc == CLI_PARSE_OK && !strcmp(config.hostname, "build-host") &&
          config.read_only == 1 && config.net_none == 1,
          "long-only flags: host=%s ro=%d net_none=%d",
          config.hostname, config.read_only, config.net_none);

    /* -t/--tty: docker compat; the serial console is already a tty,
     * so this is parse-level acceptance, not an allocation. */
    rc = PARSE("podvm", "-it", "img");
    CHECK(rc == CLI_PARSE_OK && config.interactive == 1 && config.tty == 1,
          "-it: inter=%d tty=%d", config.interactive, config.tty);
    rc = PARSE("podvm", "--tty=true", "img");
    CHECK(rc == CLI_PARSE_OK && config.tty == 1, "--tty=true");
    rc = PARSE("podvm", "-t", "img");
    CHECK(rc == CLI_PARSE_OK && config.tty == 1 && config.interactive == 0,
          "-t alone (docker: tty without exclusive stdin)");
}

/* ---- pflag bool `=VALUE` form (docker accepts it) ---- */

static void test_bool_eq_value(void) {
    int rc = PARSE("podvm", "--rm=true", "img");
    CHECK(rc == CLI_PARSE_OK && config.autoremove == 1,
          "--rm=true: rc=%d autoremove=%d", rc, config.autoremove);

    rc = PARSE("podvm", "--rm=1", "img");
    CHECK(rc == CLI_PARSE_OK && config.autoremove == 1, "--rm=1");

    rc = PARSE("podvm", "--rm=False", "img");
    CHECK(rc == CLI_PARSE_OK && config.autoremove == 0, "--rm=False");

    rc = PARSE("podvm", "--read-only=false", "img");
    CHECK(rc == CLI_PARSE_OK && config.read_only == 0, "--read-only=false");

    /* pflag names the flag and value in the error */
    rc = PARSE("podvm", "--rm=banana", "img");
    CHECK(rc == CLI_PARSE_ERROR, "--rm=banana accepted");

    rc = PARSE("podvm", "--tty=banana", "img");
    CHECK(rc == CLI_PARSE_ERROR, "--tty=banana accepted");
}

/* ---- flags stop at the image (docker: args after image = command) ---- */

static void test_post_image_passthrough(void) {
    int rc = PARSE("podvm", "img", "--rm");
    CHECK(rc == CLI_PARSE_OK, "post-image --rm: rc=%d", rc);
    CHECK(config.autoremove == 0, "--rm after image was parsed as a flag");
    CHECK(!strcmp(config.entrypoint, "--rm"),
          "post-image --rm not passed as command: %s", config.entrypoint);

    /* the full docker idiom: container flags never leak. Note args
     * are kv-escaped for the wire: spaces become \-escaped. */
    rc = PARSE("podvm", "img", "/bin/sh", "-c", "echo hi");
    CHECK(rc == CLI_PARSE_OK &&
          !strcmp(config.entrypoint, "/bin/sh") &&
          !strcmp(config.args, "-c,echo\\ hi"),
          "command args: entry=%s args=%s", config.entrypoint, config.args);

    /* `--` also ends podvm's flags */
    rc = PARSE("podvm", "-i", "--", "img", "-e");
    CHECK(rc == CLI_PARSE_OK && config.interactive == 1 &&
          !strcmp(config.entrypoint, "-e"),
          "-- handling: inter=%d entry=%s", config.interactive, config.entrypoint);
}

/* ---- cross-flag validations ---- */

static void test_validations(void) {
    CHECK(PARSE("podvm", "--detach", "img") == CLI_PARSE_ERROR,
          "--detach without --name accepted");
    CHECK(PARSE("podvm", "--detach", "-n", "c1", "--rm", "img") == CLI_PARSE_ERROR,
          "--detach --rm accepted");
    CHECK(PARSE("podvm", "--detach", "-n", "c1", "-i", "img") == CLI_PARSE_ERROR,
          "--detach -i accepted");
    CHECK(PARSE("podvm", "--detach", "-n", "c1", "img") == CLI_PARSE_OK,
          "--detach --name rejected");

    CHECK(PARSE("podvm", "--network=none", "-p", "8080:80", "img") == CLI_PARSE_ERROR,
          "-p with --network=none accepted");
    CHECK(PARSE("podvm", "-e", "NOEQUALS", "img") == CLI_PARSE_ERROR,
          "-e without '=' accepted");
    CHECK(PARSE("podvm", "-u", "", "img") == CLI_PARSE_ERROR,
          "empty --user accepted");
    CHECK(PARSE("podvm", "-w", "relative/dir", "img") == CLI_PARSE_ERROR,
          "relative --workdir accepted");
    CHECK(PARSE("podvm", "-n", "bad/name", "img") == CLI_PARSE_ERROR,
          "'/' in container name accepted");
    CHECK(PARSE("podvm", "--hostname", "", "img") == CLI_PARSE_ERROR,
          "empty --hostname accepted");
    CHECK(PARSE("podvm", "--tmpfs", "relative", "img") == CLI_PARSE_ERROR,
          "relative --tmpfs accepted");
    CHECK(PARSE("podvm", "-v", "rel:/mnt", "img") == CLI_PARSE_ERROR,
          "relative -v host path accepted");
    CHECK(PARSE("podvm") == CLI_PARSE_ERROR, "no arguments accepted");
    CHECK(PARSE("podvm", "-h") == CLI_PARSE_HELP, "-h not HELP");

    /* -p spec validation */
    CHECK(PARSE("podvm", "-p", "garbage", "img") == CLI_PARSE_ERROR, "-p garbage");
    CHECK(PARSE("podvm", "-p", "70000:80", "img") == CLI_PARSE_ERROR, "-p port>65535");
    int rc = PARSE("podvm", "-p", "127.0.0.1:8080:80", "img");
    CHECK(rc == CLI_PARSE_OK && npublishes == 1 &&
          !strcmp(publishes[0].hostip, "127.0.0.1") &&
          publishes[0].hostport == 8080 && publishes[0].containerport == 80,
          "-p full spec: ip=%s hp=%d cp=%d",
          publishes[0].hostip, publishes[0].hostport, publishes[0].containerport);
}

/* ---- state resets between parses (cli_parse_run is re-callable) ---- */

static void test_state_reset(void) {
    CHECK(PARSE("podvm", "-e", "A=1", "-p", "8080:80", "-n", "x", "img")
              == CLI_PARSE_OK, "setup parse");
    int rc = PARSE("podvm", "img2");
    CHECK(rc == CLI_PARSE_OK, "second parse rc=%d", rc);
    CHECK(npublishes == 0, "publishes leaked across parses: %d", npublishes);
    CHECK(config.env[0] == '\0', "env leaked: '%s'", config.env);
    CHECK(config.name[0] == '\0', "name leaked: '%s'", config.name);
    CHECK(!strcmp(config.rootfs_dir, "img2"), "rootfs: %s", config.rootfs_dir);
    CHECK(config.memory == 128 && config.cpus == 1, "defaults not restored");
}

/* ---- build grammar (cli_parse_build) ----
 *
 * Pins the docker-build flag surface now that it lives in cli.c. Covers
 * the fixes: an honest "requires an argument" (was "unknown option '-t'")
 * and positive-integer validation for -m/-c (was a silent atoi->0). */
static int parse_build(char **argv, PodvmBuildArgs *out) {
    int argc = 0;
    while (argv[argc]) argc++;
    return cli_parse_build(argc, argv, out);
}

#define PBUILD(...) parse_build((char *[]){ __VA_ARGS__, NULL }, &b)

static void test_build_flags(void) {
    PodvmBuildArgs b;
    int rc;

    /* required -t, default context "." */
    rc = PBUILD("podvm", "build", "-t", "name:tag");
    CHECK(rc == CLI_PARSE_OK, "build -t: rc=%d", rc);
    CHECK(!strcmp(b.tag, "name:tag"), "build tag: %s", b.tag);
    CHECK(!strcmp(b.context, "."), "build default context: %s", b.context);
    CHECK(b.dockerfile == NULL, "build -f default: %s", b.dockerfile);

    /* -f + explicit context */
    rc = PBUILD("podvm", "build", "-f", "DF", "-t", "n:t", "ctx");
    CHECK(rc == CLI_PARSE_OK && !strcmp(b.dockerfile, "DF") &&
          !strcmp(b.context, "ctx"), "build -f ctx: df=%s ctx=%s",
          b.dockerfile ? b.dockerfile : "(null)", b.context);

    /* flags AFTER the context still parse (docker grammar; not getopt '+') */
    rc = PBUILD("podvm", "build", "ctx", "-t", "n:t");
    CHECK(rc == CLI_PARSE_OK && !strcmp(b.context, "ctx") &&
          !strcmp(b.tag, "n:t"), "build flags-after-context: ctx=%s tag=%s",
          b.context, b.tag);

    /* -m/-c reach config as positive ints */
    rc = PBUILD("podvm", "build", "-t", "n:t", "-m", "512", "-c", "2");
    CHECK(rc == CLI_PARSE_OK && config.memory == 512 && config.cpus == 2,
          "build -m/-c: mem=%d cpus=%d", config.memory, config.cpus);

    /* missing -t is an error */
    rc = PBUILD("podvm", "build", "ctx");
    CHECK(rc == CLI_PARSE_ERROR, "build without -t: rc=%d", rc);

    /* MISSING ARGUMENT is honest (was: "unknown option '-t'") */
    rc = PBUILD("podvm", "build", "-t");
    CHECK(rc == CLI_PARSE_ERROR, "build -t at end: rc=%d", rc);
    rc = PBUILD("podvm", "build", "-f");
    CHECK(rc == CLI_PARSE_ERROR, "build -f at end: rc=%d", rc);

    /* garbage / non-positive -m and -c are rejected, not atoi'd to 0 */
    rc = PBUILD("podvm", "build", "-t", "n:t", "-m", "banana");
    CHECK(rc == CLI_PARSE_ERROR, "build -m banana: rc=%d", rc);
    rc = PBUILD("podvm", "build", "-t", "n:t", "-c", "0");
    CHECK(rc == CLI_PARSE_ERROR, "build -c 0: rc=%d", rc);

    /* unknown option and a second positional are errors */
    rc = PBUILD("podvm", "build", "--frobnicate", "-t", "n:t");
    CHECK(rc == CLI_PARSE_ERROR, "build unknown opt: rc=%d", rc);
    rc = PBUILD("podvm", "build", "-t", "n:t", "ctx", "extra");
    CHECK(rc == CLI_PARSE_ERROR, "build 2nd positional: rc=%d", rc);

    /* -h is HELP, not an error */
    rc = PBUILD("podvm", "build", "-h");
    CHECK(rc == CLI_PARSE_HELP, "build -h: rc=%d", rc);
}

int main(void) {
    int saved_stderr = dup(2);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, 2);

    test_run_keyword();
    test_flag_forms();
    test_bool_eq_value();
    test_post_image_passthrough();
    test_validations();
    test_state_reset();
    test_build_flags();

    if (saved_stderr >= 0) dup2(saved_stderr, 2);

    if (failures) {
        printf("test_cli: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_cli: all checks passed\n");
    return 0;
}
