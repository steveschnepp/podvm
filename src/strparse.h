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
 * Value extends to next space or end of string.
 */
const char *kv_find(const char *cmdline, const char *prefix);

/*
 * Copy value from src to dst, stopping at space or end.
 * Returns number of bytes copied.
 */
size_t kv_copy_value(char *dst, size_t dst_size, const char *src);

/*
 * Parse comma-separated values.
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
