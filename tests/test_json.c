/*
 * test_json.c - unit tests for src/json.c
 *
 * The DOM parser is only exercised indirectly today (via test_image's
 * config blob and test_build's JSON-form RUN). This covers the parser
 * contract on its own: shape access, escape decoding, numbers/booleans/
 * null, and the rejection of malformed input.
 *
 * Build: gcc -o test_json tests/test_json.c src/json.c -Isrc
 * Run: ./test_json
 */

#include <stdio.h>
#include <string.h>
#include "json.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void test_object_and_access(void) {
    JNode *j = json_parse("{\"a\":1,\"b\":\"two\",\"c\":[1,2],\"d\":{\"e\":true}}");
    CHECK(j && j->type == JOBJ);
    CHECK(j->n == 4);
    CHECK(json_str(json_get(j, "a")) && strcmp(json_str(json_get(j, "a")), "1") == 0);
    CHECK(json_str(json_get(j, "b")) && strcmp(json_str(json_get(j, "b")), "two") == 0);

    JNode *c = json_get(j, "c");
    CHECK(c && c->type == JARR && c->n == 2);
    CHECK(json_str(json_at(c, 0)) && strcmp(json_str(json_at(c, 0)), "1") == 0);
    CHECK(json_str(json_at(c, 1)) && strcmp(json_str(json_at(c, 1)), "2") == 0);
    CHECK(json_at(c, 2) == NULL);            /* out of range is NULL, not a crash */

    JNode *d = json_get(j, "d");
    CHECK(d && d->type == JOBJ);
    JNode *e = json_get(d, "e");
    CHECK(e && e->type == JBOOL && e->bval == 1);

    CHECK(json_get(j, "missing") == NULL);
    json_free(j);
}

static void test_numbers_and_null(void) {
    JNode *j = json_parse("{\"i\":42,\"neg\":-7,\"f\":3.5,\"e\":1e3,\"n\":null,\"f2\":false}");
    CHECK(j && j->type == JOBJ);
    CHECK(json_str(json_get(j, "i")) && strcmp(json_str(json_get(j, "i")), "42") == 0);
    CHECK(json_str(json_get(j, "neg")) && strcmp(json_str(json_get(j, "neg")), "-7") == 0);
    CHECK(json_str(json_get(j, "f")) && strcmp(json_str(json_get(j, "f")), "3.5") == 0);
    CHECK(json_str(json_get(j, "e")) && strcmp(json_str(json_get(j, "e")), "1e3") == 0);
    CHECK(json_get(j, "n")->type == JNULL);
    CHECK(json_get(j, "f2")->type == JBOOL && json_get(j, "f2")->bval == 0);
    json_free(j);
}

static void test_escape_decoding(void) {
    /* The decoder must emit real bytes, not leave escape sequences raw. */
    JNode *j = json_parse("{\"s\":\"a\\nb\\tc\\\"d\\\\e\\/f\"}");
    CHECK(j && j->type == JOBJ);
    const char *s = json_str(json_get(j, "s"));
    CHECK(s && strcmp(s, "a\nb\tc\"d\\e/f") == 0);
    json_free(j);

    /* \uXXXX BMP + a surrogate pair (2-byte and 4-byte UTF-8). */
    JNode *u = json_parse("{\"a\":\"\\u0041\\u00e9\",\"b\":\"\\ud83d\\ude00\"}");
    CHECK(u && u->type == JOBJ);
    const char *a = json_str(json_get(u, "a"));
    CHECK(a && strcmp(a, "A\xc3\xa9") == 0);          /* A + e-acute */
    const char *b = json_str(json_get(u, "b"));
    CHECK(b && strcmp(b, "\xf0\x9f\x98\x80") == 0);   /* U+1F600 */
    json_free(u);
}

static void test_rejects_malformed(void) {
    /* json_parse returns a value or NULL; garbage must be NULL. */
    CHECK(json_parse("") == NULL);
    CHECK(json_parse("{") == NULL);
    CHECK(json_parse("{\"a\":}") == NULL);
    CHECK(json_parse("{\"a\":1,}") == NULL);      /* trailing comma */
    CHECK(json_parse("[1,2") == NULL);
    CHECK(json_parse("[1,2,]") == NULL);
    CHECK(json_parse("\"unterminated") == NULL);
    CHECK(json_parse("{\"a\" 1}") == NULL);       /* missing colon */
    CHECK(json_parse("{a:1}") == NULL);           /* unquoted key */
    CHECK(json_parse("nul") == NULL);
    CHECK(json_parse("truth") == NULL);
}

static void test_trailing_garbage_is_tolerated(void) {
    /* Contract: the parser stops at the end of the first value and does
     * not validate the remainder. Callers (store.c) rely on this being
     * tolerant, so pin the behavior rather than pretend it is strict. */
    JNode *j = json_parse("{\"a\":1} trailing junk");
    CHECK(j && j->type == JOBJ);
    CHECK(json_str(json_get(j, "a")) && strcmp(json_str(json_get(j, "a")), "1") == 0);
    json_free(j);
}

int main(void) {
    test_object_and_access();
    test_numbers_and_null();
    test_escape_decoding();
    test_rejects_malformed();
    test_trailing_garbage_is_tolerated();

    if (failures) {
        printf("json tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("json tests: all passed\n");
    return 0;
}
