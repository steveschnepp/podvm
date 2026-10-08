/*
 * json.h - minimal JSON DOM parser
 */

#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } JType;

typedef struct JNode JNode;
struct JNode {
    JType type;
    char *str;       /* JSTR (decoded) or JNUM (raw) */
    int bval;        /* JBOOL */
    char **keys;     /* JOBJ */
    JNode **items;   /* JOBJ values / JARR items */
    size_t n;
};

JNode *json_parse(const char *s);
void json_free(JNode *j);
JNode *json_get(JNode *o, const char *key);
JNode *json_at(JNode *a, size_t i);
const char *json_str(JNode *n);

#endif /* JSON_H */
