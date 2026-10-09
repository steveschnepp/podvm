/*
 * imgcfg.c - fold an image's config into a run spec
 *
 * Pure resolution logic, no I/O and no globals, so it can be driven by
 * table-driven tests. See imgcfg.h for the semantics.
 */

#include <stdio.h>
#include <string.h>

#include "imgcfg.h"
#include "strparse.h"

/* Escaping a field can double its size. */
#define IMGCFG_ESC_MAX (MAX_PATH * 2 + 16)

/* Append a (possibly already-escaped) field to the csv args. */
#define PUSH_TO_ARG(field) do { \
    size_t _have = strlen(argbuf); \
    int _w = snprintf(argbuf + _have, sizeof(argbuf) - _have, \
                      "%s%s", _have ? "," : "", (field)); \
    (void)_w; \
} while (0)

/*
 * Append one word from the IMAGE config. Image words are raw, so they
 * are escaped here -- the last place that knows which commas separate
 * words -- before joining the csv args. The entrypoint slot is a
 * single-value field escaped later by the token builder, so it must
 * NOT be escaped here.
 */
#define PUSH_WORD(w) do { \
    if (!have_entry) { \
        int _w = snprintf(entry, sizeof(entry), "%s", (w)); \
        if (_w >= 0 && (size_t)_w < sizeof(entry)) have_entry = 1; \
    } else { \
        char _esc[IMGCFG_ESC_MAX]; \
        if (kv_escape(_esc, sizeof(_esc), (w)) == (size_t)-1) _esc[0] = '\0'; \
        PUSH_TO_ARG(_esc); \
    } \
} while (0)

/*
 * Append an ALREADY-ESCAPED field (the user's config.args, escaped at
 * the argv join in main.c). Re-escaping would corrupt it.
 */
#define PUSH_FIELD(f) do { \
    if (!have_entry) { \
        int _w = snprintf(entry, sizeof(entry), "%s", (f)); \
        if (_w >= 0 && (size_t)_w < sizeof(entry)) have_entry = 1; \
    } else { \
        PUSH_TO_ARG(f); \
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
        /* user_args fields are ALREADY escaped (the argv join did it),
         * so split on *unescaped* commas and push each field as-is.
         * A plain strtok would cut fields in half at an escaped comma. */
        if (user_args) {
            const char *t = user_args;
            while (*t) {
                char field[IMGCFG_ARGS_MAX];
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
                if (*t == ',') t++;
                if (n) PUSH_FIELD(field);
            }
        }
    } else if (ic) {
        for (int i = 0; i < ic->ncmd; i++)
            PUSH_WORD(ic->cmd[i]);
    }

    if (have_entry) {
        snprintf(out->entrypoint, sizeof(out->entrypoint), "%s", entry);
        snprintf(out->args, sizeof(out->args), "%s", argbuf);
    }

    /* Env: the image's vars first, then the base (the user's CLI -e),
     * so a CLI key overrides the image's value for the same key (docker
     * semantics). The guest setenv's in list order, later wins, which is
     * what makes "image first, CLI last" the override. HOME is ensured
     * last: docker always provides one; the image may not. */
    if (ic) {
        for (int i = 0; i < ic->nenv; i++) {
            /* Image env values are raw (they came from the OCI config),
             * so escape the VALUE here; the key is copied verbatim.
             * Base (CLI) env arrives already escaped. */
            const char *eq = strchr(ic->env[i], '=');
            size_t klen = eq ? (size_t)(eq - ic->env[i]) : strlen(ic->env[i]);
            const char *val = eq ? eq + 1 : ic->env[i] + klen;
            char esc[IMGCFG_ESC_MAX];
            if (kv_escape(esc, sizeof(esc), val) == (size_t)-1) esc[0] = '\0';

            size_t have = strlen(out->env);
            int w = snprintf(out->env + have, sizeof(out->env) - have,
                             "%s%.*s=%s", have ? "," : "",
                             (int)klen, ic->env[i], esc);
            if (w < 0 || (size_t)w >= sizeof(out->env) - have) break;
        }
    }
    if (base && base->env[0]) {
        size_t have = strlen(out->env);
        int w = snprintf(out->env + have, sizeof(out->env) - have,
                         "%s%s", have ? "," : "", base->env);
        if (w < 0 || (size_t)w >= sizeof(out->env) - have) {
            /* keep what fits; the tail is truncated */
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
