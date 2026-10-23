/*
 * Cross-checks the firmware crypto against OpenSSL's libcrypto.
 * Build/run with `make -C test crypto`.
 */
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/des.h"
#include "../src/p384.h"
#include "../src/sha512.h"

static int failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            failures++;                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                   \
            fprintf(stderr, "\n");                          \
        }                                                   \
    } while (0)

static void rnd(void *p, size_t n)
{
    if (RAND_bytes(p, (int)n) != 1)
        abort();
}

static void test_sha(void)
{
    static uint8_t msg[1000];
    for (int iter = 0; iter < 300; iter++) {
        size_t len = (size_t)iter * 3 % sizeof(msg);
        uint8_t a[64], b[64];
        unsigned int blen;
        rnd(msg, len);

        for (int v = 0; v < 2; v++) {
            sha512_ctx c;
            size_t outlen = v ? 64 : 48;
            v ? sha512_init(&c) : sha384_init(&c);
            /* feed in uneven pieces */
            size_t off = 0, step = 1 + iter % 130;
            while (off < len) {
                size_t n = len - off < step ? len - off : step;
                sha512_update(&c, msg + off, n);
                off += n;
            }
            sha512_final(&c, a, outlen);
            EVP_Digest(msg, len, b, &blen, v ? EVP_sha512() : EVP_sha384(), NULL);
            CHECK(blen == outlen && memcmp(a, b, outlen) == 0, "sha%d len %zu", v ? 512 : 384, len);
        }

        uint8_t key[48];
        rnd(key, sizeof(key));
        hmac384_ctx h;
        hmac384_init(&h, key, 48);
        hmac384_update(&h, msg, len);
        hmac384_final(&h, a);
        HMAC(EVP_sha384(), key, 48, msg, len, b, &blen);
        CHECK(blen == 48 && memcmp(a, b, 48) == 0, "hmac len %zu", len);
    }
}

static void test_des(void)
{
    for (int iter = 0; iter < 2000; iter++) {
        uint8_t key[24], in[8], a[8], b[16];
        int outl = 0;
        rnd(key, sizeof(key));
        rnd(in, sizeof(in));
        des3_encrypt(a, in, key);

        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, EVP_des_ede3_ecb(), NULL, key, NULL);
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        EVP_EncryptUpdate(ctx, b, &outl, in, 8);
        EVP_CIPHER_CTX_free(ctx);
        CHECK(outl == 8 && memcmp(a, b, 8) == 0, "3des iter %d", iter);
    }
    /* Known answer: YubiKey default management key. */
    static const uint8_t mk[24] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8};
    static const uint8_t zero[8];
    uint8_t out[8];
    des3_encrypt(out, zero, mk);
    /* With K1 == K2 == K3 this degenerates to single DES of K1. */
    uint8_t ref[16];
    int outl = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_des_ede3_ecb(), NULL, mk, NULL);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_EncryptUpdate(ctx, ref, &outl, zero, 8);
    EVP_CIPHER_CTX_free(ctx);
    CHECK(memcmp(out, ref, 8) == 0, "3des default key");
}

static EC_GROUP *grp;
static BIGNUM *order;

static void bn_to48(uint8_t out[48], const BIGNUM *bn)
{
    BN_bn2binpad(bn, out, 48);
}

/* Random scalar in [1, n-1], biased towards edge cases on some iterations. */
static void pick_scalar(uint8_t d[48], int iter)
{
    BIGNUM *k = BN_new();
    switch (iter) {
    case 0: BN_one(k); break;
    case 1: BN_set_word(k, 2); break;
    case 2: BN_set_word(k, 15); break;
    case 3: BN_set_word(k, 16); break;
    case 4: BN_sub(k, order, BN_value_one()); break;
    case 5: BN_sub(k, order, BN_value_one()); BN_sub_word(k, 1); break;
    case 6: BN_rshift1(k, order); break;
    default:
        do {
            BN_rand_range(k, order);
        } while (BN_is_zero(k));
    }
    bn_to48(d, k);
    BN_free(k);
}

static EC_KEY *key_from_scalar(const uint8_t d[48])
{
    EC_KEY *ek = EC_KEY_new_by_curve_name(NID_secp384r1);
    BIGNUM *bd = BN_bin2bn(d, 48, NULL);
    EC_POINT *pub = EC_POINT_new(grp);
    EC_POINT_mul(grp, pub, bd, NULL, NULL, NULL);
    EC_KEY_set_private_key(ek, bd);
    EC_KEY_set_public_key(ek, pub);
    EC_POINT_free(pub);
    BN_free(bd);
    return ek;
}

static void test_pubkey_and_sign(void)
{
    for (int iter = 0; iter < 400; iter++) {
        uint8_t d[48], pub[97], ref[97], sig[96], digest[64];
        pick_scalar(d, iter);
        EC_KEY *ek = key_from_scalar(d);

        CHECK(p384_check_scalar(d) == 0, "scalar iter %d", iter);
        CHECK(p384_pubkey(pub, d) == 0, "pubkey ret iter %d", iter);
        EC_POINT_point2oct(grp, EC_KEY_get0_public_key(ek), POINT_CONVERSION_UNCOMPRESSED, ref, 97, NULL);
        CHECK(memcmp(pub, ref, 97) == 0, "pubkey iter %d", iter);

        static const size_t dlens[] = {48, 32, 20, 64, 28, 1, 0};
        size_t dlen = dlens[iter % 7];
        rnd(digest, sizeof(digest));
        if (iter == 7)
            memset(digest, 0xff, sizeof(digest)); /* e >= n, needs reduction */
        CHECK(p384_sign(sig, d, digest, dlen) == 0, "sign ret iter %d", iter);

        ECDSA_SIG *es = ECDSA_SIG_new();
        ECDSA_SIG_set0(es, BN_bin2bn(sig, 48, NULL), BN_bin2bn(sig + 48, 48, NULL));
        /* OpenSSL truncates long digests the same way (leftmost bits). */
        int ok = ECDSA_do_verify(digest, (int)dlen, es, ek);
        CHECK(ok == 1, "verify iter %d dlen %zu", iter, dlen);
        ECDSA_SIG_free(es);

        /* Deterministic: signing again gives the same bytes. */
        uint8_t sig2[96];
        p384_sign(sig2, d, digest, dlen);
        CHECK(memcmp(sig, sig2, 96) == 0, "determinism iter %d", iter);
        EC_KEY_free(ek);
    }
}

static void test_ecdh(void)
{
    for (int iter = 0; iter < 200; iter++) {
        uint8_t d[48], d2[48], peer[97], out[48], ref[48];
        pick_scalar(d, iter);
        pick_scalar(d2, iter + 11);
        EC_KEY *mine = key_from_scalar(d);
        EC_KEY *theirs = key_from_scalar(d2);
        EC_POINT_point2oct(grp, EC_KEY_get0_public_key(theirs), POINT_CONVERSION_UNCOMPRESSED, peer, 97, NULL);

        CHECK(p384_ecdh(out, d, peer) == 0, "ecdh ret iter %d", iter);
        int n = ECDH_compute_key(ref, sizeof(ref), EC_KEY_get0_public_key(theirs), mine, NULL);
        CHECK(n == 48 && memcmp(out, ref, 48) == 0, "ecdh iter %d", iter);

        /* Corrupt the point: off-curve, bad prefix, x >= p. */
        peer[96] ^= 1;
        CHECK(p384_ecdh(out, d, peer) == -1, "off-curve accepted iter %d", iter);
        peer[96] ^= 1;
        peer[0] = 0x02;
        CHECK(p384_ecdh(out, d, peer) == -1, "bad prefix accepted");
        peer[0] = 0x04;
        EC_KEY_free(mine);
        EC_KEY_free(theirs);
    }
    uint8_t d[48], peer[97], out[48];
    pick_scalar(d, 100);
    memset(peer, 0xff, sizeof(peer));
    peer[0] = 0x04;
    CHECK(p384_ecdh(out, d, peer) == -1, "x,y >= p accepted");
    memset(peer, 0, sizeof(peer));
    peer[0] = 0x04;
    CHECK(p384_ecdh(out, d, peer) == -1, "(0,0) accepted");
}

static void test_bad_scalars(void)
{
    uint8_t d[48], pub[97], sig[96], dig[48] = {0};
    BIGNUM *k = BN_new();

    memset(d, 0, 48);
    CHECK(p384_check_scalar(d) == -1 && p384_pubkey(pub, d) == -1 && p384_sign(sig, d, dig, 48) == -1, "zero scalar");
    bn_to48(d, order);
    CHECK(p384_check_scalar(d) == -1 && p384_pubkey(pub, d) == -1, "scalar == n");
    BN_add(k, order, BN_value_one());
    bn_to48(d, k);
    CHECK(p384_check_scalar(d) == -1, "scalar == n+1");
    memset(d, 0xff, 48);
    CHECK(p384_check_scalar(d) == -1, "scalar 2^384-1");
    BN_free(k);
}

/* RFC 6979 A.2.6, P-384 with SHA-384, message "sample". */
static void test_rfc6979(void)
{
    static const char *x =
        "6B9D3DAD2E1B8C1C05B19875B6659F4DE23C3B667BF297BA9AA47740787137D896D5724E4C70A825F872C9EA60D2EDF5";
    static const char *r =
        "94EDBB92A5ECB8AAD4736E56C691916B3F88140666CE9FA73D64C4EA95AD133C81A648152E44ACF96E36DD1E80FABE46";
    static const char *s =
        "99EF4AEB15F178CEA1FE40DB2603138F130E740A19624526203B6351D0A3A94FA329C145786E679E7B82C71A38628AC8";
    uint8_t d[48], dig[48], sig[96], want[96];
    unsigned int dl;
    BIGNUM *b = NULL;

    BN_hex2bn(&b, x);
    bn_to48(d, b);
    BN_hex2bn(&b, r);
    bn_to48(want, b);
    BN_hex2bn(&b, s);
    bn_to48(want + 48, b);
    BN_free(b);
    EVP_Digest("sample", 6, dig, &dl, EVP_sha384(), NULL);
    p384_sign(sig, d, dig, 48);
    CHECK(memcmp(sig, want, 96) == 0, "RFC 6979 vector");
}

int main(void)
{
    grp = EC_GROUP_new_by_curve_name(NID_secp384r1);
    order = BN_new();
    EC_GROUP_get_order(grp, order, NULL);

    test_sha();
    test_des();
    test_bad_scalars();
    test_rfc6979();
    test_pubkey_and_sign();
    test_ecdh();

    if (failures) {
        printf("crypto: %d failures\n", failures);
        return 1;
    }
    printf("crypto: all tests passed\n");
    return 0;
}
