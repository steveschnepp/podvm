/*
 * sha256.h - compact SHA-256
 *
 * Hand-rolled so the registry pull can verify layer digests without
 * adding a libcrypto dependency. The project already carries only
 * curl (HTTP) and zlib (gunzip); integrity checking shouldn't change
 * that budget.
 */

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];        /* chaining state */
    uint64_t len;         /* total bytes hashed so far */
    unsigned char buf[64];
    size_t nbuf;          /* bytes currently buffered in buf */
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const void *data, size_t len);
/* out must hold 32 bytes. */
void sha256_final(SHA256_CTX *ctx, unsigned char out[32]);

/* One-shot: hash a buffer and write 64 hex chars + NUL to out[65]. */
void sha256_hex(const void *data, size_t len, char out[65]);

#endif /* SHA256_H */
