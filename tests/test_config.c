/*
 * test_config.c - Test config parsing using strparse
 *
 * Build: gcc -o test_config tests/test_config.c src/strparse.c -Isrc
 * Run: ./test_config
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "strparse.h"

#define MAX_ARGS  64
#define MAX_ENVS  128
#define PATH_SIZE 1024

/* Configuration from kernel cmdline */
typedef struct {
    char root[PATH_SIZE];
    char entrypoint[PATH_SIZE];
    char *args[MAX_ARGS];
    int  argc;
    char *envs[MAX_ENVS];
    int  envc;
    char cwd[PATH_SIZE];
    char hostname[256];
    int  uid;
    int  gid;
} Config;

/* Free config */
static void config_free(Config *cfg) {
    for (int i = 0; i < cfg->argc; i++)
        free(cfg->args[i]);
    for (int i = 0; i < cfg->envc; i++)
        free(cfg->envs[i]);
    memset(cfg, 0, sizeof(*cfg));
}

/* Parse config from kernel cmdline */
static void parse_cmdline(Config *cfg, const char *cmdline) {
    const char *val;
    char buf[4096];

    memset(cfg, 0, sizeof(*cfg));

    val = kv_find(cmdline, "podvm.root=");
    if (val) kv_copy_value(cfg->root, PATH_SIZE, val);

    val = kv_find(cmdline, "podvm.entrypoint=");
    if (val) kv_copy_value(cfg->entrypoint, PATH_SIZE, val);

    val = kv_find(cmdline, "podvm.args=");
    if (val) {
        /* Extract args value (until next space or end) */
        kv_copy_value(buf, sizeof(buf), val);
        cfg->argc = csv_parse(buf, cfg->args, MAX_ARGS);
    }

    val = kv_find(cmdline, "podvm.cwd=");
    if (val) kv_copy_value(cfg->cwd, PATH_SIZE, val);

    val = kv_find(cmdline, "podvm.hostname=");
    if (val) kv_copy_value(cfg->hostname, 256, val);

    val = kv_find(cmdline, "podvm.user=");
    parse_uid_gid(val, &cfg->uid, &cfg->gid);
}

/* Test kv_find */
static void test_kv_find(void) {
    const char *cmdline = "podvm.root=/rootfs podvm.entrypoint=/bin/sh podvm.args=-1,-2";
    const char *val;
    
    /* Found */
    val = kv_find(cmdline, "podvm.root=");
    assert(val != NULL);
    assert(strncmp(val, "/rootfs", 7) == 0);
    
    val = kv_find(cmdline, "podvm.entrypoint=");
    assert(val != NULL);
    assert(strncmp(val, "/bin/sh", 7) == 0);
    
    val = kv_find(cmdline, "podvm.args=");
    assert(val != NULL);
    assert(strncmp(val, "-1,-2", 5) == 0);
    
    /* Not found */
    assert(kv_find(cmdline, "podvm.nonexistent=") == NULL);
    assert(kv_find(cmdline, "other=") == NULL);
    
    /* Empty */
    assert(kv_find("", "podvm.root=") == NULL);
    
    printf("PASS: test_kv_find\n");
}

/* Test kv_copy_value */
static void test_kv_copy_value(void) {
    char buf[64];
    
    /* Normal copy */
    size_t len = kv_copy_value(buf, sizeof(buf), "/rootfs podvm.entrypoint=/bin/sh");
    assert(len == 7);
    assert(strcmp(buf, "/rootfs") == 0);
    
    /* Empty value */
    len = kv_copy_value(buf, sizeof(buf), " podvm.entrypoint=/bin/sh");
    assert(len == 0);
    assert(strcmp(buf, "") == 0);
    
    /* Truncation */
    len = kv_copy_value(buf, 4, "/rootfs");
    assert(len == 3);
    assert(strcmp(buf, "/ro") == 0);
    
    printf("PASS: test_kv_copy_value\n");
}

/* Test csv_parse */
static void test_csv_parse(void) {
    char *values[10];
    int count;
    
    /* Single value */
    count = csv_parse("hello", values, 10);
    assert(count == 1);
    assert(strcmp(values[0], "hello") == 0);
    csv_free(values, count);
    
    /* Multiple values */
    count = csv_parse("a,b,c", values, 10);
    assert(count == 3);
    assert(strcmp(values[0], "a") == 0);
    assert(strcmp(values[1], "b") == 0);
    assert(strcmp(values[2], "c") == 0);
    csv_free(values, count);
    
    /* Empty string */
    count = csv_parse("", values, 10);
    assert(count == 0);
    
    /* Max limit */
    count = csv_parse("1,2,3,4,5", values, 3);
    assert(count == 3);
    csv_free(values, count);
    
    printf("PASS: test_csv_parse\n");
}

/* Test parse_uid_gid */
static void test_parse_uid_gid(void) {
    int uid, gid;
    
    /* Valid */
    assert(parse_uid_gid("1000:1000", &uid, &gid) == 0);
    assert(uid == 1000);
    assert(gid == 1000);
    
    assert(parse_uid_gid("0:0", &uid, &gid) == 0);
    assert(uid == 0);
    assert(gid == 0);
    
    /* UID only */
    assert(parse_uid_gid("1000", &uid, &gid) == 0);
    assert(uid == 1000);
    assert(gid == 0);
    
    /* NULL */
    assert(parse_uid_gid(NULL, &uid, &gid) == -1);
    
    printf("PASS: test_parse_uid_gid\n");
}

/* Test parse_cmdline */
static void test_parse_cmdline(void) {
    Config cfg;
    
    /* Full config */
    parse_cmdline(&cfg, 
        "podvm.root=/rootfs "
        "podvm.entrypoint=/bin/sh "
        "podvm.args=-l,-c,-echoservice "
        "podvm.cwd=/tmp "
        "podvm.hostname=testvm "
        "podvm.user=1000:1000");
    
    assert(strcmp(cfg.root, "/rootfs") == 0);
    assert(strcmp(cfg.entrypoint, "/bin/sh") == 0);
    assert(cfg.argc == 3);
    assert(strcmp(cfg.args[0], "-l") == 0);
    assert(strcmp(cfg.args[1], "-c") == 0);
    assert(strcmp(cfg.args[2], "-echoservice") == 0);
    assert(strcmp(cfg.cwd, "/tmp") == 0);
    assert(strcmp(cfg.hostname, "testvm") == 0);
    assert(cfg.uid == 1000);
    assert(cfg.gid == 1000);
    
    config_free(&cfg);
    
    /* Minimal config */
    parse_cmdline(&cfg, "podvm.root=/pod podvm.entrypoint=/bin/true");
    assert(strcmp(cfg.root, "/pod") == 0);
    assert(strcmp(cfg.entrypoint, "/bin/true") == 0);
    assert(cfg.argc == 0);
    assert(cfg.cwd[0] == '\0');
    assert(cfg.hostname[0] == '\0');
    assert(cfg.uid == 0);
    assert(cfg.gid == 0);
    
    config_free(&cfg);
    
    /* Empty */
    parse_cmdline(&cfg, "");
    assert(cfg.root[0] == '\0');
    assert(cfg.entrypoint[0] == '\0');
    
    config_free(&cfg);

    /* Regression: args value must terminate at the token boundary.
     * A space starts the next podvm.*= token; it is not an arg
     * separator (the host joins args with commas). Found via the
     * podman-storage e2e: "podvm.args=OK podvm.env=HOME=/root" used
     * to yield args = {"OK", "podvm.env=HOME=/root"}. */
    parse_cmdline(&cfg,
        "podvm.root=/rootfs "
        "podvm.entrypoint=/bin/echo "
        "podvm.args=OK "
        "podvm.env=HOME=/root");
    assert(cfg.argc == 1);
    assert(strcmp(cfg.args[0], "OK") == 0);

    config_free(&cfg);

    printf("PASS: test_parse_cmdline\n");
}

/* Test escaping: values with spaces and commas survive the channel. */
static void test_escape(void) {
    char wire[256];
    char tok[256];
    char *items[8];
    int n;

    /* Round-trip a single value containing spaces. The wire form still
     * contains space bytes (an escaped space is backslash + space), but
     * the parser treats them as escaped, not as a token boundary. */
    assert(kv_escape(wire, sizeof(wire), "/My App Dir") > 0);
    assert(strcmp(wire, "/My\\ App\\ Dir") == 0);
    kv_copy_value(tok, sizeof(tok), wire);
    kv_unescape(tok);
    assert(strcmp(tok, "/My App Dir") == 0);

    /* Round-trip a value containing a comma. */
    assert(kv_escape(wire, sizeof(wire), "a,b") > 0);
    kv_copy_value(tok, sizeof(tok), wire);
    kv_unescape(tok);
    assert(strcmp(tok, "a,b") == 0);

    /* Round-trip a value containing a backslash. */
    assert(kv_escape(wire, sizeof(wire), "a\\b") > 0);
    kv_copy_value(tok, sizeof(tok), wire);
    kv_unescape(tok);
    assert(strcmp(tok, "a\\b") == 0);

    /* A list: escape each element, join with real commas. */
    char list[256]; size_t off = 0;
    const char *parts[] = {"hello world", "a,b", "plain"};
    for (int i = 0; i < 3; i++) {
        char esc[128];
        assert(kv_escape(esc, sizeof(esc), parts[i]) > 0);
        off += (size_t)snprintf(list + off, sizeof(list) - off, "%s%s",
                                i ? "," : "", esc);
    }
    n = csv_parse(list, items, 8);
    assert(n == 3);
    assert(strcmp(items[0], "hello world") == 0);
    assert(strcmp(items[1], "a,b") == 0);
    assert(strcmp(items[2], "plain") == 0);
    csv_free(items, n);

    printf("PASS: test_escape\n");
}

/* Test escape-aware token finding across embedded spaces. */
static void test_kv_find_escaped(void) {
    /* root's value contains an escaped space; entrypoint must still be
     * found after it. */
    const char *cmdline =
        "podvm.root=/My\\ App\\ Dir podvm.entrypoint=/bin/sh podvm.args=a\\ b,c";

    const char *val = kv_find(cmdline, "podvm.root=");
    assert(val != NULL);
    char tok[64];
    kv_copy_value(tok, sizeof(tok), val);
    kv_unescape(tok);
    assert(strcmp(tok, "/My App Dir") == 0);

    val = kv_find(cmdline, "podvm.entrypoint=");
    assert(val != NULL);
    kv_copy_value(tok, sizeof(tok), val);
    kv_unescape(tok);
    assert(strcmp(tok, "/bin/sh") == 0);

    /* args: an escaped space inside one arg, a real comma separator. */
    val = kv_find(cmdline, "podvm.args=");
    assert(val != NULL);
    kv_copy_value(tok, sizeof(tok), val);
    char *items[8];
    int n = csv_parse(tok, items, 8);
    assert(n == 2);
    assert(strcmp(items[0], "a b") == 0);
    assert(strcmp(items[1], "c") == 0);
    csv_free(items, n);

    printf("PASS: test_kv_find_escaped\n");
}

/* Test edge cases */
static void test_edge_cases(void) {
    Config cfg;
    
    /* Very long values */
    char long_root[PATH_SIZE + 100];
    memset(long_root, 'a', sizeof(long_root) - 1);
    long_root[sizeof(long_root) - 1] = '\0';
    
    char cmdline[PATH_SIZE + 200];
    snprintf(cmdline, sizeof(cmdline), "podvm.root=%s", long_root);
    
    parse_cmdline(&cfg, cmdline);
    assert(strlen(cfg.root) == PATH_SIZE - 1);  /* Truncated */
    
    config_free(&cfg);
    
    /* Special characters */
    parse_cmdline(&cfg, "podvm.root=/path/with spaces podvm.entrypoint=/bin/sh");
    assert(strcmp(cfg.root, "/path/with") == 0);  /* Space terminates value */
    assert(strcmp(cfg.entrypoint, "/bin/sh") == 0);
    
    config_free(&cfg);
    
    /* Multiple spaces */
    parse_cmdline(&cfg, "podvm.root=/rootfs   podvm.entrypoint=/bin/sh");
    assert(strcmp(cfg.root, "/rootfs") == 0);
    assert(strcmp(cfg.entrypoint, "/bin/sh") == 0);
    
    config_free(&cfg);
    
    printf("PASS: test_edge_cases\n");
}

int main(void) {
    printf("Running config parsing tests...\n\n");
    
    test_kv_find();
    test_kv_copy_value();
    test_csv_parse();
    test_parse_uid_gid();
    test_escape();
    test_kv_find_escaped();
    test_parse_cmdline();
    test_edge_cases();
    
    printf("\nAll tests passed!\n");
    return 0;
}
