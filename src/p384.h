/* NIST P-384: public key derivation, ECDSA signing (RFC 6979 nonces), ECDH. */
#ifndef P384_H
#define P384_H

#include <stddef.h>
#include <stdint.h>

#define P384_LEN 48

/* Returns 0 if d is a valid private scalar (1 <= d < n), -1 otherwise. */
int p384_check_scalar(const uint8_t d[P384_LEN]);

/* pub = 0x04 || X || Y. Returns 0 on success, -1 on an invalid scalar. */
int p384_pubkey(uint8_t pub[1 + 2 * P384_LEN], const uint8_t d[P384_LEN]);

/* ECDSA over a precomputed digest of any length (leftmost 384 bits are used).
 * sig = r || s. Returns 0 on success, -1 on an invalid scalar. */
int p384_sign(uint8_t sig[2 * P384_LEN], const uint8_t d[P384_LEN],
              const uint8_t *digest, size_t digest_len);

/* out = X coordinate of d * peer, peer = 0x04 || X || Y.
 * Returns 0 on success, -1 on an invalid scalar or point. */
int p384_ecdh(uint8_t out[P384_LEN], const uint8_t d[P384_LEN],
              const uint8_t peer[1 + 2 * P384_LEN]);

#endif
