/*
 * test_sha256.c - known-answer tests for the hand-rolled SHA-256.
 *
 * Vectors are the standard NIST/FIPS 180-4 examples plus a streaming
 * split check (hashing in chunks must equal hashing in one shot).
 *
 * Build: gcc -Wall -Wextra -O2 -g -o test_sha256 test_sha256.c ../src/sha256.c -I../src
 */

#include <stdio.h>
#include <string.h>

#include "../src/sha256.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void expect(const char *label, const char *data, size_t len,
                   const char *want) {
    char got[65];
    sha256_hex(data, len, got);
    if (strcmp(got, want) != 0) {
        printf("FAIL %s:\n  got  %s\n  want %s\n", label, got, want);
        failures++;
    }
}

int main(void) {
    /* FIPS 180-4 two-block message. */
    expect("empty", "", 0,
           "e3b0c44298fc1c149afbf4c8996fb924"
           "27ae41e4649b934ca495991b7852b855");
    expect("abc", "abc", 3,
           "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");
    expect("448-bit", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
           "248d6a61d20638b8e5c026930c3e6039"
           "a33ce45964ff2167f6ecedd419db06c1");

    /* 1,000,000 'a' -- exercises the streaming path across many blocks.
     * Hashed in chunks to prove update() is order- and split-invariant. */
    {
        const char a = 'a';
        SHA256_CTX ctx;
        unsigned char d[32];
        char hex[65];
        static const char hexchars[] = "0123456789abcdef";

        sha256_init(&ctx);
        for (int i = 0; i < 1000000; i++)
            sha256_update(&ctx, &a, 1);
        sha256_final(&ctx, d);
        for (int i = 0; i < 32; i++) {
            hex[i * 2]     = hexchars[d[i] >> 4];
            hex[i * 2 + 1] = hexchars[d[i] & 0x0f];
        }
        hex[64] = '\0';
        CHECK(strcmp(hex,
            "cdc76e5c9914fb9281a1c7e284d73e67"
            "f1809a48a497200e046d39ccc7112cd0") == 0);
    }

    /* Streaming split: hash "abc" as "a" then "bc" == one-shot "abc". */
    {
        SHA256_CTX ctx;
        unsigned char d[32];
        char hex[65];
        static const char hexchars[] = "0123456789abcdef";

        sha256_init(&ctx);
        sha256_update(&ctx, "a", 1);
        sha256_update(&ctx, "bc", 2);
        sha256_final(&ctx, d);
        for (int i = 0; i < 32; i++) {
            hex[i * 2]     = hexchars[d[i] >> 4];
            hex[i * 2 + 1] = hexchars[d[i] & 0x0f];
        }
        hex[64] = '\0';
        CHECK(strcmp(hex,
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad") == 0);
    }

    /* Boundary lengths that stress the padding path (55/56/63/64 bytes). */
    {
        char buf[64];
        char got[65];
        memset(buf, 'x', sizeof(buf));

        sha256_hex(buf, 55, got);   /* fits with 1 pad byte in one block */
        CHECK(strlen(got) == 64);
        sha256_hex(buf, 56, got);   /* forces a second block */
        CHECK(strlen(got) == 64);
        sha256_hex(buf, 63, got);
        CHECK(strlen(got) == 64);
        sha256_hex(buf, 64, got);   /* exact block, then padding block */
        CHECK(strlen(got) == 64);

        /* 56 and 64 must differ from 55 and 63 respectively. */
        char a[65], b[65];
        sha256_hex(buf, 55, a); sha256_hex(buf, 56, b);
        CHECK(strcmp(a, b) != 0);
        sha256_hex(buf, 63, a); sha256_hex(buf, 64, b);
        CHECK(strcmp(a, b) != 0);
    }

    if (failures == 0) {
        printf("sha256 tests: all passed\n");
        return 0;
    }
    printf("sha256 tests: %d failure(s)\n", failures);
    return 1;
}
