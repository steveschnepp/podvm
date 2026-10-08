/*
 * strparse.c - Simple string parsing helpers
 *
 * Minimal, no dependencies.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "strparse.h"

/*
 * The config channel is a space-separated list of podvm.key=value tokens,
 * and some values (args, env) are themselves comma-joined lists. So a
 * value containing a space or comma is lossy without escaping.
 *
 * Escaping: backslash is the escape character.
 *   \\  -> literal backslash
 *   \   -> literal space
 *   \,  -> literal comma
 *
 * kv_escape()   produces this form (host side).
 * kv_copy_value() extracts one token, stopping at an *unescaped* space,
 *                 copying the escape sequences through verbatim.
 * kv_unescape() resolves the sequences (for single-valued fields).
 * csv_parse()   splits on *unescaped* commas and unescapes each field.
 * kv_find()     skips whole tokens, honoring escapes, to reach the next
 *                 key.
 */

/* Resolve escape sequences in place: \\ -> \, \  -> space, \, -> comma. */
void kv_unescape(char *s) {
    if (!s) return;
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            /* An escaped char is taken literally; for anything other than
             * space/comma/backslash we keep the character and drop the
             * backslash, which is the least-surprising resolution. */
            *w++ = *r;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/*
 * Escape a value for the wire: backslash, space, and comma become \\, \ ,
 * and \, so it survives token splitting (space) and list splitting (comma).
 * Returns bytes written (excluding NUL), or (size_t)-1 if it did not fit.
 */
size_t kv_escape(char *dst, size_t dst_size, const char *src) {
    size_t o = 0;
    for (size_t i = 0; src[i]; i++) {
        int esc = (src[i] == '\\' || src[i] == ' ' || src[i] == ',');
        size_t need = esc ? 2 : 1;
        if (o + need + 1 > dst_size) return (size_t)-1;
        if (esc) dst[o++] = '\\';
        dst[o++] = src[i];
    }
    if (o < dst_size) dst[o] = '\0';
    return o;
}

const char *kv_find(const char *cmdline, const char *prefix) {
    const char *p = cmdline;
    size_t plen = strlen(prefix);

    while (*p) {
        /* Skip spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        /* Check for prefix */
        if (strncmp(p, prefix, plen) == 0)
            return p + plen;

        /* Skip to the next *unescaped* space (an escaped space is part
         * of the value, not a token boundary). */
        while (*p) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == ' ') break;
            p++;
        }
    }
    return NULL;
}

/*
 * Copy one token (the value part of podvm.key=value) into dst, stopping at
 * an *unescaped* space. Escape sequences are copied through verbatim --
 * call kv_unescape() for single-valued fields, or csv_parse() for lists.
 * Returns bytes copied (excluding NUL).
 */
size_t kv_copy_value(char *dst, size_t dst_size, const char *src) {
    size_t i = 0, o = 0;

    while (src[i] && o < dst_size - 1) {
        if (src[i] == '\\' && src[i + 1]) {
            /* Keep the escape + escaped char for the unescape stage. */
            if (o + 2 > dst_size - 1) break;
            dst[o++] = src[i];
            dst[o++] = src[i + 1];
            i += 2;
            continue;
        }
        if (src[i] == ' ') break;   /* unescaped space = token boundary */
        dst[o++] = src[i++];
    }
    dst[o] = '\0';

    return o;
}

int csv_parse(const char *input, char **output, int max) {
    int count = 0;
    const char *p = input;

    while (*p && count < max) {
        /* Skip unescaped leading spaces (tolerant after a comma). */
        while (*p == ' ') p++;
        if (!*p) break;

        /* Collect one field, unescaping into place, stopping at an
         * unescaped comma (more fields follow) or an unescaped space (the
         * value is done -- a space starts a new podvm.*= token). */
        size_t cap = strlen(p) + 1;
        char *field = malloc(cap);
        if (!field) break;
        size_t o = 0;
        while (*p) {
            if (*p == '\\' && p[1]) {
                field[o++] = p[1];
                p += 2;
                continue;
            }
            if (*p == ',' || *p == ' ') break;
            field[o++] = *p++;
        }
        field[o] = '\0';
        output[count++] = field;

        if (*p == ',') p++;
        else break;
    }

    return count;
}

void csv_free(char **values, int count) {
    for (int i = 0; i < count; i++) {
        free(values[i]);
    }
}

int parse_uid_gid(const char *str, int *uid, int *gid) {
    *uid = *gid = 0;
    
    if (!str) return -1;
    
    if (sscanf(str, "%d:%d", uid, gid) == 2)
        return 0;
    
    if (sscanf(str, "%d", uid) == 1)
        return 0;
    
    return -1;
}
