/*
 * Ersatz für /usr/lib/libcrypto.0.9.8.dylib (OpenSSL 0.9.8) im GLD Editor.
 *
 * libarchive.2.dylib braucht daraus nur SHA-256/384/512 und RIPEMD-160.
 * Die Kontexte werden von libarchive angelegt, deshalb haben die Strukturen
 * hier exakt die Größe der OpenSSL-0.9.8-Strukturen (x86_64):
 *   SHA256_CTX 112 Byte, SHA512_CTX 216 Byte, RIPEMD160_CTX 96 Byte.
 * SHA-2 kommt aus CommonCrypto (CC_SHA*_CTX ist jeweils kleiner und passt
 * hinein), RIPEMD-160 ist hier implementiert.
 */

#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>
#include <string.h>

typedef struct { unsigned char opaque[112]; } SHA256_CTX;
typedef struct { unsigned char opaque[216]; } SHA512_CTX;

typedef struct {
    uint32_t A, B, C, D, E;
    uint32_t Nl, Nh;
    uint32_t data[16];
    uint32_t num;
} RIPEMD160_CTX;

_Static_assert(sizeof(CC_SHA256_CTX) <= sizeof(SHA256_CTX), "SHA256_CTX zu klein");
_Static_assert(sizeof(CC_SHA512_CTX) <= sizeof(SHA512_CTX), "SHA512_CTX zu klein");
_Static_assert(sizeof(RIPEMD160_CTX) == 96, "RIPEMD160_CTX hat falsche Größe");

#define EXPORT __attribute__((visibility("default")))

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* SHA-256 */
EXPORT int SHA256_Init(SHA256_CTX *c) { return CC_SHA256_Init((CC_SHA256_CTX *)c); }
EXPORT int SHA256_Update(SHA256_CTX *c, const void *d, size_t n) { return CC_SHA256_Update((CC_SHA256_CTX *)c, d, (CC_LONG)n); }
EXPORT int SHA256_Final(unsigned char *md, SHA256_CTX *c) { return CC_SHA256_Final(md, (CC_SHA256_CTX *)c); }

/* SHA-384 (nutzt in OpenSSL denselben Kontext wie SHA-512) */
EXPORT int SHA384_Init(SHA512_CTX *c) { return CC_SHA384_Init((CC_SHA512_CTX *)c); }
EXPORT int SHA384_Update(SHA512_CTX *c, const void *d, size_t n) { return CC_SHA384_Update((CC_SHA512_CTX *)c, d, (CC_LONG)n); }
EXPORT int SHA384_Final(unsigned char *md, SHA512_CTX *c) { return CC_SHA384_Final(md, (CC_SHA512_CTX *)c); }

/* SHA-512 */
EXPORT int SHA512_Init(SHA512_CTX *c) { return CC_SHA512_Init((CC_SHA512_CTX *)c); }
EXPORT int SHA512_Update(SHA512_CTX *c, const void *d, size_t n) { return CC_SHA512_Update((CC_SHA512_CTX *)c, d, (CC_LONG)n); }
EXPORT int SHA512_Final(unsigned char *md, SHA512_CTX *c) { return CC_SHA512_Final(md, (CC_SHA512_CTX *)c); }

/* RIPEMD-160 (nach Dobbertin/Bosselaers/Preneel) */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define F1(x, y, z) ((x) ^ (y) ^ (z))
#define F2(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define F3(x, y, z) (((x) | ~(y)) ^ (z))
#define F4(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define F5(x, y, z) ((x) ^ ((y) | ~(z)))

static const uint8_t RL[80] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
    3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
    1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
    4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13};
static const uint8_t RR[80] = {
    5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
    6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
    15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
    8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
    12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11};
static const uint8_t SL[80] = {
    11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
    7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
    11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
    11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
    9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6};
static const uint8_t SR[80] = {
    8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
    9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
    9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
    15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
    8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11};
static const uint32_t KL[5] = {0x00000000, 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xA953FD4E};
static const uint32_t KR[5] = {0x50A28BE6, 0x5C4DD124, 0x6D703EF3, 0x7A6D76E9, 0x00000000};

static uint32_t rmd_f(int j, uint32_t x, uint32_t y, uint32_t z)
{
    switch (j / 16) {
    case 0: return F1(x, y, z);
    case 1: return F2(x, y, z);
    case 2: return F3(x, y, z);
    case 3: return F4(x, y, z);
    default: return F5(x, y, z);
    }
}

static void rmd_block(RIPEMD160_CTX *c, const uint8_t *p)
{
    uint32_t X[16];
    for (int i = 0; i < 16; i++)
        X[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 |
               (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;

    uint32_t al = c->A, bl = c->B, cl = c->C, dl = c->D, el = c->E;
    uint32_t ar = c->A, br = c->B, cr = c->C, dr = c->D, er = c->E;
    for (int j = 0; j < 80; j++) {
        uint32_t t = ROL(al + rmd_f(j, bl, cl, dl) + X[RL[j]] + KL[j / 16], SL[j]) + el;
        al = el; el = dl; dl = ROL(cl, 10); cl = bl; bl = t;
        t = ROL(ar + rmd_f(79 - j, br, cr, dr) + X[RR[j]] + KR[j / 16], SR[j]) + er;
        ar = er; er = dr; dr = ROL(cr, 10); cr = br; br = t;
    }
    uint32_t t = c->B + cl + dr;
    c->B = c->C + dl + er;
    c->C = c->D + el + ar;
    c->D = c->E + al + br;
    c->E = c->A + bl + cr;
    c->A = t;
}

EXPORT int RIPEMD160_Init(RIPEMD160_CTX *c)
{
    memset(c, 0, sizeof(*c));
    c->A = 0x67452301;
    c->B = 0xEFCDAB89;
    c->C = 0x98BADCFE;
    c->D = 0x10325476;
    c->E = 0xC3D2E1F0;
    return 1;
}

EXPORT int RIPEMD160_Update(RIPEMD160_CTX *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint8_t *buf = (uint8_t *)c->data;
    uint64_t bits = ((uint64_t)c->Nh << 32 | c->Nl) + (uint64_t)len * 8;
    c->Nl = (uint32_t)bits;
    c->Nh = (uint32_t)(bits >> 32);

    if (c->num) {
        size_t n = 64 - c->num < len ? 64 - c->num : len;
        memcpy(buf + c->num, p, n);
        c->num += (uint32_t)n;
        p += n;
        len -= n;
        if (c->num < 64)
            return 1;
        rmd_block(c, buf);
        c->num = 0;
    }
    for (; len >= 64; p += 64, len -= 64)
        rmd_block(c, p);
    memcpy(buf, p, len);
    c->num = (uint32_t)len;
    return 1;
}

EXPORT int RIPEMD160_Final(unsigned char *md, RIPEMD160_CTX *c)
{
    uint8_t *buf = (uint8_t *)c->data;
    uint32_t nl = c->Nl, nh = c->Nh;

    buf[c->num++] = 0x80;
    if (c->num > 56) {
        memset(buf + c->num, 0, 64 - c->num);
        rmd_block(c, buf);
        c->num = 0;
    }
    memset(buf + c->num, 0, 56 - c->num);
    for (int i = 0; i < 4; i++) {
        buf[56 + i] = (uint8_t)(nl >> (8 * i));
        buf[60 + i] = (uint8_t)(nh >> (8 * i));
    }
    rmd_block(c, buf);

    uint32_t h[5] = {c->A, c->B, c->C, c->D, c->E};
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++)
            md[4 * i + k] = (uint8_t)(h[i] >> (8 * k));
    memset(c, 0, sizeof(*c));
    return 1;
}
