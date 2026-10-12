/*
 * test_build.c - Dockerfile parser tests
 *
 * Links the real src/build.c (no mocked copy).
 *
 * Build: gcc -Wall -Wextra -O2 -g -o test_build test_build.c ../src/build.c -I../src
 * Run: ./test_build
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../src/build.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static BuildOp ops[BUILD_OPS_MAX];
static char err[512];

/* Parse; assert success and return op count. */
static int parse_ok(const char *text) {
    int n = dockerfile_parse(text, ops, BUILD_OPS_MAX, err, sizeof(err));
    if (n < 0) printf("  unexpected parse error: %s\n", err);
    CHECK(n >= 0);
    return n;
}

static void test_basic(void) {
    int n = parse_ok(
        "# a comment\n"
        "\n"
        "FROM alpine:latest\n"
        "RUN echo hi\n"
        "COPY data.txt /app/data.txt\n"
        "WORKDIR /app\n"
        "ENV A=1 B=2\n"
        "USER app\n"
        "EXPOSE 8080\n"
        "LABEL x=y\n"
        "CMD [\"/bin/echo\", \"hello\"]\n");
    CHECK(n == 7);                       /* EXPOSE/LABEL dropped */
    CHECK(ops[0].kind == BFROM && strcmp(ops[0].a, "alpine:latest") == 0);
    CHECK(ops[1].kind == BRUN && strcmp(ops[1].a, "echo hi") == 0);
    CHECK(ops[2].kind == BCOPY &&
          strcmp(ops[2].a, "data.txt") == 0 &&
          strcmp(ops[2].b, "/app/data.txt") == 0);
    CHECK(ops[3].kind == BWORKDIR && strcmp(ops[3].a, "/app") == 0);
    CHECK(ops[4].kind == BENV && strcmp(ops[4].a, "A=1 B=2") == 0);
    CHECK(ops[5].kind == BUSER && strcmp(ops[5].a, "app") == 0);
    CHECK(ops[6].kind == BCMD && ops[6].json_form == 1 &&
          strcmp(ops[6].a, "/bin/echo,hello") == 0);
    printf("PASS: basic instructions\n");
}

static void test_continuation(void) {
    /* The continuation join keeps the next line's leading whitespace
     * (docker does too), so this second line has no indent. */
    int n = parse_ok(
        "FROM alpine\n"
        "RUN echo one \\\n"
        "&& echo two\n"
        "CMD [\"a\", \"b\"]\n");
    CHECK(n == 3);
    CHECK(strcmp(ops[1].a, "echo one && echo two") == 0);
    CHECK(ops[2].json_form == 1 && strcmp(ops[2].a, "a,b") == 0);
    printf("PASS: line continuations\n");
}

static void test_shell_form_cmd(void) {
    int n = parse_ok("FROM alpine\nCMD /bin/sh -c 'echo x'\n");
    CHECK(n == 2);
    CHECK(ops[1].kind == BCMD && ops[1].json_form == 0);
    CHECK(strcmp(ops[1].a, "/bin/sh -c 'echo x'") == 0);
    printf("PASS: shell-form CMD\n");
}

static void test_errors(void) {
    int n;

    n = dockerfile_parse("RUN echo hi\n", ops, BUILD_OPS_MAX, err, sizeof(err));
    CHECK(n == -1 && strstr(err, "FROM") != NULL);

    n = dockerfile_parse("FROM alpine\nFROM alpine\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    /* Multiple FROM is accepted by the parser (multi-stage is a driver
     * concern); just ensure it parses. */
    CHECK(n == 2);

    n = dockerfile_parse("FROM alpine\nFROB x\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    CHECK(n == -1 && strstr(err, "unsupported") != NULL &&
          strstr(err, "line 2") != NULL);

    n = dockerfile_parse("FROM alpine\nRUN [\"/bin/ls\"]\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    CHECK(n == -1 && strstr(err, "exec form") != NULL);

    n = dockerfile_parse("FROM alpine\nENV JUSTKEY\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    CHECK(n == -1 && strstr(err, "KEY=VALUE") != NULL);

    n = dockerfile_parse("FROM alpine\nCOPY onlysrc\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    /* one token: no dst (docker: "COPY requires at least two arguments") */
    CHECK(n == -1 && strstr(err, "src and dst") != NULL);

    n = dockerfile_parse("FROM alpine\nRUN echo x \\\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    CHECK(n == -1 && strstr(err, "continuation") != NULL);

    n = dockerfile_parse("# only a comment\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    CHECK(n == -1 && strstr(err, "no instructions") != NULL);

    printf("PASS: error cases\n");
}

static void test_multistage(void) {
    int n = parse_ok(
        "FROM alpine:latest AS builder\n"
        "RUN make\n"
        "COPY --from=builder /src/app /usr/local/bin/app\n"
        "FROM scratch\n"
        "COPY --from=alpine:3 /bin/sh /bin/sh\n"
        "CMD [\"/usr/local/bin/app\"]\n");
    CHECK(n == 6);
    /* FROM ... AS name */
    CHECK(ops[0].kind == BFROM && strcmp(ops[0].a, "alpine:latest") == 0 &&
          strcmp(ops[0].b, "builder") == 0);
    /* FROM without AS leaves the stage name empty */
    CHECK(ops[3].kind == BFROM && strcmp(ops[3].a, "scratch") == 0 &&
          ops[3].b[0] == '\0');
    /* COPY --from=<stage>: flag captured, src is the stage path */
    CHECK(ops[2].kind == BCOPY && strcmp(ops[2].from, "builder") == 0 &&
          strcmp(ops[2].a, "/src/app") == 0 &&
          strcmp(ops[2].b, "/usr/local/bin/app") == 0);
    /* COPY --from=<image> */
    CHECK(ops[4].kind == BCOPY && strcmp(ops[4].from, "alpine:3") == 0 &&
          strcmp(ops[4].a, "/bin/sh") == 0);
    /* plain COPY keeps an empty --from */
    n = parse_ok("FROM alpine\nCOPY x /x\n");
    CHECK(n == 2 && ops[1].from[0] == '\0');
    /* multi-source COPY: everything but the last token is a source */
    n = parse_ok("FROM alpine\nWORKDIR /build\nCOPY go.mod go.sum ./\n");
    CHECK(n == 3);
    CHECK(ops[2].kind == BCOPY && strcmp(ops[2].a, "go.mod go.sum") == 0 &&
          strcmp(ops[2].b, "./") == 0);
    printf("PASS: multi-stage (FROM..AS, COPY --from)\n");
}

static void test_multistage_errors(void) {
    int n;

    n = dockerfile_parse("FROM alpine AS\n", ops, BUILD_OPS_MAX, err, sizeof(err));
    CHECK(n == -1 && strstr(err, "stage name") != NULL);

    n = dockerfile_parse("FROM alpine nope x\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    CHECK(n == -1 && strstr(err, "AS") != NULL);

    n = dockerfile_parse("FROM alpine\nCOPY --from= /x\n", ops, BUILD_OPS_MAX,
                         err, sizeof(err));
    CHECK(n == -1 && strstr(err, "--from") != NULL);

    n = dockerfile_parse("FROM alpine\nCOPY --chown=1:1 x /x\n", ops,
                         BUILD_OPS_MAX, err, sizeof(err));
    CHECK(n == -1 && strstr(err, "unsupported COPY flag") != NULL);

    printf("PASS: multi-stage error cases\n");
}

static void test_env_set(void) {
    char env[256];
    env[0] = '\0';
    CHECK(build_env_set(env, sizeof(env), "A=1") == 0);
    CHECK(strcmp(env, "A=1") == 0);
    CHECK(build_env_set(env, sizeof(env), "B=2") == 0);
    CHECK(strcmp(env, "A=1,B=2") == 0);
    /* later ENV for the same key replaces the earlier one */
    CHECK(build_env_set(env, sizeof(env), "A=9") == 0);
    CHECK(strcmp(env, "B=2,A=9") == 0);
    CHECK(build_env_set(env, sizeof(env), "NOEQ") == -1);
    printf("PASS: env set/override\n");
}

int main(void) {
    printf("Running build parser tests...\n\n");

    test_basic();
    test_continuation();
    test_shell_form_cmd();
    test_errors();
    test_multistage();
    test_multistage_errors();
    test_env_set();

    if (failures) {
        printf("\n%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("\nAll tests passed!\n");
    return 0;
}
