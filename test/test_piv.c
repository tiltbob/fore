/*
 * Applet-level tests: APDU parsing, security conditions, PIN policies,
 * management key mutual authentication, and storage robustness (torn state
 * writes, corrupted retry counters).
 */
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/platform_host.h"
#include "../src/des.h"
#include "../src/p384.h"
#include "../src/piv.h"
#include "../src/platform.h"
#include "../src/store.h"

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

static size_t unhex(uint8_t *out, const char *hex)
{
    size_t n = 0;
    while (*hex) {
        if (*hex == ' ') {
            hex++;
            continue;
        }
        sscanf(hex, "%2hhx", &out[n++]);
        hex += 2;
    }
    return n;
}

static unsigned send(const uint8_t *apdu, size_t len)
{
    rlen = (int)piv_apdu(apdu, len, resp);
    return (unsigned)(resp[rlen - 2] << 8 | resp[rlen - 1]);
}

static unsigned sendx(const char *hex)
{
    uint8_t buf[600];
    return send(buf, unhex(buf, hex));
}

static const uint8_t DEFAULT_MGM[24] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8};

static void des3_decrypt(uint8_t out[8], const uint8_t in[8], const uint8_t key[24])
{
    int n;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(c, EVP_des_ede3_ecb(), NULL, key, NULL);
    EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_DecryptUpdate(c, out, &n, in, 8);
    EVP_CIPHER_CTX_free(c);
}

/* yubico-piv-tool style mutual authentication. Returns the final SW. */
static unsigned mutual_auth(const uint8_t key[24], int corrupt)
{
    uint8_t cmd[32] = {0x00, 0x87, 0x03, 0x9b, 0x16, 0x7c, 0x14, 0x80, 0x08};
    uint8_t chal[8] = {1, 1, 2, 3, 5, 8, 13, 21}, want[8];
    unsigned sw = sendx("00 87 03 9b 04 7c 02 80 00");

    if (sw != 0x9000 || rlen != 14)
        return sw;
    des3_decrypt(cmd + 9, resp + 4, key);
    cmd[9] ^= (uint8_t)corrupt;
    cmd[17] = 0x81;
    cmd[18] = 0x08;
    memcpy(cmd + 19, chal, 8);
    sw = send(cmd, 27);
    if (sw == 0x9000) {
        des3_encrypt(want, chal, key);
        if (rlen != 14 || resp[2] != 0x82 || memcmp(resp + 4, want, 8) != 0)
            return 0xffff;
    }
    return sw;
}

static void import_key(uint8_t slot, const uint8_t d[48], int policy)
{
    uint8_t cmd[64] = {0x00, 0xfe, 0x14, slot, 50, 0x06, 0x30};
    size_t n = 55;
    memcpy(cmd + 7, d, 48);
    if (policy) {
        cmd[4] = 53;
        cmd[n++] = 0xaa;
        cmd[n++] = 0x01;
        cmd[n++] = (uint8_t)policy;
    }
    CHECK(send(cmd, n) == 0x9000, "import into %02x", slot);
}

static unsigned sign(uint8_t slot, const uint8_t *digest, size_t dlen)
{
    uint8_t cmd[128] = {0x00, 0x87, 0x14, slot, (uint8_t)(dlen + 6), 0x7c, (uint8_t)(dlen + 4), 0x82, 0x00,
                        0x81, (uint8_t)dlen};
    memcpy(cmd + 11, digest, dlen);
    return send(cmd, 11 + dlen);
}

static int verify_sig(const uint8_t d[48], const uint8_t *digest, size_t dlen)
{
    /* 7C L 82 L <DER> 90 00 */
    const uint8_t *der = resp + 4;
    long derlen = resp[3];
    ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &der, derlen);
    EC_KEY *k = EC_KEY_new_by_curve_name(NID_secp384r1);
    const EC_GROUP *g = EC_KEY_get0_group(k);
    BIGNUM *bd = BN_bin2bn(d, 48, NULL);
    EC_POINT *q = EC_POINT_new(g);
    int ok;

    EC_POINT_mul(g, q, bd, NULL, NULL, NULL);
    EC_KEY_set_public_key(k, q);
    ok = sig && resp[0] == 0x7c && resp[2] == 0x82 && ECDSA_do_verify(digest, (int)dlen, sig, k) == 1;
    ECDSA_SIG_free(sig);
    EC_POINT_free(q);
    BN_free(bd);
    EC_KEY_free(k);
    return ok;
}

static void fresh(void)
{
    host_flash_open(NULL);
    piv_init();
}

static void test_parsing(void)
{
    fresh();
    CHECK(sendx("00 a4 04 00 05 a0 00 00 03 08") == 0x9000 && rlen == 42, "SELECT by truncated AID");
    CHECK(sendx("00 a4 04 00 0b a0 00 00 03 08 00 00 10 00 01 00 00") == 0x9000, "SELECT by full AID");
    CHECK(sendx("00 a4 04 00 05 a0 00 00 03 09") == 0x6a82, "SELECT other AID");
    CHECK(sendx("00 a4 04 00 04 a0 00 00 03") == 0x6a82, "SELECT too short");
    CHECK(sendx("00 a4") == 0x6700, "short header");
    CHECK(sendx("00 cb 3f ff 00 00 05 5c 03 5f c1 02") == 0x6700, "extended APDU rejected");
    CHECK(sendx("00 cb 3f ff 05 5c 03 5f c1 02 00 00") == 0x6700, "Lc mismatch");
    CHECK(sendx("80 cb 3f ff 05 5c 03 5f c1 02 00") == 0x6e00, "proprietary class");
    CHECK(sendx("0c cb 3f ff 05 5c 03 5f c1 02 00") == 0x6e00, "secure messaging class");
    CHECK(sendx("00 47 00 9a 05 ac 03 80 01 14") == 0x6d00, "no on-card key generation");
    CHECK(sendx("00 c0 00 00 00") == 0x6985, "GET RESPONSE with nothing pending");
    CHECK(sendx("00 fd 00 00 00") == 0x9000 && rlen == 5 && resp[0] == PIV_VERSION_MAJOR, "GET VERSION");
    CHECK(sendx("00 f8 00 00 00") == 0x9000 && rlen == 6, "GET SERIAL");

    /* Le shorter than the data: 61xx and GET RESPONSE */
    CHECK(sendx("00 cb 3f ff 05 5c 03 5f c1 02 10") == 0x612d && rlen == 18, "Le-limited GET DATA");
    CHECK(sendx("00 c0 00 00 2d") == 0x9000 && rlen == 0x2f, "GET RESPONSE remainder");
    CHECK(sendx("00 cb 3f ff 03 5c 01 7e 00") == 0x9000 && resp[0] == 0x7e, "discovery object");
    CHECK(sendx("00 cb 3f ff 05 5c 03 5f c1 05 00") == 0x6a82, "absent certificate");
    CHECK(sendx("00 cb 3f ff 05 5c 03 5f c1 0c 00") == 0x6a82, "unsupported object");
    CHECK(sendx("00 cb 3f ff 04 5c 02 5f c1 00") == 0x6a82, "two-byte tag");
    CHECK(sendx("00 cb 3f ff 03 5d 01 7e 00") == 0x6a80, "GET DATA without 5C");
    CHECK(sendx("00 cb 3f fe 03 5c 01 7e 00") == 0x6a86, "GET DATA bad P2");
}

static void test_mgmt_and_put_data(void)
{
    static uint8_t big[2100];

    fresh();
    CHECK(sendx("00 db 3f ff 0a 5c 03 5f c1 07 53 03 01 02 03") == 0x6982, "PUT DATA needs mgmt auth");
    CHECK(mutual_auth(DEFAULT_MGM, 1) == 0x6982, "wrong witness rejected");
    CHECK(sendx("00 87 03 9b 16 7c 14 80 08 00 00 00 00 00 00 00 00 81 08 00 00 00 00 00 00 00 00") == 0x6982,
          "witness response without a witness");
    CHECK(mutual_auth(DEFAULT_MGM, 0) == 0x9000, "mutual auth with default key");
    CHECK(sendx("00 87 0a 9b 04 7c 02 80 00") == 0x6a86, "AES management key unsupported");

    CHECK(sendx("00 db 3f ff 0a 5c 03 5f c1 07 53 03 01 02 03") == 0x9000, "PUT DATA CCC");
    CHECK(sendx("00 cb 3f ff 05 5c 03 5f c1 07 00") == 0x9000 && rlen == 7 && resp[0] == 0x53, "GET DATA CCC");
    CHECK(sendx("00 db 3f ff 07 5c 03 5f c1 07 53 00") == 0x9000, "delete CCC");
    CHECK(sendx("00 cb 3f ff 05 5c 03 5f c1 07 00") == 0x6a82, "CCC gone");
    CHECK(sendx("00 db 3f ff 0a 5c 03 5f c1 07 53 04 01 02 03") == 0x6a80, "53 length overrun");
    CHECK(sendx("00 db 3f ff 0a 5c 03 5f c1 07 53 02 01 02 03") == 0x6a80, "trailing bytes");
    CHECK(sendx("00 db 3f ff 0a 5c 03 5f ff 00 53 03 01 02 03") == 0x6a84, "unsupported object");
    CHECK(sendx("00 db 3f ff 03 5c 01 7e") == 0x6a80, "discovery is read-only");

    /* Object that does not fit two flash pages: chain it in and expect 6A84. */
    size_t n = 0;
    memcpy(big, "\x5c\x03\x5f\xc1\x05\x53\x82\x07\xf9", 9); /* 0x7f9 = 2041 bytes of content */
    n = 9 + 2041;
    for (size_t off = 0; off < n;) {
        uint8_t cmd[260];
        size_t k = n - off > 255 ? 255 : n - off;
        cmd[0] = off + k < n ? 0x10 : 0x00;
        memcpy(cmd + 1, "\xdb\x3f\xff", 3);
        cmd[4] = (uint8_t)k;
        memcpy(cmd + 5, big + off, k);
        unsigned sw = send(cmd, 5 + k);
        off += k;
        if (off < n)
            CHECK(sw == 0x9000, "chain piece");
        else
            CHECK(sw == 0x6a84, "oversized object rejected (%04x)", sw);
    }
    /* The largest object that fits. */
    big[8] = 0xf8;
    n = 9 + 2040;
    for (size_t off = 0; off < n;) {
        uint8_t cmd[260];
        size_t k = n - off > 255 ? 255 : n - off;
        cmd[0] = off + k < n ? 0x10 : 0x00;
        memcpy(cmd + 1, "\xdb\x3f\xff", 3);
        cmd[4] = (uint8_t)k;
        memcpy(cmd + 5, big + off, k);
        CHECK(send(cmd, 5 + k) == 0x9000, "max object piece");
        off += k;
    }
    /* A chain interrupted by another command is dropped. */
    CHECK(sendx("10 db 3f ff 03 5c 03 5f") == 0x9000, "chain start");
    CHECK(sendx("00 cb 3f ff 03 5c 01 7e 00") == 0x9000, "other command mid-chain");
    CHECK(sendx("00 db 3f ff 06 c1 07 53 02 aa bb") == 0x6a80, "orphan chain tail treated as new command");

    /* SET MANAGEMENT KEY then authenticate with it. */
    uint8_t cmd[40] = {0x00, 0xff, 0xff, 0xff, 27, 0x03, 0x9b, 0x18};
    uint8_t newkey[24];
    for (int i = 0; i < 24; i++)
        newkey[i] = (uint8_t)(0xa0 + i);
    memcpy(cmd + 8, newkey, 24);
    CHECK(send(cmd, 32) == 0x9000, "set management key");
    piv_reset();
    CHECK(mutual_auth(DEFAULT_MGM, 0) == 0x6982, "old management key rejected");
    CHECK(mutual_auth(newkey, 0) == 0x9000, "new management key accepted");
}

static const uint8_t D1[48] = {
    0x6b, 0x9d, 0x3d, 0xad, 0x2e, 0x1b, 0x8c, 0x1c, 0x05, 0xb1, 0x98, 0x75, 0xb6, 0x65, 0x9f, 0x4d,
    0xe2, 0x3c, 0x3b, 0x66, 0x7b, 0xf2, 0x97, 0xba, 0x9a, 0xa4, 0x77, 0x40, 0x78, 0x71, 0x37, 0xd8,
    0x96, 0xd5, 0x72, 0x4e, 0x4c, 0x70, 0xa8, 0x25, 0xf8, 0x72, 0xc9, 0xea, 0x60, 0xd2, 0xed, 0xf5};

static void test_keys_and_policies(void)
{
    uint8_t digest[64], cmd[128];

    fresh();
    memset(digest, 0x42, sizeof(digest));
    memset(cmd, 0, sizeof(cmd));
    memcpy(cmd, "\x00\xfe\x14\x9a\x32\x06\x30", 7);
    memcpy(cmd + 7, D1, 48);
    CHECK(send(cmd, 55) == 0x6982, "import needs mgmt auth");
    CHECK(mutual_auth(DEFAULT_MGM, 0) == 0x9000, "auth");

    /* Bad scalars and parameters */
    memset(cmd + 7, 0, 48);
    CHECK(send(cmd, 55) == 0x6a80, "zero scalar rejected");
    memset(cmd + 7, 0xff, 48);
    CHECK(send(cmd, 55) == 0x6a80, "scalar >= n rejected");
    memcpy(cmd + 7, D1, 48);
    cmd[2] = 0x11;
    CHECK(send(cmd, 55) == 0x6a80, "P-256 not supported");
    cmd[2] = 0x14;
    cmd[3] = 0x9b;
    CHECK(send(cmd, 55) == 0x6a86, "cannot import into 9B");
    cmd[3] = 0x9a;
    memcpy(cmd + 55, "\xab\x01\x02", 3);
    cmd[4] = 53;
    CHECK(send(cmd, 58) == 0x6a80, "touch policy 'always' refused (no button)");

    import_key(0x9a, D1, 0);              /* default: PIN once */
    import_key(0x9c, D1, 0);              /* default: PIN always */
    import_key(0x9d, D1, 1);              /* explicit: never */
    import_key(0x9e, D1, 0);              /* default: never */

    CHECK(sign(0x9a, digest, 48) == 0x6982, "9A needs the PIN");
    CHECK(sign(0x9e, digest, 48) == 0x9000 && verify_sig(D1, digest, 48), "9E signs without PIN");
    CHECK(sign(0x9d, digest, 32) == 0x9000 && verify_sig(D1, digest, 32), "9D with policy never, 32-byte digest");
    CHECK(sign(0x82, digest, 48) == 0x6a88, "retired slot unsupported");

    CHECK(sendx("00 20 00 80 08 31 32 33 34 35 36 ff ff") == 0x9000, "VERIFY");
    CHECK(sendx("00 20 00 80 00") == 0x9000, "VERIFY status: verified");
    CHECK(sign(0x9a, digest, 48) == 0x9000 && verify_sig(D1, digest, 48), "9A after VERIFY");
    CHECK(sign(0x9a, digest, 64) == 0x9000 && verify_sig(D1, digest, 64), "9A, 64-byte digest truncated");
    CHECK(sign(0x9a, digest, 48) == 0x9000, "PIN once: still fine");
    CHECK(sign(0x9c, digest, 48) == 0x9000 && verify_sig(D1, digest, 48), "9C right after VERIFY");
    CHECK(sign(0x9c, digest, 48) == 0x6982, "9C needs a fresh VERIFY each time");
    CHECK(sendx("00 20 00 80 08 31 32 33 34 35 36 ff ff") == 0x9000, "VERIFY again");
    CHECK(sign(0x9c, digest, 48) == 0x9000, "9C after second VERIFY");
    CHECK(sendx("00 20 ff 80") == 0x9000, "VERIFY reset");
    CHECK(sign(0x9a, digest, 48) == 0x6982, "PIN state cleared");

    /* Malformed GENERAL AUTHENTICATE */
    CHECK(sendx("00 87 11 9e 06 7c 04 82 00 81 00") == 0x6a86, "wrong algorithm");
    CHECK(sendx("00 87 14 9e 05 7c 03 82 00 81") == 0x6a80, "truncated TLV");
    CHECK(sendx("00 87 14 9e 04 7c 02 81 00") == 0x6a80, "missing response tag");
    CHECK(sendx("00 87 14 9e 06 7c 04 82 00 81 00") == 0x6a80, "empty challenge");
    CHECK(sendx("00 87 14 9e 08 7c 06 82 00 81 01 01 99") == 0x6a80, "trailing garbage in template");
    CHECK(sendx("00 87 14 9e 08 7c 06 82 00 82 00 81 00") == 0x6a80, "duplicate tag");

    /* ECDH */
    uint8_t peer_d[48], peer[97], want[48];
    memcpy(peer_d, D1, 48);
    peer_d[47] ^= 0x5a;
    p384_pubkey(peer, peer_d);
    uint8_t mine[97];
    p384_pubkey(mine, D1);
    p384_ecdh(want, peer_d, mine);
    memcpy(cmd, "\x00\x87\x14\x9e\x67\x7c\x65\x82\x00\x85\x61", 11);
    memcpy(cmd + 11, peer, 97);
    CHECK(send(cmd, 108) == 0x9000 && rlen == 54 && memcmp(resp + 4, want, 48) == 0, "ECDH on 9E");
    cmd[108 - 1] ^= 1;
    CHECK(send(cmd, 108) == 0x6a80, "ECDH with an off-curve point");
}

static void test_pins(void)
{
    fresh();
    CHECK(sendx("00 20 00 80 00") == 0x63c3, "3 tries initially");
    CHECK(sendx("00 20 00 81 08 31 32 33 34 35 36 37 38") == 0x6a88, "VERIFY on PUK unsupported");
    CHECK(sendx("00 20 00 80 06 31 32 33 34 35 36") == 0x6a80, "unpadded PIN rejected");
    CHECK(sendx("00 20 00 80 00") == 0x63c3, "bad length does not cost a try");
    CHECK(sendx("00 20 00 80 08 31 31 31 31 31 31 ff ff") == 0x63c2, "wrong PIN");
    piv_init(); /* power cycle: counters live in flash */
    CHECK(sendx("00 20 00 80 00") == 0x63c2, "counter persisted");
    CHECK(sendx("00 24 00 80 10 31 32 33 34 35 36 ff ff 39 39 39 ff ff ff ff ff") == 0x6a80,
          "new PIN too short");
    CHECK(sendx("00 24 00 80 10 31 32 33 34 35 36 ff ff 39 ff 39 39 39 39 39 ff") == 0x6a80,
          "padding inside new PIN");
    CHECK(sendx("00 24 00 80 10 31 32 33 34 35 36 ff ff 39 39 39 39 39 39 ff ff") == 0x9000, "change PIN");
    CHECK(sendx("00 20 00 80 00") == 0x9000, "changing the PIN verifies it");
    CHECK(sendx("00 20 00 80 08 31 31 31 31 31 31 ff ff") == 0x63c2, "wrong");
    CHECK(sendx("00 20 00 80 00") == 0x63c2, "failed VERIFY clears verification");
    CHECK(sendx("00 20 00 80 08 31 31 31 31 31 31 ff ff") == 0x63c1, "wrong");
    CHECK(sendx("00 20 00 80 08 31 31 31 31 31 31 ff ff") == 0x63c0, "wrong, blocked");
    CHECK(sendx("00 20 00 80 08 39 39 39 39 39 39 ff ff") == 0x6983, "right PIN refused once blocked");
    CHECK(sendx("00 fb 00 00") == 0x6985, "reset refused while PUK usable");
    CHECK(sendx("00 2c 00 80 10 31 32 33 34 35 36 37 39 31 32 33 34 35 36 ff ff") == 0x63c2, "wrong PUK");
    CHECK(sendx("00 2c 00 80 10 31 32 33 34 35 36 37 38 31 32 33 34 35 36 ff ff") == 0x9000, "unblock");
    CHECK(sendx("00 20 00 80 00") == 0x63c3, "PIN tries restored");
    CHECK(sendx("00 24 00 81 10 31 32 33 34 35 36 37 38 38 37 36 35 34 33 32 31") == 0x9000, "change PUK");
    CHECK(sendx("00 2c 00 80 10 31 32 33 34 35 36 37 38 31 32 33 34 35 36 ff ff") == 0x63c2, "old PUK fails");
}

static void test_storage_robustness(void)
{
    uint8_t *mem;

    fresh();
    CHECK(mutual_auth(DEFAULT_MGM, 0) == 0x9000, "auth");
    import_key(0x9e, D1, 0);
    CHECK(sendx("00 24 00 80 10 31 32 33 34 35 36 ff ff 31 31 31 31 31 31 31 31") == 0x9000, "change PIN");

    /* Tear the newest state copy: the older one (before the PIN change) wins. */
    mem = host_flash_mem();
    unsigned newest = ((const uint32_t *)mem)[1] > ((const uint32_t *)(mem + FLASH_PAGE_SIZE))[1] ? 0 : 1;
    mem[newest * FLASH_PAGE_SIZE + 100] ^= 0x01;
    piv_init();
    CHECK(sendx("00 20 00 80 08 31 32 33 34 35 36 ff ff") == 0x9000, "fell back to the previous state");
    uint8_t digest[48] = {1};
    CHECK(sign(0x9e, digest, 48) == 0x9000, "key from the previous state still there");

    /* Garbage in a retry counter page blocks that reference instead of resetting it. */
    mem[2 * FLASH_PAGE_SIZE + 10] = 0x12;
    piv_init();
    CHECK(sendx("00 20 00 80 00") == 0x6983, "corrupt PIN counter fails closed");
    CHECK(sendx("00 2c 00 80 10 31 32 33 34 35 36 37 38 31 32 33 34 35 36 ff ff") == 0x9000,
          "PUK still unblocks");
    CHECK(sendx("00 20 00 80 08 31 32 33 34 35 36 ff ff") == 0x9000, "PIN works again");

    /* Both state copies gone: back to factory defaults, objects wiped. */
    memset(mem, 0x00, 2 * FLASH_PAGE_SIZE);
    piv_init();
    CHECK(sign(0x9e, digest, 48) == 0x6a88, "no keys after state loss");
    CHECK(sendx("00 20 00 80 00") == 0x63c3, "defaults restored");
}

int main(void)
{
    test_parsing();
    test_mgmt_and_put_data();
    test_keys_and_policies();
    test_pins();
    test_storage_robustness();
    if (failures) {
        printf("piv: %d failures\n", failures);
        return 1;
    }
    printf("piv: all tests passed\n");
    return 0;
}
