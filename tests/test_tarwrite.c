/*
 * test_tarwrite.c - tar pack/extract round-trip
 *
 * Packs a fixture tree with tar_pack_dir and extracts it with
 * tar_extract_mem (the reader moved out of image.c), verifying dirs,
 * nested files, symlinks, and a GNU-longname path survive the round trip.
 *
 * Build: gcc -o test_tarwrite tests/test_tarwrite.c src/tar.c -Isrc
 * Run: ./test_tarwrite
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tar.h"

static void mk(const char *p) { assert(mkdir(p, 0755) == 0); }

static void wf(const char *p, const char *content) {
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static char *rf(const char *p) {
    FILE *f = fopen(p, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = malloc((size_t)n + 1);
    assert(b);
    size_t got = fread(b, 1, (size_t)n, f);
    b[got] = '\0';
    fclose(f);
    return b;
}

static void same(const char *path, const char *want) {
    char *got = rf(path);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "MISMATCH %s: got '%s' want '%s'\n", path, got, want);
        free(got);
        exit(1);
    }
    free(got);
}

int main(void) {
    char tmpl[] = "/tmp/podvm-tarwrite-XXXXXX";
    char *T = mkdtemp(tmpl);
    assert(T);

    char src[512], dst[512], p[512];
    snprintf(src, sizeof(src), "%s/src", T);
    snprintf(dst, sizeof(dst), "%s/dst", T);
    mk(src);
    mk(dst);

    snprintf(p, sizeof(p), "%s/hello.txt", src);
    wf(p, "hello world\n");
    snprintf(p, sizeof(p), "%s/sub", src);
    mk(p);
    snprintf(p, sizeof(p), "%s/sub/nested.txt", src);
    wf(p, "nested\n");
    snprintf(p, sizeof(p), "%s/sub/link", src);
    assert(symlink("../hello.txt", p) == 0);
    snprintf(p, sizeof(p), "%s/emptydir", src);
    mk(p);

    /* A single path component longer than 100 chars forces the GNU
     * longname ('L') path in the writer. */
    char longname[160];
    memset(longname, 'a', 150);
    longname[150] = '\0';
    snprintf(p, sizeof(p), "%s/%s", src, longname);
    wf(p, "long\n");

    char tartar[512];
    snprintf(tartar, sizeof(tartar), "%s/out.tar", T);
    assert(tar_pack_dir(src, tartar) == 0);

    size_t len = 0;
    char *data = NULL;
    {
        FILE *f = fopen(tartar, "rb");
        assert(f);
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        rewind(f);
        data = malloc((size_t)n);
        assert(data);
        len = fread(data, 1, (size_t)n, f);
        fclose(f);
    }
    assert(tar_extract_mem(data, len, dst) == 0);
    free(data);

    snprintf(p, sizeof(p), "%s/hello.txt", dst);
    same(p, "hello world\n");
    snprintf(p, sizeof(p), "%s/sub/nested.txt", dst);
    same(p, "nested\n");
    snprintf(p, sizeof(p), "%s/sub/link", dst);
    char target[512];
    ssize_t tl = readlink(p, target, sizeof(target) - 1);
    assert(tl > 0);
    target[tl] = '\0';
    assert(strcmp(target, "../hello.txt") == 0);
    snprintf(p, sizeof(p), "%s/emptydir", dst);
    struct stat st;
    assert(stat(p, &st) == 0 && S_ISDIR(st.st_mode));
    snprintf(p, sizeof(p), "%s/%s", dst, longname);
    same(p, "long\n");

    printf("PASS: tar round-trip (dirs, files, symlink, long name)\n");
    return 0;
}
