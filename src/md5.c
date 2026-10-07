/*
 * md5.c - MD5 message digest, written from the text of RFC 1321 (section 3).
 *
 * Words are kept in unsigned long, which may be wider than 32 bits, so every addition and
 * rotation is masked to 32 bits. Bytes are loaded and stored one at a time in little-endian
 * order; no pointer casts, so alignment and endianness do not matter.
 */
#include "config.h"

#include <string.h>

#include "md5.h"

#define MASK32 0xFFFFFFFFUL

/* T[i] = floor(2^32 * abs(sin(i + 1))), RFC 1321 section 3.4. */
static const unsigned long T[64] = {
    0xd76aa478UL, 0xe8c7b756UL, 0x242070dbUL, 0xc1bdceeeUL,
    0xf57c0fafUL, 0x4787c62aUL, 0xa8304613UL, 0xfd469501UL,
    0x698098d8UL, 0x8b44f7afUL, 0xffff5bb1UL, 0x895cd7beUL,
    0x6b901122UL, 0xfd987193UL, 0xa679438eUL, 0x49b40821UL,
    0xf61e2562UL, 0xc040b340UL, 0x265e5a51UL, 0xe9b6c7aaUL,
    0xd62f105dUL, 0x02441453UL, 0xd8a1e681UL, 0xe7d3fbc8UL,
    0x21e1cde6UL, 0xc33707d6UL, 0xf4d50d87UL, 0x455a14edUL,
    0xa9e3e905UL, 0xfcefa3f8UL, 0x676f02d9UL, 0x8d2a4c8aUL,
    0xfffa3942UL, 0x8771f681UL, 0x6d9d6122UL, 0xfde5380cUL,
    0xa4beea44UL, 0x4bdecfa9UL, 0xf6bb4b60UL, 0xbebfbc70UL,
    0x289b7ec6UL, 0xeaa127faUL, 0xd4ef3085UL, 0x04881d05UL,
    0xd9d4d039UL, 0xe6db99e5UL, 0x1fa27cf8UL, 0xc4ac5665UL,
    0xf4292244UL, 0x432aff97UL, 0xab9423a7UL, 0xfc93a039UL,
    0x655b59c3UL, 0x8f0ccc92UL, 0xffeff47dUL, 0x85845dd1UL,
    0x6fa87e4fUL, 0xfe2ce6e0UL, 0xa3014314UL, 0x4e0811a1UL,
    0xf7537e82UL, 0xbd3af235UL, 0x2ad7d2bbUL, 0xeb86d391UL
};

/* Left rotation of each step, four per round. */
static const unsigned int S[4][4] = {
    { 7, 12, 17, 22 },
    { 5, 9, 14, 20 },
    { 4, 11, 16, 23 },
    { 6, 10, 15, 21 }
};

static unsigned long rotl32(unsigned long x, unsigned int n)
{
    x &= MASK32;
    return ((x << n) | (x >> (32 - n))) & MASK32;
}

static unsigned long load_le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16)
           | ((unsigned long)p[3] << 24);
}

static void store_le32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Processes one 64-byte block (RFC 1321 section 3.4). */
static void transform(unsigned long state[4], const unsigned char block[64])
{
    unsigned long X[16];
    unsigned long a = state[0], b = state[1], c = state[2], d = state[3];
    unsigned int i;

    for (i = 0; i < 16; i++)
        X[i] = load_le32(block + 4 * i);
    for (i = 0; i < 64; i++) {
        unsigned int round = i / 16;
        unsigned long f, tmp;
        unsigned int k;

        switch (round) {
        case 0:   /* F(X,Y,Z) = XY v not(X) Z */
            f = (b & c) | (~b & MASK32 & d);
            k = i;
            break;
        case 1:   /* G(X,Y,Z) = XZ v Y not(Z) */
            f = (b & d) | (c & ~d & MASK32);
            k = (1 + 5 * i) % 16;
            break;
        case 2:   /* H(X,Y,Z) = X xor Y xor Z */
            f = b ^ c ^ d;
            k = (5 + 3 * i) % 16;
            break;
        default:  /* I(X,Y,Z) = Y xor (X v not(Z)) */
            f = c ^ ((b | ~d) & MASK32);
            k = (7 * i) % 16;
            break;
        }
        tmp = d;
        d = c;
        c = b;
        b = (b + rotl32((a + f + T[i] + X[k]) & MASK32, S[round][i % 4])) & MASK32;
        a = tmp;
    }
    state[0] = (state[0] + a) & MASK32;
    state[1] = (state[1] + b) & MASK32;
    state[2] = (state[2] + c) & MASK32;
    state[3] = (state[3] + d) & MASK32;
}

void md5_init(struct md5_ctx *ctx)
{
    ctx->state[0] = 0x67452301UL;
    ctx->state[1] = 0xefcdab89UL;
    ctx->state[2] = 0x98badcfeUL;
    ctx->state[3] = 0x10325476UL;
    ctx->count_lo = 0;
    ctx->count_hi = 0;
}

/* Adds len bytes to the 64-bit length counter, with carry between the two words. */
static void count_add(struct md5_ctx *ctx, size_t len)
{
    unsigned long lo = (unsigned long)(len & MASK32);
    /* Two shifts of 16: a single shift of 32 would be undefined when size_t has 32 bits. */
    unsigned long hi = (unsigned long)(((len >> 16) >> 16) & MASK32);

    ctx->count_lo = (ctx->count_lo + lo) & MASK32;
    if (ctx->count_lo < lo)
        hi++;
    ctx->count_hi = (ctx->count_hi + hi) & MASK32;
}

void md5_update(struct md5_ctx *ctx, const unsigned char *data, size_t len)
{
    size_t used = (size_t)(ctx->count_lo & 63);

    count_add(ctx, len);
    if (used > 0) {
        size_t take = 64 - used;

        if (take > len)
            take = len;
        memcpy(ctx->block + used, data, take);
        data += take;
        len -= take;
        if (used + take < 64)
            return;
        transform(ctx->state, ctx->block);
    }
    while (len >= 64) {
        transform(ctx->state, data);
        data += 64;
        len -= 64;
    }
    if (len > 0)
        memcpy(ctx->block, data, len);
}

void md5_final(struct md5_ctx *ctx, unsigned char out[16])
{
    unsigned char tail[72];
    unsigned long bits_lo = (ctx->count_lo << 3) & MASK32;
    unsigned long bits_hi = ((ctx->count_hi << 3) | (ctx->count_lo >> 29)) & MASK32;
    size_t used = (size_t)(ctx->count_lo & 63);
    size_t padlen = used < 56 ? 56 - used : 120 - used;
    int i;

    /* Padding (section 3.1) and length (section 3.2), appended through md5_update. */
    memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    store_le32(tail + padlen, bits_lo);
    store_le32(tail + padlen + 4, bits_hi);
    md5_update(ctx, tail, padlen + 8);
    for (i = 0; i < 4; i++)
        store_le32(out + 4 * i, ctx->state[i]);
}

void md5_hex(const unsigned char digest[16], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 16; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[32] = '\0';
}
