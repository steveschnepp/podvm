/*
 * build.c - Dockerfile parsing + build helpers (podvm build)
 *
 * Pure logic: no globals, no I/O beyond the copy helper's
 * syscalls and warnings on stderr.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>      /* major()/minor() for whiteout checks */

#include "build.h"
#include "subuid.h"   /* subuid_rm_rf: build staging is subuid-written */

#define PATH_MAX_BUILD 2048

static void seterr(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* Strip trailing \r and whitespace in place. */
static void rstrip(char *s) {
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t'))
        s[--l] = '\0';
}

/*
 * Exec-form array ("a", "b c") -> comma-joined csv. No escape handling:
 * a field containing a comma would collide with the join, which is the
 * same limitation the config channel has for args (documented).
 */
static int json_arr_to_csv(const char *s, char *csv, size_t n) {
    size_t off = 0;
    int first = 1;
    csv[0] = '\0';
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '[') return -1;
    s++;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (*s == ']') { s++; break; }
        if (*s != '"') return -1;
        s++;
        if (!first) {
            if (off + 1 >= n) return -1;
            csv[off++] = ',';
        }
        first = 0;
        while (*s && *s != '"') {
            if (off + 1 >= n) return -1;
            csv[off++] = *s++;
        }
        if (*s != '"') return -1;
        s++;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == ',') s++;
        else if (*s != ']') return -1;
    }
    csv[off] = '\0';
    return 0;
}

static BuildOpKind kind_of(const char *word) {
    if (!strcasecmp(word, "FROM"))       return BFROM;
    if (!strcasecmp(word, "RUN"))        return BRUN;
    if (!strcasecmp(word, "COPY"))       return BCOPY;
    if (!strcasecmp(word, "WORKDIR"))    return BWORKDIR;
    if (!strcasecmp(word, "ENV"))        return BENV;
    if (!strcasecmp(word, "USER"))       return BUSER;
    if (!strcasecmp(word, "CMD"))        return BCMD;
    if (!strcasecmp(word, "ENTRYPOINT")) return BENTRYPOINT;
    return (BuildOpKind)-1;
}

int dockerfile_parse(const char *text, BuildOp *ops, int max,
                     char *err, size_t errsz) {
    int n = 0;
    int lineno = 0;
    char logical[BUILD_OP_STR * 2];
    logical[0] = '\0';
    int cont_start = 0;

    const char *p = text;
    while (*p) {
        /* One physical line */
        char line[1024];
        size_t l = 0;
        while (*p && *p != '\n' && l + 1 < sizeof(line)) line[l++] = *p++;
        if (*p == '\n') p++;
        line[l] = '\0';
        lineno++;
        rstrip(line);

        int continues = 0;
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\\') {
            line[ll - 1] = '\0';
            rstrip(line);              /* drop the space before the backslash */
            continues = 1;
        }

        if (!logical[0]) cont_start = lineno;

        if (continues) {
            /* Append and keep reading. */
            size_t have = strlen(logical);
            snprintf(logical + have, sizeof(logical) - have, "%s%s",
                     have ? " " : "", line);
            continue;
        }
        /* Final physical line of a (possibly continued) logical line. */
        {
            size_t have = strlen(logical);
            snprintf(logical + have, sizeof(logical) - have, "%s%s",
                     have ? " " : "", line);
        }

        char *cmd = logical;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (!*cmd) { logical[0] = '\0'; continue; }        /* blank */
        if (*cmd == '#') { logical[0] = '\0'; continue; }  /* comment */

        char *arg = cmd;
        while (*arg && *arg != ' ' && *arg != '\t') arg++;
        if (*arg) { *arg++ = '\0'; while (*arg == ' ' || *arg == '\t') arg++; }
        rstrip(arg);

        /* Accepted-but-ignored: runtime concerns for podvm. */
        if (!strcasecmp(cmd, "EXPOSE") || !strcasecmp(cmd, "LABEL") ||
            !strcasecmp(cmd, "MAINTAINER")) {
            logical[0] = '\0';
            continue;
        }

        BuildOpKind k = kind_of(cmd);
        if (k == (BuildOpKind)-1) {
            seterr(err, errsz, "line %d: unsupported instruction '%s'",
                   cont_start, cmd);
            return -1;
        }
        if (n >= max) {
            seterr(err, errsz, "too many instructions (max %d)", max);
            return -1;
        }

        BuildOp *op = &ops[n];
        memset(op, 0, sizeof(*op));
        op->kind = k;
        op->line = cont_start;

        switch (k) {
        case BFROM: {
            if (!*arg) {
                seterr(err, errsz, "line %d: FROM needs an argument",
                       cont_start);
                return -1;
            }
            char *sp = arg;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            if (*sp) { *sp++ = '\0'; while (*sp == ' ' || *sp == '\t') sp++; }
            rstrip(sp);
            snprintf(op->a, sizeof(op->a), "%s", arg);
            if (*sp) {
                /* split the next word ("AS") off the remainder */
                char *word = sp;
                while (*word && *word != ' ' && *word != '\t') word++;
                if (*word) {
                    *word++ = '\0';
                    while (*word == ' ' || *word == '\t') word++;
                }
                rstrip(word);
                if (strcasecmp(sp, "AS")) {
                    seterr(err, errsz, "line %d: expected 'AS <name>' "
                           "after FROM, got '%s'", cont_start, sp);
                    return -1;
                }
                if (!*word) {
                    seterr(err, errsz, "line %d: FROM ... AS needs a "
                           "stage name", cont_start);
                    return -1;
                }
                snprintf(op->b, sizeof(op->b), "%s", word);
            }
            break;
        }
        case BWORKDIR:
        case BUSER:
            if (!*arg) {
                seterr(err, errsz, "line %d: %s needs an argument",
                       cont_start, cmd);
                return -1;
            }
            snprintf(op->a, sizeof(op->a), "%s", arg);
            break;
        case BRUN:
            if (!*arg) {
                seterr(err, errsz, "line %d: RUN needs a command", cont_start);
                return -1;
            }
            if (*arg == '[') {
                seterr(err, errsz, "line %d: RUN exec form is not supported",
                       cont_start);
                return -1;
            }
            snprintf(op->a, sizeof(op->a), "%s", arg);
            break;
        case BENV:
            if (!*arg || !strchr(arg, '=')) {
                seterr(err, errsz, "line %d: ENV needs KEY=VALUE pairs",
                       cont_start);
                return -1;
            }
            snprintf(op->a, sizeof(op->a), "%s", arg);
            break;
        case BCOPY: {
            /* Leading flags: only --from=<ref> (docker also has
             * --chown/--link; reject those explicitly rather than
             * silently misparsing them as a src path). */
            while (!strncmp(arg, "--", 2)) {
                if (strncmp(arg, "--from=", 7)) {
                    seterr(err, errsz, "line %d: unsupported COPY flag '%s'",
                           cont_start, arg);
                    return -1;
                }
                char *val = arg + 7;
                char *end = val;
                while (*end && *end != ' ' && *end != '\t') end++;
                char save = *end; *end = '\0';
                if (!*val) {
                    *end = save;
                    seterr(err, errsz, "line %d: COPY --from needs a "
                           "stage name or image", cont_start);
                    return -1;
                }
                snprintf(op->from, sizeof(op->from), "%s", val);
                *end = save;
                arg = end;
                while (*arg == ' ' || *arg == '\t') arg++;
            }
            /* dst is the LAST token; everything before it is the src
             * list (docker: "COPY a b /dir/" copies each source). */
            if (!*arg) {
                seterr(err, errsz, "line %d: COPY needs src and dst", cont_start);
                return -1;
            }
            char *dststart = arg, *p = arg;
            while (*p) {
                while (*p == ' ' || *p == '\t') { p++; dststart = p; }
                while (*p && *p != ' ' && *p != '\t') p++;
            }
            if (dststart == arg) {
                seterr(err, errsz, "line %d: COPY needs src and dst", cont_start);
                return -1;
            }
            char *srcend = dststart;
            while (srcend > arg && (srcend[-1] == ' ' || srcend[-1] == '\t'))
                srcend--;
            *srcend = '\0';
            rstrip(dststart);
            if (!*dststart) {
                seterr(err, errsz, "line %d: COPY needs a destination",
                       cont_start);
                return -1;
            }
            snprintf(op->a, sizeof(op->a), "%s", arg);
            snprintf(op->b, sizeof(op->b), "%s", dststart);
            break;
        }
        case BCMD:
        case BENTRYPOINT:
            if (!*arg) {
                seterr(err, errsz, "line %d: %s needs an argument",
                       cont_start, cmd);
                return -1;
            }
            if (*arg == '[') {
                if (json_arr_to_csv(arg, op->a, sizeof(op->a)) < 0) {
                    seterr(err, errsz, "line %d: malformed %s exec form",
                           cont_start, cmd);
                    return -1;
                }
                op->json_form = 1;
            } else {
                snprintf(op->a, sizeof(op->a), "%s", arg);
                op->json_form = 0;
            }
            break;
        default:
            seterr(err, errsz, "line %d: internal op error", cont_start);
            return -1;
        }

        n++;
        logical[0] = '\0';
    }

    if (logical[0]) {
        /* Trailing continuation at EOF */
        seterr(err, errsz, "line %d: dangling line continuation", lineno);
        return -1;
    }
    if (n == 0) {
        seterr(err, errsz, "no instructions found");
        return -1;
    }
    if (ops[0].kind != BFROM) {
        seterr(err, errsz, "line %d: the first instruction must be FROM",
               ops[0].line);
        return -1;
    }
    return n;
}

/* ─────────────────────────── COPY ─────────────────────────── */

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX_BUILD];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/') {
            *q = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
            *q = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_rec(const char *src, const char *dst, char *err, size_t errsz) {
    struct stat st;
    if (lstat(src, &st) < 0) {
        seterr(err, errsz, "cannot stat %s: %s", src, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) < 0 && errno != EEXIST) {
            seterr(err, errsz, "cannot mkdir %s: %s", dst, strerror(errno));
            return -1;
        }
        DIR *d = opendir(src);
        if (!d) {
            seterr(err, errsz, "cannot read %s: %s", src, strerror(errno));
            return -1;
        }
        struct dirent *de;
        int rc = 0;
        while (rc == 0 && (de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char s2[PATH_MAX_BUILD], d2[PATH_MAX_BUILD];
            if (snprintf(s2, sizeof(s2), "%s/%s", src, de->d_name) >= (int)sizeof(s2) ||
                snprintf(d2, sizeof(d2), "%s/%s", dst, de->d_name) >= (int)sizeof(d2)) {
                seterr(err, errsz, "path too long under %s", src);
                rc = -1;
                break;
            }
            rc = copy_rec(s2, d2, err, errsz);
        }
        closedir(d);
        return rc;
    }

    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX_BUILD];
        ssize_t t = readlink(src, target, sizeof(target) - 1);
        if (t < 0) {
            seterr(err, errsz, "cannot readlink %s: %s", src, strerror(errno));
            return -1;
        }
        target[t] = '\0';
        remove(dst);
        if (symlink(target, dst) < 0) {
            seterr(err, errsz, "cannot symlink %s: %s", dst, strerror(errno));
            return -1;
        }
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        int in = open(src, O_RDONLY);
        if (in < 0) {
            seterr(err, errsz, "cannot open %s: %s", src, strerror(errno));
            return -1;
        }
        int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
        if (out < 0) {
            seterr(err, errsz, "cannot create %s: %s", dst, strerror(errno));
            close(in);
            return -1;
        }
        char buf[65536];
        ssize_t r;
        while ((r = read(in, buf, sizeof(buf))) > 0) {
            char *q = buf;
            ssize_t left = r;
            while (left > 0) {
                ssize_t w = write(out, q, (size_t)left);
                if (w < 0) {
                    seterr(err, errsz, "cannot write %s: %s", dst,
                           strerror(errno));
                    close(in); close(out);
                    return -1;
                }
                left -= w;
                q += w;
            }
        }
        close(in);
        close(out);
        return r < 0 ? -1 : 0;
    }

    fprintf(stderr, "podvm build: skipping special file %s\n", src);
    return 0;
}

int build_copy_path(const char *src, const char *layer_root,
                    const char *target, char *err, size_t errsz) {
    /* Context-safety (relative, no '..') is the driver's check: it is
     * the one holding the original context-relative src. Here src is
     * the resolved absolute path; it just has to exist. */
    if (!src[0]) {
        seterr(err, errsz, "COPY src is empty");
        return -1;
    }
    if (!target[0] || target[0] != '/') {
        seterr(err, errsz, "COPY dst must be absolute: '%s'", target);
        return -1;
    }

    char dst[PATH_MAX_BUILD];
    if (snprintf(dst, sizeof(dst), "%s%s", layer_root, target)
        >= (int)sizeof(dst)) {
        seterr(err, errsz, "COPY destination too long");
        return -1;
    }

    struct stat sst;
    if (stat(src, &sst) < 0) {
        seterr(err, errsz, "COPY src not found: %s", src);
        return -1;
    }

    if (S_ISDIR(sst.st_mode)) {
        /* dir src: its CONTENTS go into target (the `COPY app/ /app`
         * shape) */
        if (mkdir_p(dst, 0755) < 0) {
            seterr(err, errsz, "cannot mkdir %s: %s", dst, strerror(errno));
            return -1;
        }
        DIR *d = opendir(src);
        if (!d) {
            seterr(err, errsz, "cannot read %s: %s", src, strerror(errno));
            return -1;
        }
        struct dirent *de;
        int rc = 0;
        while (rc == 0 && (de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char s2[PATH_MAX_BUILD], d2[PATH_MAX_BUILD];
            if (snprintf(s2, sizeof(s2), "%s/%s", src, de->d_name) >= (int)sizeof(s2) ||
                snprintf(d2, sizeof(d2), "%s/%s", dst, de->d_name) >= (int)sizeof(d2)) {
                seterr(err, errsz, "path too long under %s", src);
                rc = -1;
                break;
            }
            rc = copy_rec(s2, d2, err, errsz);
        }
        closedir(d);
        return rc;
    }

    /* file (or symlink) src: becomes exactly target */
    {
        char parent[PATH_MAX_BUILD];
        snprintf(parent, sizeof(parent), "%s", dst);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = '\0';
        if (parent[0] && mkdir_p(parent, 0755) < 0) {
            seterr(err, errsz, "cannot mkdir %s: %s", parent, strerror(errno));
            return -1;
        }
    }
    remove(dst);
    return copy_rec(src, dst, err, errsz);
}

/* ─────────────────── COPY --from (layer chain) ─────────────────── */

static int rm_rf(const char *path) {
    /* Build staging is written via the userns overlay, so it can hold
     * subuid-owned files the invoking user cannot unlink. subuid_rm_rf
     * handles both: a plain fast path for user-owned trees, escalating to
     * a subuid-mapped userns for the rest. Keep the ENOENT->0 convention
     * (it lstats first and leaves errno=ENOENT when absent). */
    if (subuid_rm_rf(path) < 0) return errno == ENOENT ? 0 : -1;
    return 0;
}

/* overlayfs whiteout: char device 0:0 with the deleted path's name. */
static int is_whiteout(const struct stat *st) {
    return S_ISCHR(st->st_mode) && major(st->st_rdev) == 0 &&
           minor(st->st_rdev) == 0;
}

/*
 * Merge src (one layer's view of the subtree, bottom-to-top order) into
 * dst (the merged view so far): whiteout clears dst, a directory merges
 * entry-by-entry, a file/symlink replaces dst wholesale.
 */
static int merge_subtree(const char *src, const char *dst,
                         char *err, size_t errsz) {
    struct stat st;
    if (lstat(src, &st) < 0) return 0;            /* not in this layer */

    if (is_whiteout(&st)) {
        if (rm_rf(dst) < 0) {
            seterr(err, errsz, "cannot apply whiteout %s: %s", dst,
                   strerror(errno));
            return -1;
        }
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir_p(dst, st.st_mode & 07777) < 0 && errno != EEXIST) {
            seterr(err, errsz, "cannot mkdir %s: %s", dst, strerror(errno));
            return -1;
        }
        DIR *d = opendir(src);
        if (!d) {
            seterr(err, errsz, "cannot read %s: %s", src, strerror(errno));
            return -1;
        }
        struct dirent *de;
        int rc = 0;
        while (rc == 0 && (de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char s2[PATH_MAX_BUILD], d2[PATH_MAX_BUILD];
            if (snprintf(s2, sizeof(s2), "%s/%s", src, de->d_name) >= (int)sizeof(s2) ||
                snprintf(d2, sizeof(d2), "%s/%s", dst, de->d_name) >= (int)sizeof(d2)) {
                seterr(err, errsz, "path too long under %s", src);
                rc = -1;
                break;
            }
            rc = merge_subtree(s2, d2, err, errsz);
        }
        closedir(d);
        return rc;
    }

    if (S_ISLNK(st.st_mode) || S_ISREG(st.st_mode)) {
        char parent[PATH_MAX_BUILD];
        snprintf(parent, sizeof(parent), "%s", dst);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = '\0';
        if (parent[0] && mkdir_p(parent, 0755) < 0) {
            seterr(err, errsz, "cannot mkdir %s: %s", parent, strerror(errno));
            return -1;
        }
        remove(dst);
        return copy_rec(src, dst, err, errsz);
    }

    fprintf(stderr, "podvm build: skipping special file %s\n", src);
    return 0;
}

int build_copy_from_chain(const char *const *diffs, int nd,
                          const char *srcrel, const char *layer_root,
                          const char *target, char *err, size_t errsz) {
    if (nd <= 0) {
        seterr(err, errsz, "COPY --from: empty layer chain");
        return -1;
    }
    if (strstr(srcrel, "..")) {
        seterr(err, errsz, "COPY --from src must not contain '..': '%s'",
               srcrel);
        return -1;
    }
    while (*srcrel == '/') srcrel++;               /* /out and out are equal */
    if (!*srcrel) {
        seterr(err, errsz, "COPY --from src is empty");
        return -1;
    }

    char tmpl[] = "/tmp/podvm-cpyfrom.XXXXXX";
    char *tmp = mkdtemp(tmpl);
    if (!tmp) {
        seterr(err, errsz, "cannot create a temp dir: %s", strerror(errno));
        return -1;
    }

    char merged[PATH_MAX_BUILD];
    if (snprintf(merged, sizeof(merged), "%s/%s", tmp, srcrel)
        >= (int)sizeof(merged)) {
        seterr(err, errsz, "COPY --from src path too long: '%s'", srcrel);
        rm_rf(tmp);
        return -1;
    }

    /* bottom -> top: diffs is top-first, so walk it backwards */
    for (int l = nd - 1; l >= 0; l--) {
        char src[PATH_MAX_BUILD];
        if (snprintf(src, sizeof(src), "%s/%s", diffs[l], srcrel)
            >= (int)sizeof(src)) {
            seterr(err, errsz, "COPY --from path too long: '%s'", diffs[l]);
            rm_rf(tmp);
            return -1;
        }
        if (merge_subtree(src, merged, err, errsz) < 0) {
            rm_rf(tmp);
            return -1;
        }
    }

    struct stat st;
    if (lstat(merged, &st) < 0) {
        seterr(err, errsz, "COPY --from src not found in the source "
               "image: /%s", srcrel);
        rm_rf(tmp);
        return -1;
    }

    int rc = build_copy_path(merged, layer_root, target, err, errsz);
    rm_rf(tmp);
    return rc;
}

/* ─────────────────────────── ENV ─────────────────────────── */

int build_env_set(char *envlist, size_t n, const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq) return -1;
    size_t klen = (size_t)(eq - kv) + 1;   /* include '=' */

    /* Replace an existing entry for the same key (later ENV wins). The
     * entry is longer than the key (it has a value), so match on the
     * key prefix: "A=" must not match "AB=2" -- strncmp stops at '='. */
    char *p = envlist;
    while (p && *p) {
        char *end = strchr(p, ',');
        size_t clen = end ? (size_t)(end - p) : strlen(p);
        if (clen >= klen && !strncmp(p, kv, klen)) {
            /* Remove this entry: shift the rest left. */
            size_t tail = clen + (end ? 1 : 0);
            memmove(p, p + tail, strlen(p + tail) + 1);
            break;
        }
        p = end ? end + 1 : NULL;
    }

    size_t have = strlen(envlist);
    int w = snprintf(envlist + have, n - have, "%s%s", have ? "," : "", kv);
    if (w < 0 || (size_t)w >= n - have) return -1;
    return 0;
}
