/*
 * strparse.c - Simple string parsing helpers
 *
 * Minimal, no dependencies.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "strparse.h"

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

        /* Skip to next space */
        while (*p && *p != ' ') p++;
    }
    return NULL;
}

size_t kv_copy_value(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    
    while (src[i] && src[i] != ' ' && i < dst_size - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    
    return i;
}

int csv_parse(const char *input, char **output, int max) {
    int count = 0;
    const char *p = input;

    while (*p && count < max) {
        /* Skip leading spaces (tolerant after a comma) */
        while (*p == ' ') p++;
        if (!*p) break;

        /* Find start of value */
        const char *start = p;

        /* Find comma, space, or end */
        while (*p && *p != ',' && *p != ' ') p++;

        /* Copy value */
        size_t len = p - start;
        output[count] = malloc(len + 1);
        if (output[count]) {
            memcpy(output[count], start, len);
            output[count][len] = '\0';
            count++;
        }

        /* Comma: continue. Space/end: the value is done -- in the
         * kernel cmdline format a space starts a new podvm.*= token;
         * the host joins args with commas only, so a space is a
         * terminator, not a separator. */
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
