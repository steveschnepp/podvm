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
#define BUILD_STAGES_MAX 16

typedef enum {
    BFROM, BCOPY, BRUN, BWORKDIR, BENV, BUSER, BCMD, BENTRYPOINT
} BuildOpKind;

/*
 * One instruction.
 *   BFROM:       a = image ref or prior stage name,
 *                b = stage name (the "AS name" part, may be empty)
 *   BRUN:        a = shell command
 *   BCOPY:       a = src (context-relative, or stage/image path with
 *                    --from), b = dst (container path),
 *                from = "--from" ref (empty for a context COPY)
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
    char  from[BUILD_OP_STR];         /* COPY --from only */
    int   json_form;
} BuildOp;

/*
 * Parse a Dockerfile. Returns the op count, or -1 with a message in
 * err. Supported: FROM (single or multi-stage, "FROM x AS name"),
 * RUN (shell form), COPY (context-relative, or "--from=<stage|image>"),
 * WORKDIR, ENV, USER, CMD, ENTRYPOINT (both forms), and
 * EXPOSE/LABEL (accepted, ignored --
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
 * COPY --from: copy srcrel out of an IMAGE LAYER CHAIN instead of the
 * build context. diffs is the chain in TOP-FIRST order; layers are
 * merged bottom-to-top with overlay semantics (a char 0:0 whiteout in
 * a layer removes the path in the merged view). srcrel may have a
 * leading '/'; '..' is rejected. On success the merged subtree has
 * been copied into layer_root at target with build_copy_path
 * semantics. Returns 0, or -1 with a message in err.
 */
int build_copy_from_chain(const char *const *diffs, int nd,
                          const char *srcrel, const char *layer_root,
                          const char *target, char *err, size_t errsz);

/*
 * Append K=V to a comma-joined env list, replacing an existing entry
 * for the same key (docker: a later ENV wins). Returns 0, or -1 when
 * the list would overflow.
 */
int build_env_set(char *envlist, size_t n, const char *kv);

#endif /* BUILD_H */
