/*
 * strparse.h - Simple string parsing helpers
 *
 * Minimal, no dependencies.
 */

#ifndef STRPARSE_H
#define STRPARSE_H

#include <stddef.h>

/*
 * Find value after prefix in "key=value key2=value2" format.
 * Returns pointer to value, or NULL if not found.
 * Value extends to next *unescaped* space or end of string.
 */
const char *kv_find(const char *cmdline, const char *prefix);

/*
 * Copy one token's value into dst, stopping at an *unescaped* space.
 * Escape sequences (\\, \ , \,) are copied through verbatim -- call
 * kv_unescape() for single values, csv_parse() for comma lists.
 * Returns bytes copied (excluding NUL).
 */
size_t kv_copy_value(char *dst, size_t dst_size, const char *src);

/*
 * Resolve escape sequences in place: \\ -> \, \  -> space, \, -> comma.
 */
void kv_unescape(char *s);

/*
 * Escape a value for the wire (backslash, space, comma -> \\, \ , \,).
 * Returns bytes written (excluding NUL), or (size_t)-1 if it did not fit.
 */
size_t kv_escape(char *dst, size_t dst_size, const char *src);

/*
 * Parse a comma-separated list, splitting on *unescaped* commas and
 * unescaping each field. An unescaped space terminates the value.
 * Returns number of values parsed.
 */
int csv_parse(const char *input, char **output, int max);

/*
 * Parse "uid:gid" string.
 * Returns 0 on success.
 */
int parse_uid_gid(const char *str, int *uid, int *gid);

/*
 * Free values allocated by csv_parse.
 */
void csv_free(char **values, int count);

#endif /* STRPARSE_H */
