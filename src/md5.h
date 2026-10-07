/* md5.h - MD5 message digest, written from the text of RFC 1321. */
#ifndef HD_MD5_H
#define HD_MD5_H

#include <stddef.h>

struct md5_ctx {
    unsigned long state[4];   /* A, B, C, D: 32-bit words */
    unsigned long count_lo;   /* message length in bytes: 64-bit counter as two words */
    unsigned long count_hi;
    unsigned char block[64];  /* pending bytes of the current block */
};

void md5_init(struct md5_ctx *ctx);
void md5_update(struct md5_ctx *ctx, const unsigned char *data, size_t len);
void md5_final(struct md5_ctx *ctx, unsigned char out[16]);

/* Writes the 32 lowercase hex digits of a digest and a terminating NUL. */
void md5_hex(const unsigned char digest[16], char out[33]);

#endif
