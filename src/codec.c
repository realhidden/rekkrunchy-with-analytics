// Portable range coder for the kkrunchy context-mixing model.
// Encoder ported from rangecoder.cpp, decoder from depacker.asm; the x86 inline
// asm (sMulShift12, emms) is replaced with plain 64-bit arithmetic. Public domain.
#include "codec.h"

#ifdef STUB_PAQ
/* Freestanding stub: provide our own memset */
static void *paq_memset(void *s, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}
#define memset paq_memset
#else
#include <stdlib.h>
#include <string.h>
#endif

#define ZEROPAGE 8192

static inline uint32_t mulshift12(uint32_t a, uint32_t b) {
    return (uint32_t)(((uint64_t)a * b) >> 12);
}

// ---- decoder types and helper (used by both host Decompress and Decompress_buf) --

typedef struct { const uint8_t *src; uint32_t range, csub; } Dec;

static int dec_bit(Dec *d, uint32_t prob) {
    uint32_t bound = mulshift12(d->range, prob);
    uint32_t code = ((uint32_t)d->src[0] << 24) | ((uint32_t)d->src[1] << 16)
                  | ((uint32_t)d->src[2] << 8)  | (uint32_t)d->src[3];
    code -= d->csub;
    int bit;
    if (bound > code) {
        d->range = bound; bit = 1;
    } else {
        d->csub += bound; d->range -= bound; bit = 0;
    }
    while ((d->range & 0xff000000u) == 0) {
        d->src++;
        d->range <<= 8;
        d->csub <<= 8;
    }
    return bit;
}

// ---- encoder ----------------------------------------------------------------

typedef struct {
    uint8_t *out;
    uint64_t low;
    uint32_t range, cache, ffnum;
    int firstByte;
} Enc;

static void enc_shiftlow(Enc *e) {
    uint32_t carry = (uint32_t)(e->low >> 32);
    if (e->low < 0xff000000ULL || carry == 1) {
        if (!e->firstByte) *e->out++ = (uint8_t)(e->cache + carry);
        else e->firstByte = 0;
        for (; e->ffnum; e->ffnum--) *e->out++ = (uint8_t)(0xff + carry);
        e->cache = (uint32_t)((e->low >> 24) & 0xff);
    } else {
        e->ffnum++;
    }
    e->low = (e->low << 8) & 0xffffffffULL;
}

static void enc_bit(Enc *e, uint32_t prob, int bit) {
    uint32_t bound = mulshift12(e->range, prob);
    if (bit) e->range = bound;
    else { e->low += bound; e->range -= bound; }
    while (e->range < 0x01000000) { e->range <<= 8; enc_shiftlow(e); }
}

// Compress `in`/`inSize` to a freshly malloc'd buffer; *outSize gets the length.
// Output layout: [4 bytes little-endian original size][range-coded stream].
#ifndef STUB_PAQ  /* encoder + host decoder not needed in stub */
uint8_t *Compress(const uint8_t *in, uint32_t inSize, uint32_t *outSize) {
    uint8_t *out = malloc((size_t)inSize + inSize / 2 + 64);
    if (!out) return NULL;
    out[0] = inSize & 0xff; out[1] = (inSize >> 8) & 0xff;
    out[2] = (inSize >> 16) & 0xff; out[3] = (inSize >> 24) & 0xff;

    Enc e = { out + 4, 0, ~0u, 0, 0, 1 };
    uint32_t prob = 2048, zeroProb = 1;
    ModelInit(in);

    const uint8_t *p = in;
    for (uint32_t pos = 0; pos < inSize; pos++) {
        if ((pos & (ZEROPAGE - 1)) == 0) {
            int isZero = (inSize - pos) > ZEROPAGE;
            if (isZero)
                for (int i = 0; i < ZEROPAGE; i++)
                    if (p[i]) { isZero = 0; break; }
            enc_bit(&e, zeroProb, isZero);
            zeroProb = (zeroProb + (isZero ? 4096 : 1)) >> 1;
            if (isZero) { p += ZEROPAGE; pos += ZEROPAGE - 1; continue; }
        }
        for (int i = 0; i < 8; i++) {
            int bit = (*p >> (7 - i)) & 1;
            enc_bit(&e, prob, bit);
            if (i == 7) p++;
            ModelSetPtr(p);
            prob = ModelUpdate(bit);
        }
    }
    for (int i = 0; i < 5; i++) enc_shiftlow(&e);

    *outSize = (uint32_t)(e.out - out);
    return out;
}

// Thread-safe variant: caller supplies PaqWorkspace; no global state touched.
// bytes_done: if non-NULL, atomically incremented by ZEROPAGE after each 8 KB chunk.
#include <stdatomic.h>
uint8_t *CompressBuf(const uint8_t *in, uint32_t inSize, uint32_t *outSize,
                     PaqWorkspace *w, atomic_size_t *bytes_done) {
    uint8_t *out = malloc((size_t)inSize + inSize / 2 + 64);
    if (!out) return NULL;
    out[0] = inSize & 0xff; out[1] = (inSize >> 8) & 0xff;
    out[2] = (inSize >> 16) & 0xff; out[3] = (inSize >> 24) & 0xff;

    Enc e = { out + 4, 0, ~0u, 0, 0, 1 };
    uint32_t prob = 2048, zeroProb = 1;
    ModelInitBuf(w, in);

    const uint8_t *p = in;
    for (uint32_t pos = 0; pos < inSize; pos++) {
        if ((pos & (ZEROPAGE - 1)) == 0) {
            if (bytes_done && pos > 0)
                atomic_fetch_add(bytes_done, ZEROPAGE);
            int isZero = (inSize - pos) > ZEROPAGE;
            if (isZero)
                for (int i = 0; i < ZEROPAGE; i++)
                    if (p[i]) { isZero = 0; break; }
            enc_bit(&e, zeroProb, isZero);
            zeroProb = (zeroProb + (isZero ? 4096 : 1)) >> 1;
            if (isZero) {
                p += ZEROPAGE; pos += ZEROPAGE - 1;
                if (bytes_done) atomic_fetch_add(bytes_done, ZEROPAGE);
                continue;
            }
        }
        for (int i = 0; i < 8; i++) {
            int bit = (*p >> (7 - i)) & 1;
            enc_bit(&e, prob, bit);
            if (i == 7) p++;
            ModelSetPtrBuf(w, p);
            prob = ModelUpdateBuf(w, bit);
        }
    }
    for (int i = 0; i < 5; i++) enc_shiftlow(&e);

    *outSize = (uint32_t)(e.out - out);
    return out;
}

// ---- host decoder (malloc'd output) -----------------------------------------

uint8_t *Decompress(const uint8_t *in, uint32_t inSize, uint32_t *outSize) {
    (void)inSize;
    uint32_t n = (uint32_t)in[0] | ((uint32_t)in[1] << 8)
               | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    uint8_t *out = malloc(n ? n : 1);
    if (!out) return NULL;

    Dec d = { in + 4, ~0u, 0 };
    d.csub = 0;
    uint32_t prob = 2048, zeroProb = 1;
    ModelInit(out);

    uint8_t *p = out;
    uint32_t pos = 0;
    while (pos < n) {
        if ((pos & (ZEROPAGE - 1)) == 0) {
            int isZero = dec_bit(&d, zeroProb);
            zeroProb = (zeroProb + (isZero ? 4096 : 1)) >> 1;
            if (isZero) {
                memset(p, 0, ZEROPAGE);
                p += ZEROPAGE; pos += ZEROPAGE;
                ModelSetPtr(p);
                continue;
            }
        }
        int byte = 0;
        for (int i = 0; i < 8; i++) {
            int bit = dec_bit(&d, prob);
            byte = (byte << 1) | bit;
            if (i == 7) *p++ = (uint8_t)byte;
            ModelSetPtr(p);
            prob = ModelUpdate(bit);
        }
        pos++;
    }
    *outSize = n;
    return out;
}
#endif /* !STUB_PAQ */

// ---- stub-safe decompress: caller supplies output buffer and PaqWorkspace ----

int Decompress_buf(const uint8_t *in, uint32_t inSize,
                   uint8_t *out, uint32_t outSize, PaqWorkspace *ws) {
    (void)inSize;
    uint32_t n = (uint32_t)in[0] | ((uint32_t)in[1] << 8)
               | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (n != outSize) return -1;

    Dec d = { in + 4, ~0u, 0 };
    uint32_t prob = 2048, zeroProb = 1;
    ModelInitBuf(ws, out);

    uint8_t *p = out;
    uint32_t pos = 0;
    while (pos < n) {
        if ((pos & (ZEROPAGE - 1)) == 0) {
            int isZero = dec_bit(&d, zeroProb);
            zeroProb = (zeroProb + (isZero ? 4096 : 1)) >> 1;
            if (isZero) {
                memset(p, 0, ZEROPAGE);
                p += ZEROPAGE; pos += ZEROPAGE;
                ModelSetPtrBuf(ws, p);
                continue;
            }
        }
        int byte = 0;
        for (int i = 0; i < 8; i++) {
            int bit = dec_bit(&d, prob);
            byte = (byte << 1) | bit;
            if (i == 7) *p++ = (uint8_t)byte;
            ModelSetPtrBuf(ws, p);
            prob = ModelUpdateBuf(ws, bit);
        }
        pos++;
    }
    return 0;
}
