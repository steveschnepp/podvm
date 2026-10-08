/*
 * json.c - minimal JSON DOM parser
 *
 * Extracted from image.c so the podman storage reader (store.c) can
 * share it. Recursive descent; handles escapes, \uXXXX incl. surrogate
 * pairs, numbers, nested objects/arrays. No external dependency.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "json.h"

static JNode *jnew(JType t) {
    JNode *j = calloc(1, sizeof(*j));
    if (j) j->type = t;
    return j;
}

void json_free(JNode *j) {
    if (!j) return;
    free(j->str);
    for (size_t i = 0; i < j->n; i++) {
        free(j->keys ? j->keys[i] : NULL);
        json_free(j->items ? j->items[i] : NULL);
    }
    free(j->keys);
    free(j->items);
    free(j);
}

typedef struct { const char *s; const char *p; } JParser;

static void jskip(JParser *jp) {
    while (*jp->p && isspace((unsigned char)*jp->p)) jp->p++;
}

static JNode *jparse_value(JParser *jp);

static char *jparse_string_raw(JParser *jp) {
    /* jp->p points at opening quote; returns malloc'd decoded string */
    if (*jp->p != '"') return NULL;
    jp->p++;
    size_t cap = 16, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;

    while (*jp->p && *jp->p != '"') {
        char c = *jp->p;
        if (c == '\\') {
            jp->p++;
            char e = *jp->p;
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                /* BMP code point; handle surrogate pairs minimally */
                unsigned int cp = 0;
                for (int k = 1; k <= 4 && jp->p[k]; k++) {
                    char h = jp->p[k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                }
                jp->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && jp->p[1] == '\\' && jp->p[2] == 'u') {
                    unsigned int lo = 0;
                    const char *q = jp->p + 3;
                    for (int k = 0; k < 4 && q[k]; k++) {
                        char h = q[k];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                    }
                    jp->p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                /* UTF-8 encode */
                char ub[4];
                int un = 0;
                if (cp < 0x80) { ub[0] = (char)cp; un = 1; }
                else if (cp < 0x800) { ub[0] = (char)(0xC0 | (cp >> 6)); ub[1] = (char)(0x80 | (cp & 0x3F)); un = 2; }
                else if (cp < 0x10000) { ub[0] = (char)(0xE0 | (cp >> 12)); ub[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); ub[2] = (char)(0x80 | (cp & 0x3F)); un = 3; }
                else { ub[0] = (char)(0xF0 | (cp >> 18)); ub[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); ub[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); ub[3] = (char)(0x80 | (cp & 0x3F)); un = 4; }
                if (len + (size_t)un + 1 > cap) { cap = (len + (size_t)un + 1) * 2; char *t = realloc(out, cap); if (!t) { free(out); return NULL; } out = t; }
                memcpy(out + len, ub, (size_t)un);
                len += (size_t)un;
                jp->p++;   /* consume the final hex digit */
                continue;
            }
            default: c = e; break;
            }
            jp->p++;
        } else {
            jp->p++;
        }
        if (len + 1 >= cap) { cap *= 2; char *t = realloc(out, cap); if (!t) { free(out); return NULL; } out = t; }
        out[len++] = c;
    }
    if (*jp->p != '"') { free(out); return NULL; }
    jp->p++;
    out[len] = '\0';
    return out;
}

static JNode *jparse_value(JParser *jp) {
    jskip(jp);
    char c = *jp->p;

    if (c == '{') {
        jp->p++;
        JNode *o = jnew(JOBJ);
        if (!o) return NULL;
        jskip(jp);
        if (*jp->p == '}') { jp->p++; return o; }
        for (;;) {
            jskip(jp);
            char *key = jparse_string_raw(jp);
            if (!key) { json_free(o); return NULL; }
            jskip(jp);
            if (*jp->p != ':') { free(key); json_free(o); return NULL; }
            jp->p++;
            JNode *val = jparse_value(jp);
            if (!val) { free(key); json_free(o); return NULL; }
            JNode **ni = realloc(o->items, (o->n + 1) * sizeof(*ni));
            char **nk = realloc(o->keys, (o->n + 1) * sizeof(*nk));
            if (!ni || !nk) { free(ni); free(nk); free(key); json_free(val); json_free(o); return NULL; }
            o->items = ni; o->keys = nk;
            o->items[o->n] = val;
            o->keys[o->n] = key;
            o->n++;
            jskip(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == '}') { jp->p++; return o; }
            json_free(o); return NULL;
        }
    }

    if (c == '[') {
        jp->p++;
        JNode *a = jnew(JARR);
        if (!a) return NULL;
        jskip(jp);
        if (*jp->p == ']') { jp->p++; return a; }
        for (;;) {
            JNode *val = jparse_value(jp);
            if (!val) { json_free(a); return NULL; }
            JNode **ni = realloc(a->items, (a->n + 1) * sizeof(*ni));
            if (!ni) { json_free(val); json_free(a); return NULL; }
            a->items = ni;
            a->items[a->n++] = val;
            jskip(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == ']') { jp->p++; return a; }
            json_free(a); return NULL;
        }
    }

    if (c == '"') {
        char *s = jparse_string_raw(jp);
        if (!s) return NULL;
        JNode *n = jnew(JSTR);
        if (!n) { free(s); return NULL; }
        n->str = s;
        return n;
    }

    if (!strncmp(jp->p, "true", 4)) { jp->p += 4; JNode *n = jnew(JBOOL); if (n) n->bval = 1; return n; }
    if (!strncmp(jp->p, "false", 5)) { jp->p += 5; JNode *n = jnew(JBOOL); return n; }
    if (!strncmp(jp->p, "null", 4)) { jp->p += 4; return jnew(JNULL); }

    /* number */
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *start = jp->p;
        while (*jp->p && (isdigit((unsigned char)*jp->p) || *jp->p == '-' ||
                          *jp->p == '+' || *jp->p == '.' || *jp->p == 'e' || *jp->p == 'E'))
            jp->p++;
        size_t len = (size_t)(jp->p - start);
        char *raw = malloc(len + 1);
        if (!raw) return NULL;
        memcpy(raw, start, len);
        raw[len] = '\0';
        JNode *n = jnew(JNUM);
        if (!n) { free(raw); return NULL; }
        n->str = raw;
        return n;
    }

    return NULL;
}

JNode *json_parse(const char *s) {
    JParser jp = { s, s };
    JNode *v = jparse_value(&jp);
    return v;
}

JNode *json_get(JNode *o, const char *key) {
    if (!o || o->type != JOBJ) return NULL;
    for (size_t i = 0; i < o->n; i++)
        if (strcmp(o->keys[i], key) == 0) return o->items[i];
    return NULL;
}

JNode *json_at(JNode *a, size_t i) {
    if (!a || a->type != JARR || i >= a->n) return NULL;
    return a->items[i];
}

const char *json_str(JNode *n) {
    if (!n) return NULL;
    if (n->type == JSTR || n->type == JNUM) return n->str;
    return NULL;
}

