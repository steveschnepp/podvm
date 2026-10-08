/*
 * imgcfg.h - fold an image's config into a run spec
 *
 * The pure command/env/cwd/user resolution behind `podvm <image>`.
 * Split out of main.c so the docker-semantics matrix (image Entrypoint
 * vs Cmd vs a user-typed command) is table-testable without booting a
 * VM. See ADR-005 (an image carries its own default command).
 */

#ifndef IMGCFG_H
#define IMGCFG_H

#include "image.h"

/* Generous bounds; the wire escaping happens later and may grow these. */
#define IMGCFG_ARGS_MAX (MAX_PATH * 4)
#define IMGCFG_ENV_MAX  (MAX_PATH * 4)

/*
 * A resolved (or partially-resolved) run: what to exec, the environment,
 * working dir, and user. Strings are raw -- wire escaping is applied
 * later by build_config_tokens(), not here.
 */
typedef struct {
    char entrypoint[MAX_PATH];   /* program to exec */
    char args[IMGCFG_ARGS_MAX];  /* comma-joined args for the program */
    char env[IMGCFG_ENV_MAX];    /* comma-joined K=V pairs */
    char cwd[MAX_PATH];          /* working directory ("" if unset) */
    char user[128];              /* "uid:gid" ("" if unset) */
} RunSpec;

/*
 * Fold an image's config (ic) on top of the user's CLI-provided base
 * spec, honoring docker semantics:
 *
 *   base has no entrypoint  -> Entrypoint + Cmd from the image
 *   base has an entrypoint  -> image Entrypoint, then the base command
 *                              and its args (the user's command replaces
 *                              Cmd, but an image Entrypoint still leads)
 *
 * Env: base env, then the image's vars, then HOME ensured.
 * cwd/user: the base value wins if set, else the image's.
 *
 * Either argument may be NULL/zeroed; the result is always fully formed
 * (empty strings where nothing supplies a value). Caller owns nothing --
 * out is filled in place.
 */
void imgcfg_fold(const ImageConfig *ic, const RunSpec *base, RunSpec *out);

#endif /* IMGCFG_H */
