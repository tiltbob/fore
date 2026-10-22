/*
 * NIST P-384 for a 32-bit MCU.
 *
 * Field and scalar arithmetic use 12x32-bit limbs in Montgomery form. Point
 * arithmetic uses the complete projective addition formula for a = -3 from
 * Renes, Costello and Batina (eprint 2015/1060, algorithm 4), which also
 * handles doubling and the point at infinity, so the fixed-window scalar
 * multiplication below has no secret-dependent branches or table indices.
 */
#include "p384.h"

#include <string.h>

#include "sha512.h"

#define NL 12

typedef uint32_t fe[NL];

typedef struct {
    uint32_t m[NL];
    uint32_t rr[NL]; /* R^2 mod m, R = 2^384 */
    uint32_t m0inv;  /* -m^-1 mod 2^32 */
} modulus;

typedef struct {
    fe x, y, z;
} point;

static const modulus P = {
    {0xffffffff, 0x00000000, 0x00000000, 0xffffffff, 0xfffffffe, 0xffffffff,
     0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
    {0x00000001, 0xfffffffe, 0x00000000, 0x00000002, 0x00000000, 0xfffffffe,
     0x00000000, 0x00000002, 0x00000001, 0x00000000, 0x00000000, 0x00000000},
    0x00000001,
};

static const modulus N = {
    {0xccc52973, 0xecec196a, 0x48b0a77a, 0x581a0db2, 0xf4372ddf, 0xc7634d81,
     0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
    {0x19b409a9, 0x2d319b24, 0xdf1aa419, 0xff3d81e5, 0xfcb82947, 0xbc3e483a,
     0x4aab1cc5, 0xd40d4917, 0x28266895, 0x3fb05b7a, 0x2b39bf21, 0x0c84ee01},
    0xe88fdc45,
};

static const fe CURVE_B = {
    0xd3ec2aef, 0x2a85c8ed, 0x8a2ed19d, 0xc656398d, 0x5013875a, 0x0314088f,
    0xfe814112, 0x181d9c6e, 0xe3f82d19, 0x988e056b, 0xe23ee7e4, 0xb3312fa7,
};

static const fe GX = {
    0x72760ab7, 0x3a545e38, 0xbf55296c, 0x5502f25d, 0x82542a38, 0x59f741e0,
    0x8ba79b98, 0x6e1d3b62, 0xf320ad74, 0x8eb1c71e, 0xbe8b0537, 0xaa87ca22,
};

static const fe GY = {
    0x90ea0e5f, 0x7a431d7c, 0x1d7e819d, 0x0a60b1ce, 0xb5f0b8c0, 0xe9da3113,
    0x289a147c, 0xf8f41dbd, 0x9292dc29, 0x5d9e98bf, 0x96262c6f, 0x3617de4a,
};

static const fe ONE = {1};

/* Curve b in Montgomery form, set by curve_setup(). */
static fe b_mont;

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

static uint32_t add_n(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint64_t c = 0;
    for (int i = 0; i < NL; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

static uint32_t sub_n(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint64_t c = 0;
    for (int i = 0; i < NL; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - c;
        r[i] = (uint32_t)t;
        c = (t >> 32) & 1;
    }
    return (uint32_t)c;
}

/* r = a if mask is all ones, unchanged if mask is zero. */
static void cmov(uint32_t *r, const uint32_t *a, uint32_t mask)
{
    for (int i = 0; i < NL; i++)
        r[i] ^= mask & (r[i] ^ a[i]);
}

/* 1 if a < b, else 0. */
static uint32_t lt(const uint32_t *a, const uint32_t *b)
{
    fe t;
    return sub_n(t, a, b);
}

static uint32_t is_zero(const uint32_t *a)
{
    uint32_t x = 0;
    for (int i = 0; i < NL; i++)
        x |= a[i];
    return ((x | (0u - x)) >> 31) ^ 1;
}

static uint32_t equal(const uint32_t *a, const uint32_t *b)
{
    fe t;
    for (int i = 0; i < NL; i++)
        t[i] = a[i] ^ b[i];
    return is_zero(t);
}

/* a = a mod m, for a < 2m. */
static void reduce_once(uint32_t *a, const modulus *M)
{
    fe t;
    uint32_t bw = sub_n(t, a, M->m);
    cmov(a, t, bw - 1);
}

static void mod_add(uint32_t *r, const uint32_t *a, const uint32_t *b, const modulus *M)
{
    fe t;
    uint32_t c = add_n(r, a, b);
    uint32_t bw = sub_n(t, r, M->m);
    cmov(r, t, 0u - (c | (bw ^ 1)));
}

static void mod_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, const modulus *M)
{
    fe t;
    uint32_t bw = sub_n(r, a, b);
    add_n(t, r, M->m);
    cmov(r, t, 0u - bw);
}

/* Montgomery multiplication (CIOS): r = a * b / R mod m, for a, b < m. */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, const modulus *M)
{
    uint32_t t[NL + 2];

    memset(t, 0, sizeof(t));
    for (int i = 0; i < NL; i++) {
        uint64_t c = 0;
        for (int j = 0; j < NL; j++) {
            c += (uint64_t)a[i] * b[j] + t[j];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        c += t[NL];
        t[NL] = (uint32_t)c;
        t[NL + 1] = (uint32_t)(c >> 32);

        uint32_t u = t[0] * M->m0inv;
        c = ((uint64_t)u * M->m[0] + t[0]) >> 32;
        for (int j = 1; j < NL; j++) {
            c += (uint64_t)u * M->m[j] + t[j];
            t[j - 1] = (uint32_t)c;
            c >>= 32;
        }
        c += t[NL];
        t[NL - 1] = (uint32_t)c;
        t[NL] = t[NL + 1] + (uint32_t)(c >> 32);
    }
    /* t < 2m; subtract m unless t (including the top word) is below m. */
    uint32_t bw = sub_n(r, t, M->m);
    cmov(r, t, 0u - (bw & (t[NL] ^ 1)));
}

static void to_mont(uint32_t *r, const uint32_t *a, const modulus *M)
{
    mont_mul(r, a, M->rr, M);
}

static void from_mont(uint32_t *r, const uint32_t *a, const modulus *M)
{
    mont_mul(r, a, ONE, M);
}

/* r = a^(m-2) = a^-1 (Montgomery form in and out). The exponent is public. */
static void mod_inv(uint32_t *r, const uint32_t *a, const modulus *M)
{
    static const fe TWO = {2};
    fe e, x;

    sub_n(e, M->m, TWO);
    to_mont(x, ONE, M);
    for (int i = 32 * NL - 1; i >= 0; i--) {
        mont_mul(x, x, x, M);
        if ((e[i / 32] >> (i % 32)) & 1)
            mont_mul(x, x, a, M);
    }
    memcpy(r, x, sizeof(x));
}

static void fe_from_bytes(uint32_t *r, const uint8_t *in)
{
    for (int i = 0; i < NL; i++) {
        const uint8_t *p = in + 4 * (NL - 1 - i);
        r[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
}

static void fe_to_bytes(uint8_t *out, const uint32_t *a)
{
    for (int i = 0; i < NL; i++) {
        uint8_t *p = out + 4 * (NL - 1 - i);
        p[0] = (uint8_t)(a[i] >> 24);
        p[1] = (uint8_t)(a[i] >> 16);
        p[2] = (uint8_t)(a[i] >> 8);
        p[3] = (uint8_t)a[i];
    }
}

static void curve_setup(void)
{
    to_mont(b_mont, CURVE_B, &P);
}

/* r = p + q; complete, so p == q and the point at infinity are fine. */
static void point_add(point *r, const point *p, const point *q)
{
    const modulus *M = &P;
    fe t0, t1, t2, t3, t4, x3, y3, z3;

    mont_mul(t0, p->x, q->x, M);
    mont_mul(t1, p->y, q->y, M);
    mont_mul(t2, p->z, q->z, M);
    mod_add(t3, p->x, p->y, M);
    mod_add(t4, q->x, q->y, M);
    mont_mul(t3, t3, t4, M);
    mod_add(t4, t0, t1, M);
    mod_sub(t3, t3, t4, M);
    mod_add(t4, p->y, p->z, M);
    mod_add(x3, q->y, q->z, M);
    mont_mul(t4, t4, x3, M);
    mod_add(x3, t1, t2, M);
    mod_sub(t4, t4, x3, M);
    mod_add(x3, p->x, p->z, M);
    mod_add(y3, q->x, q->z, M);
    mont_mul(x3, x3, y3, M);
    mod_add(y3, t0, t2, M);
    mod_sub(y3, x3, y3, M);
    mont_mul(z3, b_mont, t2, M);
    mod_sub(x3, y3, z3, M);
    mod_add(z3, x3, x3, M);
    mod_add(x3, x3, z3, M);
    mod_sub(z3, t1, x3, M);
    mod_add(x3, t1, x3, M);
    mont_mul(y3, b_mont, y3, M);
    mod_add(t1, t2, t2, M);
    mod_add(t2, t1, t2, M);
    mod_sub(y3, y3, t2, M);
    mod_sub(y3, y3, t0, M);
    mod_add(t1, y3, y3, M);
    mod_add(y3, t1, y3, M);
    mod_add(t1, t0, t0, M);
    mod_add(t0, t1, t0, M);
    mod_sub(t0, t0, t2, M);
    mont_mul(t1, t4, y3, M);
    mont_mul(t2, t0, y3, M);
    mont_mul(y3, x3, z3, M);
    mod_add(y3, y3, t2, M);
    mont_mul(x3, t3, x3, M);
    mod_sub(x3, x3, t1, M);
    mont_mul(z3, t4, z3, M);
    mont_mul(t1, t3, t0, M);
    mod_add(z3, z3, t1, M);

    memcpy(r->x, x3, sizeof(fe));
    memcpy(r->y, y3, sizeof(fe));
    memcpy(r->z, z3, sizeof(fe));
}

/* r = k * p with a fixed 4-bit window and constant-time table lookups. */
static void scalar_mult(point *r, const point *p, const uint32_t *k)
{
    point tbl[16], acc, sel;
    uint32_t *s = (uint32_t *)&sel;

    memset(&tbl[0], 0, sizeof(point));
    to_mont(tbl[0].y, ONE, &P);
    tbl[1] = *p;
    for (int i = 2; i < 16; i++)
        point_add(&tbl[i], &tbl[i - 1], p);

    acc = tbl[0];
    for (int w = 32 * NL / 4 - 1; w >= 0; w--) {
        for (int i = 0; i < 4; i++)
            point_add(&acc, &acc, &acc);
        uint32_t idx = (k[w / 8] >> ((w % 8) * 4)) & 15;
        memset(&sel, 0, sizeof(sel));
        for (uint32_t j = 0; j < 16; j++) {
            uint32_t d = j ^ idx;
            uint32_t mask = ((d | (0u - d)) >> 31) - 1;
            const uint32_t *t = (const uint32_t *)&tbl[j];
            for (unsigned l = 0; l < sizeof(point) / 4; l++)
                s[l] |= mask & t[l];
        }
        point_add(&acc, &acc, &sel);
    }
    *r = acc;
    wipe(tbl, sizeof(tbl));
    wipe(&sel, sizeof(sel));
}

static void base_point(point *g)
{
    to_mont(g->x, GX, &P);
    to_mont(g->y, GY, &P);
    to_mont(g->z, ONE, &P);
}

/* Affine coordinates in normal form. Returns -1 for the point at infinity. */
static int point_to_affine(uint32_t *x, uint32_t *y, const point *p)
{
    fe zi, t;

    mod_inv(zi, p->z, &P);
    mont_mul(t, p->x, zi, &P);
    from_mont(x, t, &P);
    mont_mul(t, p->y, zi, &P);
    from_mont(y, t, &P);
    return is_zero(p->z) ? -1 : 0;
}

/* Parses an uncompressed point and checks that it is on the curve. */
static int point_from_bytes(point *p, const uint8_t *in)
{
    fe x, y, t1, t2;

    if (in[0] != 0x04)
        return -1;
    fe_from_bytes(x, in + 1);
    fe_from_bytes(y, in + 1 + P384_LEN);
    if (!lt(x, P.m) || !lt(y, P.m))
        return -1;
    to_mont(p->x, x, &P);
    to_mont(p->y, y, &P);
    to_mont(p->z, ONE, &P);

    /* y^2 == x^3 - 3x + b */
    mont_mul(t1, p->y, p->y, &P);
    mont_mul(t2, p->x, p->x, &P);
    mont_mul(t2, t2, p->x, &P);
    mod_sub(t2, t2, p->x, &P);
    mod_sub(t2, t2, p->x, &P);
    mod_sub(t2, t2, p->x, &P);
    mod_add(t2, t2, b_mont, &P);
    return equal(t1, t2) ? 0 : -1;
}

static int load_scalar(uint32_t *k, const uint8_t *in)
{
    fe_from_bytes(k, in);
    return (!is_zero(k) && lt(k, N.m)) ? 0 : -1;
}

int p384_check_scalar(const uint8_t d[P384_LEN])
{
    fe k;
    int r = load_scalar(k, d);
    wipe(k, sizeof(k));
    return r;
}

int p384_pubkey(uint8_t pub[1 + 2 * P384_LEN], const uint8_t d[P384_LEN])
{
    point g, q;
    fe k, x, y;
    int r = -1;

    curve_setup();
    if (load_scalar(k, d) == 0) {
        base_point(&g);
        scalar_mult(&q, &g, k);
        if (point_to_affine(x, y, &q) == 0) {
            pub[0] = 0x04;
            fe_to_bytes(pub + 1, x);
            fe_to_bytes(pub + 1 + P384_LEN, y);
            r = 0;
        }
    }
    wipe(k, sizeof(k));
    wipe(&q, sizeof(q));
    return r;
}

int p384_ecdh(uint8_t out[P384_LEN], const uint8_t d[P384_LEN],
              const uint8_t peer[1 + 2 * P384_LEN])
{
    point p, q;
    fe k, x, y;
    int r = -1;

    curve_setup();
    if (load_scalar(k, d) == 0 && point_from_bytes(&p, peer) == 0) {
        scalar_mult(&q, &p, k);
        if (point_to_affine(x, y, &q) == 0) {
            fe_to_bytes(out, x);
            r = 0;
        }
    }
    wipe(k, sizeof(k));
    wipe(&q, sizeof(q));
    wipe(x, sizeof(x));
    return r;
}

/* One ECDSA attempt with nonce k. Returns -1 if r or s came out zero. */
static int sign_with_k(uint8_t sig[2 * P384_LEN], const uint32_t *d,
                       const uint32_t *e, const uint32_t *k)
{
    point g, kg;
    fe x, y, kinv, t, u;
    int ret = -1;

    base_point(&g);
    scalar_mult(&kg, &g, k);
    if (point_to_affine(x, y, &kg) != 0)
        goto out;
    reduce_once(x, &N); /* r = x mod n; x < p < 2n */
    if (is_zero(x))
        goto out;

    to_mont(kinv, k, &N);
    mod_inv(kinv, kinv, &N);
    to_mont(t, x, &N);
    to_mont(u, d, &N);
    mont_mul(t, t, u, &N);
    to_mont(u, e, &N);
    mod_add(t, t, u, &N);
    mont_mul(t, t, kinv, &N);
    from_mont(u, t, &N);
    if (is_zero(u))
        goto out;

    fe_to_bytes(sig, x);
    fe_to_bytes(sig + P384_LEN, u);
    ret = 0;
out:
    wipe(&kg, sizeof(kg));
    wipe(kinv, sizeof(kinv));
    wipe(t, sizeof(t));
    wipe(u, sizeof(u));
    return ret;
}

static void hmac_kv(uint8_t out[48], const uint8_t K[48], const uint8_t V[48],
                    const uint8_t *extra, size_t extra_len)
{
    hmac384_ctx h;

    hmac384_init(&h, K, 48);
    hmac384_update(&h, V, 48);
    hmac384_update(&h, extra, extra_len);
    hmac384_final(&h, out);
}

int p384_sign(uint8_t sig[2 * P384_LEN], const uint8_t d[P384_LEN],
              const uint8_t *digest, size_t digest_len)
{
    uint8_t eb[P384_LEN], K[48], V[48], seed[1 + 2 * P384_LEN];
    fe dk, e, k;
    int ret = -1;

    curve_setup();
    if (load_scalar(dk, d) != 0)
        goto out;

    /* e = bits2int(digest) mod n */
    memset(eb, 0, sizeof(eb));
    if (digest_len >= P384_LEN)
        memcpy(eb, digest, P384_LEN);
    else
        memcpy(eb + P384_LEN - digest_len, digest, digest_len);
    fe_from_bytes(e, eb);
    reduce_once(e, &N);
    fe_to_bytes(eb, e);

    /* RFC 6979 section 3.2 with HMAC-SHA-384. */
    memcpy(seed + 1, d, P384_LEN);
    memcpy(seed + 1 + P384_LEN, eb, P384_LEN);
    memset(V, 0x01, sizeof(V));
    memset(K, 0x00, sizeof(K));
    for (uint8_t i = 0; i < 2; i++) {
        seed[0] = i;
        hmac_kv(K, K, V, seed, sizeof(seed));
        hmac_kv(V, K, V, NULL, 0);
    }
    for (;;) {
        hmac_kv(V, K, V, NULL, 0);
        fe_from_bytes(k, V);
        if (!is_zero(k) && lt(k, N.m) && sign_with_k(sig, dk, e, k) == 0)
            break;
        seed[0] = 0;
        hmac_kv(K, K, V, seed, 1);
        hmac_kv(V, K, V, NULL, 0);
    }
    ret = 0;
out:
    wipe(dk, sizeof(dk));
    wipe(k, sizeof(k));
    wipe(K, sizeof(K));
    wipe(V, sizeof(V));
    wipe(seed, sizeof(seed));
    return ret;
}
