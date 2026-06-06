// Portable C port of kkrunchy's context-mixing model (model_asm.asm).
// Byte-exact reimplementation of modelInitASM / modelASM. Public domain.
//
// The model predicts one bit at a time. Each call to ModelUpdate(bit):
//   1. trains the mixer weights using `bit` (the bit just coded), and
//   2. returns the probability for the *next* bit.
// On byte boundaries it also updates the per-context run/state models and the
// match model. This mirrors the original asm exactly, including the fixed-point
// arithmetic and 16-bit saturation of the MMX code.
#include "model.h"

#ifdef STUB_PAQ
/* Freestanding stub: provide our own memset/memcpy */
static void *paq_memset(void *s, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}
static void *paq_memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}
#define memset  paq_memset
#define memcpy  paq_memcpy
#define size_t unsigned long
#else
#include <string.h>
#include <stddef.h>
#endif

// Tables built by the original asm init, dumped once and baked in (tables.c).
extern const uint32_t RUNTABLE[256];
extern const uint8_t  STATECODE[512];
extern const uint8_t  STATENEXT[512];
extern const uint32_t STATEMAP_INIT[256];
extern const uint32_t STRETCH[4096];

// ---- internal helpers (all take explicit workspace pointer) ----

static inline int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint8_t prevByte(PaqWorkspace *w) {
    return (w->bufPtr > w->bufStart) ? w->bufPtr[-1] : 0;
}
static inline int16_t sat16(int32_t v) {
    return (int16_t)clamp32(v, -32768, 32767);
}

static const int16_t squashTab[33] = {
    1,2,4,6,10,17,27,45,74,120,194,311,488,747,1102,1546,2048,2550,2994,3349,
    3608,3785,3902,3976,4022,4051,4069,4079,4086,4090,4092,4094,4095
};
static int32_t squash(int32_t x) {
    if (x <  -2047) return 0;
    if (x >   2047) return 4095;
    int idx = (x >> 7) + 16;
    int32_t ww = x & 127;
    int32_t lo = squashTab[idx];
    int32_t hi = squashTab[idx + 1];
    return ((((hi - lo) & 0xffff) * ww + 64) >> 7) + lo;
}

static uint8_t *contextHash(PaqWorkspace *w, uint32_t i) {
    uint32_t tag = i >> 24;
    uint32_t idx = i & (((PAQ_MEM / 4) - 1) & ~1u);
    uint8_t *e = w->modelMem + (size_t)idx * 4;
    if ((uint8_t)tag == e[0]) return e + 1;
    e += 4;
    if ((uint8_t)tag == e[0]) return e + 1;
    if (e[1] > e[-3]) e -= 4;
    e[0] = (uint8_t)tag; e[1] = 0; e[2] = 0; e[3] = 0;
    return e + 1;
}

static void train(int32_t err, const int16_t *t, int16_t *ww, int n) {
    int16_t e16 = (int16_t)err;
    for (int k = 0; k < n; k++) {
        int32_t prod = (int32_t)sat16(t[k] * 2) * (int32_t)e16;
        int32_t m = prod >> 16;
        m = (m + 1) >> 1;
        ww[k] = sat16((int32_t)ww[k] + m);
    }
}

static const uint8_t masks[PAQ_NMODEL] =
    {0x1f,0x27,0x88,0x07,0x0a,0x09,0x05,0x03,0x04,0x02,0x01};
static const uint8_t bitm[PAQ_NMODEL] =
    {0xff,0xff,0xff,0xe0,0xff,0xff,0xff,0xff,0xff,0xff,0xff};

// ---- workspace-pointer API (core implementation) ----

void ModelInitBuf(PaqWorkspace *w, const uint8_t *bufStart) {
    memset(w, 0, sizeof(*w));
    w->bufStart = bufStart;
    w->bufPtr   = bufStart;
    w->c0   = 1;
    w->bpos = 8;
    w->pr[0] = w->pr[1] = w->pr[2] = w->pr[3] = 2048;
    memcpy(w->stateMap, STATEMAP_INIT, sizeof(w->stateMap));
    for (int m = 0; m < PAQ_NMODEL; m++) {
        w->cm[m].cpr = contextHash(w, 1);
        w->cm[m].cps = contextHash(w, 0);
    }
    for (int j = -16; j <= 16; j++)
        w->APM[j + 16] = squash(j << 7) << 4;
    for (uint32_t c = 1; c < PAQ_APMSIZE; c++)
        memcpy(&w->APM[c * 33], &w->APM[0], 33 * sizeof(int32_t));
}

void ModelSetPtrBuf(PaqWorkspace *w, const uint8_t *p) {
    w->bufPtr = p;
}

uint32_t ModelUpdateBuf(PaqWorkspace *w, int bit) {
    w->bit = bit;
    w->bitscaled = (bit << 16) + 128;

    // ---- update mixers ----
    for (int d = 0; d < 4; d++) {
        int32_t err = ((w->bit << 12) - w->pr[d]) * 7;
        if (d == 3) break;
        train(err, w->tx, w->wx + (size_t)w->ctx[d] * PAQ_NINPUT, PAQ_NINPUT);
    }
    {
        int32_t err = ((w->bit << 12) - w->pr[3]) * 7;
        train(err, w->tx2, w->wx2, 4);
    }

    // ---- advance c0 / bit position ----
    w->c0 = (w->c0 << 1) + w->bit;
    if (--w->bpos == 0) {
        w->bpos = 8;
        w->c0 = 1;

        // ---- update context models ----
        uint32_t acc = 0;
        for (int m = 0; m < PAQ_NMODEL; m++) {
            w->cm[m].ctx = acc & ~1u;

            uint8_t *cpr = w->cm[m].cpr;
            uint8_t pv = prevByte(w);
            if (pv != cpr[1]) { cpr[0] = 0; cpr[1] = pv; }
            cpr[0]++;
            w->cm[m].cpr = contextHash(w, (acc & ~1u) + 1);

            uint32_t ec = (uint32_t)(PAQ_NMODEL - m);
            acc = 0x811c9dc5u * (ec + 1);
            uint8_t mask = masks[ec - 1];
            uint8_t bm   = bitm[ec - 1];
            const uint8_t *p = w->bufPtr;
            while (mask) {
                p--;
                if (mask & 1) {
                    uint8_t v = (p >= w->bufStart) ? (*p & bm) : 0;
                    acc = (acc ^ v) * 0x01000193u;
                }
                mask >>= 1;
            }
        }

        // ---- match model ----
        uint32_t h = acc & ((PAQ_MEM / 16) - 1);
        const uint8_t *prevMatch = w->match[h];
        w->match[h] = w->bufPtr;
        const uint8_t *cur = w->bufPtr;

        if (w->matchl) {
            w->matchl++;
            w->matchp++;
        } else if (prevMatch) {
            w->matchp = prevMatch;
            const uint8_t *a = cur - 1;
            const uint8_t *b = prevMatch - 1;
            int32_t limit = (int32_t)(prevMatch - 1 - w->bufStart);
            int32_t len = 0;
            while (len < limit && *a == *b) { a--; b--; len++; }
            w->matchl = len;
        }
        if (w->matchl > PAQ_MAXLEN) w->matchl = PAQ_MAXLEN;
        int32_t ml = w->matchl;
        if (ml > 32) ml = 32;
        w->matchw = (uint32_t)(ml << 6);
    }

    // ---- predict ----
    w->c0s = w->c0 << 3;

    int16_t *tx = w->tx;
    int ti = 0;
    tx[ti++] = 127;

    int16_t matchInput = 0;
    if (w->matchl) {
        uint32_t mbyte = (uint8_t)*w->matchp;
        uint32_t mb = (mbyte | 0x100) >> w->bpos;
        if (mb == w->c0) {
            uint32_t nextbit = (mbyte >> (w->bpos - 1)) & 1;
            int32_t v = (int32_t)w->matchw;
            matchInput = (int16_t)(nextbit ? -v : v);
        } else {
            w->matchl = 0;
        }
    }
    tx[ti++] = matchInput;

    if (w->matchl > 400) {
        w->ctx[0] = 512 + 14;
        goto mix;
    }

    w->ctx[2] = 0x200;
    for (int m = 0; m < PAQ_NMODEL; m++) {
        PaqContextModel *cm = &w->cm[m];

        uint8_t *cpr = cm->cpr;
        uint32_t cnt = cpr[0];
        uint32_t edx = (uint32_t)cpr[1] + 0x100u;
        int negate = (edx >> (w->bpos - 1)) & 1;
        edx >>= w->bpos;
        int32_t runv = (int32_t)RUNTABLE[cnt];
        if (negate) runv = -runv;
        if (edx != w->c0) runv = 0;
        tx[ti++] = (int16_t)runv;

        uint8_t *cps = cm->cps;
        uint32_t st = cps[0];
        cps[0] = STATENEXT[st * 2 + w->bit];
        cm->cps = contextHash(w, cm->ctx ^ w->c0s);

        int32_t *smp = (int32_t *)&w->stateMap[cm->st];
        *smp += (w->bitscaled - *smp) >> 8;
        uint32_t newst = cm->cps[0];
        cm->st = newst;
        uint32_t sm = w->stateMap[newst] >> 4;
        tx[ti++] = (int16_t)((int32_t)STRETCH[sm] >> 2);

        uint32_t al = (sm >> 4) & 0xff;
        uint32_t nl = (~al) & 0xff;
        tx[ti++] = (int16_t)((int32_t)al - (int32_t)nl);
        uint32_t alc = al & STATECODE[newst * 2 + 0];
        uint32_t nlc = nl & STATECODE[newst * 2 + 1];
        tx[ti++] = (int16_t)((int32_t)alc - (int32_t)nlc);

        if (newst >= 1) w->ctx[2]++;
    }

    w->ctx[0] = prevByte(w);
    w->ctx[1] = (uint32_t)((w->c0 & 0xff) | 0x100);
    {
        int32_t e = w->matchl - 1;
        if (e < 0) e = 0;
        if (e > 255) e = 255;
        w->ctx[2] += RUNTABLE[e] >> 3;
    }

mix:;
    for (int b = 2; b >= 0; b--) {
        int16_t *ww = w->wx + (size_t)w->ctx[b] * PAQ_NINPUT;
        int32_t acc2 = 0;
        for (int k = 0; k < PAQ_NINPUT; k += 2) {
            int32_t pair = (int32_t)w->tx[k]   * (int32_t)ww[k]
                         + (int32_t)w->tx[k+1] * (int32_t)ww[k+1];
            acc2 += pair >> 8;
        }
        int32_t e = acc2 >> 3;
        w->tx2[b] = sat16(e);
        w->pr[b] = squash(e);
    }

    {
        int32_t acc2 = 0;
        for (int k = 0; k < 3; k++)
            acc2 += (int32_t)w->tx2[k] * (int32_t)w->wx2[k];
        acc2 >>= 16;
        w->pr[3] = squash(acc2);
    }

    // ---- APM stage ----
    int32_t p = w->pr[3];
    {
        uint32_t g = (uint32_t)(-(int32_t)w->bit) & 0x100fe;
        int32_t *api = &w->APM[16 + w->APMi];
        api[0] += (int32_t)(g - api[0]) >> 8;
        api[1] += (int32_t)(g - api[1]) >> 8;

        uint32_t pv = prevByte(w);
        uint32_t ix = ((pv << 4) + pv + w->c0) & (PAQ_APMSIZE - 1);
        ix = ix * 33;
        int32_t st2 = (int32_t)STRETCH[p];
        ix += (uint32_t)(st2 >> 7);
        w->APMi = ix;
        uint32_t frac = (uint32_t)st2 & 127;
        int32_t *ap = &w->APM[16 + ix];
        int32_t a0 = ap[0];
        int32_t a1 = ap[1];
        p = ((a0 << 7) + (a1 - a0) * (int32_t)frac) >> 11;
    }

    if ((((uint32_t)p >> 8) & 0xff) < 8) p++;
    return (uint32_t)p;
}

// ---- static instance + host API (thin wrappers) ----

static PaqWorkspace W_static;
_Static_assert(sizeof(PaqWorkspace) == sizeof(W_static), "PaqWorkspace layout mismatch");

void ModelInit(const uint8_t *bufStart) { ModelInitBuf(&W_static, bufStart); }
void ModelSetPtr(const uint8_t *p)      { ModelSetPtrBuf(&W_static, p); }
uint32_t ModelUpdate(int bit)           { return ModelUpdateBuf(&W_static, bit); }
