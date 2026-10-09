/*
 * build.h - Dockerfile parsing + build helpers (podvm build)
 *
 * The parser is pure (text in, BuildOp array out) so it can be
 * table-tested; the driver that turns ops into VM steps lives in
 * main.c, which owns the run machinery.
 */
#ifndef BUILD_H
#define BUILD_H

#include <stddef.h>

#define BUILD_OPS_MAX 256
#define BUILD_OP_STR  1024

typedef enum {
    BFROM, BCOPY, BRUN, BWORKDIR, BENV, BUSER, BCMD, BENTRYPOINT
} BuildOpKind;

/*
 * One instruction.
 *   BFROM:       a = image ref
 *   BRUN:        a = shell command
 *   BCOPY:       a = src (context-relative), b = dst (container path)
 *   BWORKDIR:    a = absolute dir
 *   BENV:        a = "K=V" pairs, space separated (later wins)
 *   BUSER:       a = user spec
 *   BCMD/BENTRYPOINT: a = argv joined by ','; json_form = 1 when the
 *                Dockerfile used the exec form, 0 for shell form (a is
 *                then the raw command line, to be run via sh -c).
 */
typedef struct {
    BuildOpKind kind;
    int   line;                       /* 1-based, for error messages */
    char  a[BUILD_OP_STR];
    char  b[BUILD_OP_STR];
    int   json_form;
} BuildOp;

/*
 * Parse a Dockerfile. Returns the op count, or -1 with a message in
 * err. Supported: FROM, RUN (shell form), COPY, WORKDIR, ENV, USER,
 * CMD, ENTRYPOINT (both forms), and EXPOSE/LABEL (accepted, ignored --
 * ports and labels are runtime concerns for podvm). Everything else is
 * rejected with its line number. Line continuations ('\'), blank lines,
 * and '#' comments are handled. FROM must be the first instruction.
 */
int dockerfile_parse(const char *text, BuildOp *ops, int max,
                     char *err, size_t errsz);

/*
 * Copy src (a resolved absolute path; the DRIVER validates that the
 * original was context-relative) into a layer dir at container path
 * target (leading '/' is relative to the layer root). A directory
 * src's CONTENTS go into target (the `COPY app/ /app` shape); a file
 * src becomes exactly target. Symlinks are recreated; special files
 * are skipped with a warning on stderr. Rejects '..' and absolute
 * target. Returns 0 on success, -1 with a message in err.
 */
int build_copy_path(const char *src, const char *layer_root,
                    const char *target, char *err, size_t errsz);

/*
 * Append K=V to a comma-joined env list, replacing an existing entry
 * for the same key (docker: a later ENV wins). Returns 0, or -1 when
 * the list would overflow.
 */
int build_env_set(char *envlist, size_t n, const char *kv);

#endif /* BUILD_H */
