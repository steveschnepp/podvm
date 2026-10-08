/*
 * imgcfg.c - fold an image's config into a run spec
 *
 * Pure resolution logic, no I/O and no globals, so it can be driven by
 * table-driven tests. See imgcfg.h for the semantics.
 */

#include <stdio.h>
#include <string.h>

#include "imgcfg.h"

/* Append one word as either the entrypoint (if not yet set) or a
 * comma-joined arg. */
#define PUSH_WORD(w) do { \
    if (!have_entry) { \
        int _w = snprintf(entry, sizeof(entry), "%s", (w)); \
        if (_w >= 0 && (size_t)_w < sizeof(entry)) have_entry = 1; \
    } else { \
        size_t _have = strlen(argbuf); \
        int _w = snprintf(argbuf + _have, sizeof(argbuf) - _have, \
                          "%s%s", _have ? "," : "", (w)); \
        (void)_w; \
    } \
} while (0)

void imgcfg_fold(const ImageConfig *ic, const RunSpec *base, RunSpec *out) {
    char entry[MAX_PATH];
    char argbuf[IMGCFG_ARGS_MAX];
    int have_entry = 0;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    entry[0] = '\0';
    argbuf[0] = '\0';

    /* Copy any user-supplied base values first (empty when base is NULL). */
    const char *user_cmd  = (base && base->entrypoint[0]) ? base->entrypoint : NULL;
    const char *user_args = (base && base->args[0]) ? base->args : NULL;

    /* An image Entrypoint always leads, whether or not the user typed a
     * command (docker semantics). */
    if (ic)
        for (int i = 0; i < ic->nentry; i++)
            PUSH_WORD(ic->entrypoint[i]);

    if (user_cmd) {
        /* User named a command: it replaces Cmd, after any Entrypoint. */
        PUSH_WORD(user_cmd);
        /* user_args is comma-joined; re-push each element. Only split
         * when args were actually given -- snprintf("%s", NULL) would
         * render the literal "(null)" and pass it as an argument. */
        if (user_args) {
            char tmp[IMGCFG_ARGS_MAX];
            snprintf(tmp, sizeof(tmp), "%s", user_args);
            for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
                PUSH_WORD(tok);
        }
    } else if (ic) {
        for (int i = 0; i < ic->ncmd; i++)
            PUSH_WORD(ic->cmd[i]);
    }

    if (have_entry) {
        snprintf(out->entrypoint, sizeof(out->entrypoint), "%s", entry);
        snprintf(out->args, sizeof(out->args), "%s", argbuf);
    }

    /* Env: base env, then the image's vars, then HOME ensured (docker
     * always provides one; the image may not). */
    if (base && base->env[0])
        snprintf(out->env, sizeof(out->env), "%s", base->env);
    if (ic) {
        for (int i = 0; i < ic->nenv; i++) {
            size_t have = strlen(out->env);
            int w = snprintf(out->env + have, sizeof(out->env) - have,
                             "%s%s", have ? "," : "", ic->env[i]);
            if (w < 0 || (size_t)w >= sizeof(out->env) - have) break;
        }
    }
    if (!strstr(out->env, "HOME=")) {
        size_t have = strlen(out->env);
        snprintf(out->env + have, sizeof(out->env) - have,
                 "%sHOME=/root", have ? "," : "");
    }

    /* cwd / user: the base (user CLI) wins if set, else the image's. */
    if (base && base->cwd[0])
        snprintf(out->cwd, sizeof(out->cwd), "%s", base->cwd);
    else if (ic && ic->cwd[0])
        snprintf(out->cwd, sizeof(out->cwd), "%s", ic->cwd);

    if (base && base->user[0])
        snprintf(out->user, sizeof(out->user), "%s", base->user);
    else if (ic && ic->user[0])
        snprintf(out->user, sizeof(out->user), "%s", ic->user);
}
