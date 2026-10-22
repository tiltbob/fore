/* SHA-384/512 and HMAC-SHA-384. */
#ifndef SHA512_H
#define SHA512_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t h[8];
    uint64_t len;
    uint8_t buf[128];
    size_t fill;
} sha512_ctx;

void sha512_init(sha512_ctx *c);
void sha384_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, size_t len);
/* Writes the first outlen bytes of the digest (48 for SHA-384, 64 for SHA-512). */
void sha512_final(sha512_ctx *c, uint8_t *out, size_t outlen);

typedef struct {
    sha512_ctx inner, outer;
} hmac384_ctx;

/* keylen must be <= 128. */
void hmac384_init(hmac384_ctx *c, const uint8_t *key, size_t keylen);
void hmac384_update(hmac384_ctx *c, const void *data, size_t len);
void hmac384_final(hmac384_ctx *c, uint8_t out[48]);

#endif
