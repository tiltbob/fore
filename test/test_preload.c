/*
 * Preloaded factory key (make PRELOAD_KEY=...): built with the d = 4 header
 * from tools/gen-preload.py. The key and certificate appear on a blank device,
 * stay replaceable, survive reboots once replaced, and come back on reset.
 *
 * Built with FORE_LOCKED (make PRELOAD_LOCKED=1) it checks the opposite: no
 * command replaces or removes them, and nothing left in storage is used.
 */
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>

#include "../host/platform_host.h"
#include "../src/des.h"
#include "../src/piv.h"
#include "../src/store.h"
#include "preload.h"

#ifdef FORE_LOCKED
#define NAME "preload (locked)"
#else
#define NAME "preload"
#endif

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
        }                                                        \
    } while (0)

static uint8_t resp[PIV_MAX_RESPONSE];
static int rlen;

static unsigned send(const uint8_t *apdu, size_t len)
{
    rlen = (int)piv_apdu(apdu, len, resp);
    return (unsigned)(resp[rlen - 2] << 8 | resp[rlen - 1]);
}

#define SEND(...) send((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

/* GET DATA for the 9A certificate, following 61xx. Returns the length. */
static size_t get_cert(uint8_t *out)
{
    size_t n = 0;
    unsigned sw = SEND(0x00, 0xcb, 0x3f, 0xff, 0x05, 0x5c, 0x03, 0x5f, 0xc1, 0x05, 0x00);
    for (;;) {
        if ((sw & 0xff00) != 0x6100 && sw != 0x9000)
            return 0;
        memcpy(out + n, resp, (size_t)rlen - 2);
        n += (size_t)rlen - 2;
        if (sw == 0x9000)
            return n;
        sw = SEND(0x00, 0xc0, 0x00, 0x00, (uint8_t)sw);
    }
}

static EC_KEY *key4(void)
{
    EC_KEY *k = EC_KEY_new_by_curve_name(NID_secp384r1);
    const EC_GROUP *g = EC_KEY_get0_group(k);
    BIGNUM *d = BN_new();
    EC_POINT *q = EC_POINT_new(g);
    BN_set_word(d, 4);
    EC_POINT_mul(g, q, d, NULL, NULL, NULL);
    EC_KEY_set_public_key(k, q);
    EC_POINT_free(q);
    BN_free(d);
    return k;
}

static unsigned sign9a(const uint8_t digest[48])
{
    uint8_t cmd[59] = {0x00, 0x87, 0x14, 0x9a, 0x36, 0x7c, 0x34, 0x82, 0x00, 0x81, 0x30};
    memcpy(cmd + 11, digest, 48);
    return send(cmd, sizeof(cmd));
}

static int signed_by_4(const uint8_t digest[48])
{
    const uint8_t *der = resp + 4;
    ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &der, resp[3]);
    EC_KEY *k = key4();
    int ok = sig && ECDSA_do_verify(digest, 48, sig, k) == 1;
    ECDSA_SIG_free(sig);
    EC_KEY_free(k);
    return ok;
}

static void block_and_reset(void)
{
    for (int i = 0; i < 3; i++)
        SEND(0x00, 0x20, 0x00, 0x80, 0x08, '0', '0', '0', '0', '0', '0', 0xff, 0xff);
    for (int i = 0; i < 3; i++)
        SEND(0x00, 0x2c, 0x00, 0x80, 0x10, '0', '0', '0', '0', '0', '0', '0', '0',
             '1', '2', '3', '4', '5', '6', 0xff, 0xff);
    CHECK(SEND(0x00, 0xfb, 0x00, 0x00) == 0x9000, "factory reset");
}

static void check_factory_state(const char *when)
{
    static uint8_t cert[2048];
    uint8_t digest[48];
    size_t n;

    n = get_cert(cert);
    CHECK(n == sizeof(preload_cert) && memcmp(cert, preload_cert, n) == 0, "%s: preloaded certificate", when);
    memset(digest, 0x44, sizeof(digest));
    CHECK(sign9a(digest) == 0x6982, "%s: 9A still needs the PIN", when);
    CHECK(SEND(0x00, 0x20, 0x00, 0x80, 0x08, '1', '2', '3', '4', '5', '6', 0xff, 0xff) == 0x9000,
          "%s: default PIN", when);
    CHECK(sign9a(digest) == 0x9000 && signed_by_4(digest), "%s: 9A signs with d = 4", when);
}

/* Steps over a BER tag and length. */
static const uint8_t *skip_header(const uint8_t *p)
{
    p++;
    return p + (*p < 0x80 ? 1 : 1 + (*p & 0x7f));
}

int main(void)
{
    /* The certificate's public key is 4G. */
    const uint8_t *p = skip_header(skip_header(preload_cert)); /* 53 L { 70 L <cert> ... } */
    X509 *x = d2i_X509(NULL, &p, (long)(preload_cert + sizeof(preload_cert) - p));
    EC_KEY *k = key4();
    const EC_KEY *ck = x ? EVP_PKEY_get0_EC_KEY(X509_get0_pubkey(x)) : NULL;
    CHECK(PRELOAD_KEY_INDEX == 0 && PRELOAD_CERT_TAG == 0x5fc105u, "slot 9A");
    CHECK(ck && EC_POINT_cmp(EC_KEY_get0_group(k), EC_KEY_get0_public_key(ck), EC_KEY_get0_public_key(k), NULL) == 0,
          "certificate public key is 4G");
    X509_free(x);
    EC_KEY_free(k);

    host_flash_open(NULL);
    piv_init();
    check_factory_state("first boot");

    static const uint8_t mgm[24] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(SEND(0x00, 0x87, 0x03, 0x9b, 0x04, 0x7c, 0x02, 0x81, 0x00) == 0x9000, "challenge");
    uint8_t auth[17] = {0x00, 0x87, 0x03, 0x9b, 0x0c, 0x7c, 0x0a, 0x82, 0x08};
    des3_encrypt(auth + 9, resp + 4, mgm);
    CHECK(send(auth, sizeof(auth)) == 0x9000, "management key");
    uint8_t imp[55] = {0x00, 0xfe, 0x14, 0x9a, 0x32, 0x06, 0x30};
    imp[54] = 7;

#ifdef FORE_LOCKED
    /* Nothing may replace or remove the key or its certificate. */
    static const uint8_t slots[] = {0x9a, 0x9c, 0x9d, 0x9e};
    for (unsigned i = 0; i < sizeof(slots); i++) {
        imp[3] = slots[i];
        CHECK(send(imp, sizeof(imp)) == 0x6a81, "import into %02x refused", slots[i]);
    }
    CHECK(SEND(0x00, 0xdb, 0x3f, 0xff, 0x0a, 0x5c, 0x03, 0x5f, 0xc1, 0x05, 0x53, 0x03, 0x70, 0x01, 0x00) == 0x6a81,
          "certificate overwrite refused");
    CHECK(SEND(0x00, 0xdb, 0x3f, 0xff, 0x07, 0x5c, 0x03, 0x5f, 0xc1, 0x05, 0x53, 0x00) == 0x6a81,
          "certificate delete refused");
    CHECK(SEND(0x00, 0xdb, 0x3f, 0xff, 0x0a, 0x5c, 0x03, 0x5f, 0xc1, 0x0a, 0x53, 0x03, 0x70, 0x01, 0x00) == 0x6a81,
          "certificate for another slot refused");
    CHECK(SEND(0x00, 0xdb, 0x3f, 0xff, 0x0a, 0x5c, 0x03, 0x5f, 0xc1, 0x07, 0x53, 0x03, 0x01, 0x02, 0x03) == 0x9000,
          "other objects stay writable");

    /* Keys and certificates another build left in storage are never used. */
    for (int i = 0; i < NUM_KEYS; i++) {
        g_state.keys[i].present = 1;
        memcpy(g_state.keys[i].priv, imp + 7, 48); /* d = 7 */
    }
    store_save();
    store_put_object(store_object_index(0x5fc105), (const uint8_t *)"\x53\x03\x70\x01\x00", 5);
    store_put_object(store_object_index(0x5fc10a), (const uint8_t *)"\x53\x03\x70\x01\x00", 5);
    piv_init();
    check_factory_state("stale storage");
    CHECK(SEND(0x00, 0xcb, 0x3f, 0xff, 0x05, 0x5c, 0x03, 0x5f, 0xc1, 0x0a, 0x00) == 0x6a82,
          "stale 9C certificate hidden");
    CHECK(SEND(0x00, 0x87, 0x14, 0x9c, 0x36, 0x7c, 0x34, 0x82, 0x00, 0x81, 0x30, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
               11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
               34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48) == 0x6a88,
          "stale 9C key unusable");
#else
    /* Replace the key and drop the certificate; a reboot keeps the change. */
    static uint8_t cert[2048];
    uint8_t digest[48] = {9};
    CHECK(send(imp, sizeof(imp)) == 0x9000, "import d = 7");
    CHECK(SEND(0x00, 0xdb, 0x3f, 0xff, 0x07, 0x5c, 0x03, 0x5f, 0xc1, 0x05, 0x53, 0x00) == 0x9000, "delete cert");
    piv_init();
    CHECK(get_cert(cert) == 0, "deleted certificate stays deleted after reboot");
    SEND(0x00, 0x20, 0x00, 0x80, 0x08, '1', '2', '3', '4', '5', '6', 0xff, 0xff);
    CHECK(sign9a(digest) == 0x9000 && !signed_by_4(digest), "imported key replaced d = 4");
#endif

    block_and_reset();
    check_factory_state("after reset");

    if (failures) {
        printf(NAME ": %d failures\n", failures);
        return 1;
    }
    printf(NAME ": all tests passed\n");
    return 0;
}
