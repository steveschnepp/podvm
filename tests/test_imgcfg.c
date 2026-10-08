/*
 * test_imgcfg.c - table-driven tests for the image-config fold.
 *
 * Exercises the docker-semantics matrix that used to live inline in
 * main.c and was only reachable by booting a VM:
 *   - image Entrypoint vs Cmd vs a user-typed command
 *   - env merging + HOME defaulting
 *   - cwd / user precedence (user CLI wins over image)
 *
 * Build: gcc -Wall -Wextra -O2 -g -o test_imgcfg test_imgcfg.c ../src/imgcfg.c -I../src
 */

#include <stdio.h>
#include <string.h>

#include "../src/imgcfg.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* Build an ImageConfig from C string arrays (NULL-terminated). */
static void mk_icfg(ImageConfig *ic, const char *entry[], const char *cmd[],
                    const char *env[], const char *cwd, const char *user) {
    memset(ic, 0, sizeof(*ic));
    for (int i = 0; entry && entry[i]; i++) ic->entrypoint[ic->nentry++] = (char *)entry[i];
    for (int i = 0; cmd && cmd[i]; i++)      ic->cmd[ic->ncmd++]         = (char *)cmd[i];
    for (int i = 0; env && env[i]; i++)      ic->env[ic->nenv++]         = (char *)env[i];
    if (cwd)   snprintf(ic->cwd, sizeof(ic->cwd), "%s", cwd);
    if (user)  snprintf(ic->user, sizeof(ic->user), "%s", user);
}

int main(void) {
    ImageConfig ic;
    RunSpec base, out;

    /* 1. No user command -> image Entrypoint + Cmd. */
    {
        const char *entry[] = {"/entry.sh", NULL};
        const char *cmd[]   = {"serve", "--port", "80", NULL};
        mk_icfg(&ic, entry, cmd, NULL, NULL, NULL);
        memset(&base, 0, sizeof(base));
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.entrypoint, "/entry.sh") == 0);
        CHECK(strcmp(out.args, "serve,--port,80") == 0);
    }

    /* 2. No entrypoint, no user command -> just Cmd. */
    {
        const char *cmd[] = {"/bin/sh", NULL};
        mk_icfg(&ic, NULL, cmd, NULL, NULL, NULL);
        memset(&base, 0, sizeof(base));
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.entrypoint, "/bin/sh") == 0);
        CHECK(out.args[0] == '\0');
    }

    /* 3. User command replaces Cmd; image Entrypoint still leads. */
    {
        const char *entry[] = {"/entry.sh", NULL};
        const char *cmd[]   = {"serve", NULL};
        mk_icfg(&ic, entry, cmd, NULL, NULL, NULL);
        memset(&base, 0, sizeof(base));
        snprintf(base.entrypoint, sizeof(base.entrypoint), "/bin/sh");
        snprintf(base.args, sizeof(base.args), "-c,echo hi");
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.entrypoint, "/entry.sh") == 0);
        CHECK(strcmp(out.args, "/bin/sh,-c,echo hi") == 0);
    }

    /* 4. User command, no image entrypoint -> user command is the program. */
    {
        const char *cmd[] = {"serve", NULL};
        mk_icfg(&ic, NULL, cmd, NULL, NULL, NULL);
        memset(&base, 0, sizeof(base));
        snprintf(base.entrypoint, sizeof(base.entrypoint), "/bin/echo");
        snprintf(base.args, sizeof(base.args), "a,b");
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.entrypoint, "/bin/echo") == 0);
        CHECK(strcmp(out.args, "a,b") == 0);
    }

    /* 4b. User command with NO args -> args must be empty, not "(null)".
     *     (A regression the e2e caught: snprintf("%s", NULL) rendered the
     *     literal string "(null)", which then became a real argument.) */
    {
        const char *cmd[] = {"serve", NULL};
        mk_icfg(&ic, NULL, cmd, NULL, NULL, NULL);
        memset(&base, 0, sizeof(base));
        snprintf(base.entrypoint, sizeof(base.entrypoint), "/bin/cat");
        /* base.args intentionally left empty */
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.entrypoint, "/bin/cat") == 0);
        CHECK(out.args[0] == '\0');
        CHECK(strstr(out.args, "(null)") == NULL);
    }

    /* 5. Env: image vars merged, HOME ensured when image omits it. */
    {
        const char *env[] = {"PATH=/bin", "FOO=bar", NULL};
        mk_icfg(&ic, NULL, NULL, env, NULL, NULL);
        memset(&base, 0, sizeof(base));
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.env, "PATH=/bin,FOO=bar,HOME=/root") == 0);
    }

    /* 6. Env: image already sets HOME -> not duplicated. */
    {
        const char *env[] = {"HOME=/home/app", "PATH=/bin", NULL};
        mk_icfg(&ic, NULL, NULL, env, NULL, NULL);
        memset(&base, 0, sizeof(base));
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.env, "HOME=/home/app,PATH=/bin") == 0);
    }

    /* 7. Env: no image env at all -> just HOME. */
    {
        mk_icfg(&ic, NULL, NULL, NULL, NULL, NULL);
        memset(&base, 0, sizeof(base));
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.env, "HOME=/root") == 0);
    }

    /* 8. cwd/user: image supplies when base has none. */
    {
        mk_icfg(&ic, NULL, NULL, NULL, "/app", "1000:1000");
        memset(&base, 0, sizeof(base));
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.cwd, "/app") == 0);
        CHECK(strcmp(out.user, "1000:1000") == 0);
    }

    /* 9. cwd/user: base (user CLI) wins over image. */
    {
        mk_icfg(&ic, NULL, NULL, NULL, "/app", "1000:1000");
        memset(&base, 0, sizeof(base));
        snprintf(base.cwd, sizeof(base.cwd), "/override");
        snprintf(base.user, sizeof(base.user), "0:0");
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.cwd, "/override") == 0);
        CHECK(strcmp(out.user, "0:0") == 0);
    }

    /* 10. NULL image config (bare rootfs) -> base command passes through,
     *     HOME still ensured. */
    {
        memset(&base, 0, sizeof(base));
        snprintf(base.entrypoint, sizeof(base.entrypoint), "/bin/true");
        imgcfg_fold(NULL, &base, &out);
        CHECK(strcmp(out.entrypoint, "/bin/true") == 0);
        CHECK(strcmp(out.env, "HOME=/root") == 0);
    }

    /* 11. NULL base (no CLI command) + empty image -> nothing to run. */
    {
        mk_icfg(&ic, NULL, NULL, NULL, NULL, NULL);
        imgcfg_fold(&ic, NULL, &out);
        CHECK(out.entrypoint[0] == '\0');
    }

    /* 12. Image Entrypoint with no Cmd, and no user command. */
    {
        const char *entry[] = {"/only-entry", NULL};
        mk_icfg(&ic, entry, NULL, NULL, NULL, NULL);
        memset(&base, 0, sizeof(base));
        imgcfg_fold(&ic, &base, &out);
        CHECK(strcmp(out.entrypoint, "/only-entry") == 0);
        CHECK(out.args[0] == '\0');
    }

    if (failures == 0) {
        printf("imgcfg tests: all passed\n");
        return 0;
    }
    printf("imgcfg tests: %d failure(s)\n", failures);
    return 1;
}
