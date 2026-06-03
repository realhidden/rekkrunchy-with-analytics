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
#include <string.h>

#define MEMSHIFT 23
#define MEM      (1u << MEMSHIFT)
#define NMODEL   11
#define NINPUT   48
#define NWEIGHT  (256 + 256 + 16 + 128)
#define MAXLEN   2047
#define APMSIZE  8192

// Tables built by the original asm init, dumped once and baked in (tables.c).
extern const uint32_t RUNTABLE[256];
extern const uint8_t  STATECODE[512];     // 2 "and-mask" bytes per state
extern const uint8_t  STATENEXT[512];     // 2 next-states per state (bit 0/1)
extern const uint32_t STATEMAP_INIT[256];
extern const uint32_t STRETCH[4096];

typedef struct { uint8_t *cpr, *cps; uint32_t ctx, st; } ContextModel;

static struct {
    const uint8_t *bufStart, *bufPtr;
    uint32_t c0, c0s, bpos;
    int32_t  lentemp;
    const uint8_t *matchp;
    int32_t  matchl;
    uint32_t matchw;
    int32_t  pr[4];
    uint32_t ctx[3];
    int32_t  bit, bitscaled;
    uint32_t APMi;
    ContextModel cm[NMODEL];
    const uint8_t *match[MEM / 16];
    uint8_t  modelMem[MEM];
    int16_t  tx[NINPUT];
    int16_t  wx[NINPUT * NWEIGHT];
    int16_t  tx2[4];
    int16_t  wx2[8];
    uint32_t stateMap[256];
    int32_t  APM[APMSIZE * 33];
} W;

static inline int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Byte just before the cursor, or 0 at the buffer start. The depacker guards
// this underflow explicitly (.underflow: xor dl,dl); the encoder must match so
// the streams agree on the very first byte.
static inline uint8_t prevByte(void) {
    return (W.bufPtr > W.bufStart) ? W.bufPtr[-1] : 0;
}
static inline int16_t sat16(int32_t v) {
    return (int16_t)clamp32(v, -32768, 32767);
}

// squash: logistic mapping stretch-domain -> 0..4095 probability.
static const int16_t squashTab[33] = {
    1,2,4,6,10,17,27,45,74,120,194,311,488,747,1102,1546,2048,2550,2994,3349,
    3608,3785,3902,3976,4022,4051,4069,4079,4086,4090,4092,4094,4095
};
static int32_t squash(int32_t x) {
    if (x <  -2047) return 0;          // asm: cmp -2047, jge .notsmall (>= keeps)
    if (x >   2047) return 4095;
    int idx = (x >> 7) + 16;           // sar 7; index into squashTab+16
    int32_t w = x & 127;
    int32_t lo = squashTab[idx];
    int32_t hi = squashTab[idx + 1];
    return ((((hi - lo) & 0xffff) * w + 64) >> 7) + lo;   // movzx dx => mask 16 bits
}

// contextHash: 2-way associative lookup into modelMem keyed by the top byte of
// `i`; returns pointer to the 2-byte run record {count,lastbyte}. Byte-exact.
static uint8_t *contextHash(uint32_t i) {
    uint32_t tag = i >> 24;
    uint32_t idx = i & (((MEM / 4) - 1) & ~1u);
    uint8_t *e = W.modelMem + (size_t)idx * 4;     // slot0 (4 bytes)
    if ((uint8_t)tag == e[0]) return e + 1;
    e += 4;                                         // slot1
    if ((uint8_t)tag == e[0]) return e + 1;
    // evict: keep the slot with the higher count; asm compares slot1.count(e[1])
    // with slot0.count(e[-3]); if slot1 > slot0 it overwrites slot0 (e-=4).
    if (e[1] > e[-3]) e -= 4;
    e[0] = (uint8_t)tag; e[1] = 0; e[2] = 0; e[3] = 0;  // movzx edx,dl; mov [eax],edx
    return e + 1;
}

// train: w[k] += round((2*t[k]) * err16 / 65536) / 2, saturating. err is taken
// as a 16-bit value (low word, sign-extended via pmulhw semantics).
static void train(int32_t err, const int16_t *t, int16_t *w, int n) {
    int16_t e16 = (int16_t)err;                  // movd + punpcklwd: low 16 bits
    for (int k = 0; k < n; k++) {
        int32_t prod = (int32_t)sat16(t[k] * 2) * (int32_t)e16;  // paddsw; pmulhw
        int32_t m = prod >> 16;                  // pmulhw = high word of 32-bit
        m = (m + 1) >> 1;                         // paddsw +1 ; psraw 1
        w[k] = sat16((int32_t)w[k] + m);          // paddsw
    }
}

// masks/bitm select which prior bytes feed each context model's FNV hash.
static const uint8_t masks[NMODEL] =
    {0x1f,0x27,0x88,0x07,0x0a,0x09,0x05,0x03,0x04,0x02,0x01};
static const uint8_t bitm[NMODEL] =
    {0xff,0xff,0xff,0xe0,0xff,0xff,0xff,0xff,0xff,0xff,0xff};

void ModelInit(const uint8_t *bufStart) {
    memset(&W, 0, sizeof(W));
    W.bufStart = bufStart;
    W.bufPtr   = bufStart;
    W.c0   = 1;
    W.bpos = 8;
    W.pr[0] = W.pr[1] = W.pr[2] = W.pr[3] = 2048;
    memcpy(W.stateMap, STATEMAP_INIT, sizeof(W.stateMap));
    for (int m = 0; m < NMODEL; m++) {
        W.cm[m].cpr = contextHash(1);
        W.cm[m].cps = contextHash(0);
    }
    for (int j = -16; j <= 16; j++)
        W.APM[j + 16] = squash(j << 7) << 4;
    for (uint32_t c = 1; c < APMSIZE; c++)
        memcpy(&W.APM[c * 33], &W.APM[0], 33 * sizeof(int32_t));
}

void ModelSetPtr(const uint8_t *p) { W.bufPtr = p; }

uint32_t ModelUpdate(int bit) {
    W.bit = bit;
    W.bitscaled = (bit << 16) + 128;

    // ---- update mixers (pr[0..2] are weighted; pr[3] yields err only) ----
    for (int d = 0; d < 4; d++) {
        int32_t err = ((W.bit << 12) - W.pr[d]) * 7;
        if (d == 3) break;
        train(err, W.tx, W.wx + (size_t)W.ctx[d] * NINPUT, NINPUT);
    }
    {
        int32_t err = ((W.bit << 12) - W.pr[3]) * 7;
        train(err, W.tx2, W.wx2, 4);
    }

    // ---- advance c0 / bit position ----
    W.c0 = (W.c0 << 1) + W.bit;
    if (--W.bpos == 0) {
        W.bpos = 8;
        W.c0 = 1;

        // ---- update context models ----
        // `acc` chains across models: each model's ctx is the previous model's
        // FNV hash (& ~1), and its cpr record is contextHash((ctx)+1).
        uint32_t acc = 0;
        for (int m = 0; m < NMODEL; m++) {
            W.cm[m].ctx = acc & ~1u;

            // run context: flush on byte change (keep new byte), bump count
            uint8_t *cpr = W.cm[m].cpr;
            uint8_t pv = prevByte();
            if (pv != cpr[1]) { cpr[0] = 0; cpr[1] = pv; }
            cpr[0]++;
            W.cm[m].cpr = contextHash((acc & ~1u) + 1);

            // FNV hash of selected prior bytes; seed and mask depend on the
            // descending loop counter ec = NMODEL..1 (so masks are reversed).
            uint32_t ec = (uint32_t)(NMODEL - m);
            acc = 0x811c9dc5u * (ec + 1);
            uint8_t mask = masks[ec - 1];
            uint8_t bm   = bitm[ec - 1];
            const uint8_t *p = W.bufPtr;
            while (mask) {
                p--;
                if (mask & 1) {
                    uint8_t v = (p >= W.bufStart) ? (*p & bm) : 0;
                    acc = (acc ^ v) * 0x01000193u;
                }
                mask >>= 1;
            }
        }

        // ---- match model ----
        uint32_t h = acc & ((MEM / 16) - 1);
        const uint8_t *prevMatch = W.match[h];
        W.match[h] = W.bufPtr;                    // xchg edi,[match[h]]
        const uint8_t *cur = W.bufPtr;

        if (W.matchl) {
            W.matchl++;
            W.matchp++;
        } else if (prevMatch) {
            W.matchp = prevMatch;
            // backward compare (std; cmpsb) then forward extend (repe cmpsb)
            // asm computes match length of the run ending just before cur vs
            // ending just before prevMatch, bounded by start of buffer.
            // lentemp = (prevMatch-1) - bufStart bounds the backward scan; the
            // match length is the count of equal byte pairs cur[-1-k]==pm[-1-k].
            const uint8_t *a = cur - 1;
            const uint8_t *b = prevMatch - 1;
            int32_t limit = (int32_t)(prevMatch - 1 - W.bufStart);
            int32_t len = 0;
            while (len < limit && *a == *b) { a--; b--; len++; }
            W.matchl = len;
        }
        if (W.matchl > MAXLEN) W.matchl = MAXLEN;
        int32_t ml = W.matchl;
        if (ml > 32) ml = 32;
        W.matchw = (uint32_t)(ml << 6);
    }

    // ---- predict ----
    W.c0s = W.c0 << 3;

    int16_t *tx = W.tx;
    int ti = 0;
    tx[ti++] = 127;                               // constant model

    // match model input: if the match byte's high bits (so far) equal c0, emit
    // +/- matchw; the sign is the predicted next bit (bit bpos-1), negated.
    int16_t matchInput = 0;
    if (W.matchl) {
        uint32_t mbyte = (uint8_t)*W.matchp;
        uint32_t mb = (mbyte | 0x100) >> W.bpos;
        if (mb == W.c0) {
            uint32_t nextbit = (mbyte >> (W.bpos - 1)) & 1;   // CF of shr
            int32_t v = (int32_t)W.matchw;
            matchInput = (int16_t)(nextbit ? -v : v);
        } else {
            W.matchl = 0;
        }
    }
    tx[ti++] = matchInput;

    if (W.matchl > 400) {
        W.ctx[0] = 512 + 14;
        goto mix;
    }

    // context models
    W.ctx[2] = 0x200;                             // mov ah,2 (=512) into ctx[2]
    for (int m = 0; m < NMODEL; m++) {
        ContextModel *cm = &W.cm[m];

        // run model contribution: runTable[count], conditionally negated by the
        // bit shifted out of (lastByte+0x100)>>bpos, zeroed unless that == c0.
        uint8_t *cpr = cm->cpr;
        uint32_t cnt = cpr[0];
        uint32_t edx = (uint32_t)cpr[1] + 0x100u;  // inc dh
        int negate = (edx >> (W.bpos - 1)) & 1;    // CF = last bit shifted out
        edx >>= W.bpos;
        int32_t runv = (int32_t)RUNTABLE[cnt];
        if (negate) runv = -runv;                  // sbb ecx,ecx; xor; sub
        if (edx != W.c0) runv = 0;
        tx[ti++] = (int16_t)runv;

        // nonstationary context: advance state machine on the old cps, then
        // recompute the cps pointer from the (hashed) context.
        uint8_t *cps = cm->cps;
        uint32_t st = cps[0];
        cps[0] = STATENEXT[st * 2 + W.bit];
        cm->cps = contextHash(cm->ctx ^ W.c0s);

        // state map: update map cell for the *old* st, then read the cell for
        // the *new* state (taken from the recomputed cps pointer).
        int32_t *smp = (int32_t *)&W.stateMap[cm->st];
        *smp += (W.bitscaled - *smp) >> 8;
        uint32_t newst = cm->cps[0];
        cm->st = newst;
        uint32_t sm = W.stateMap[newst] >> 4;
        tx[ti++] = (int16_t)((int32_t)STRETCH[sm] >> 2);    // stretched prob

        uint32_t al = (sm >> 4) & 0xff;            // upper bits cancel in asm sub
        uint32_t nl = (~al) & 0xff;
        tx[ti++] = (int16_t)((int32_t)al - (int32_t)nl);
        uint32_t alc = al & STATECODE[newst * 2 + 0];
        uint32_t nlc = nl & STATECODE[newst * 2 + 1];
        tx[ti++] = (int16_t)((int32_t)alc - (int32_t)nlc);

        if (newst >= 1) W.ctx[2]++;                // cmp ebx,1; sbb ctx[2],-1
    }

    // weighting contexts
    W.ctx[0] = prevByte();
    W.ctx[1] = (uint32_t)((W.c0 & 0xff) | 0x100);  // al=c0; inc ah
    {
        int32_t e = W.matchl - 1;
        if (e < 0) e = 0;
        if (e > 255) e = 255;
        W.ctx[2] += RUNTABLE[e] >> 3;
    }

mix:;
    // perform per-context mixing for ctx[2],ctx[1],ctx[0]
    for (int b = 2; b >= 0; b--) {
        int16_t *w = W.wx + (size_t)W.ctx[b] * NINPUT;
        int32_t acc = 0;
        for (int k = 0; k < NINPUT; k += 2) {       // pmaddwd: sum adjacent pair,
            int32_t pair = (int32_t)W.tx[k]   * (int32_t)w[k]
                         + (int32_t)W.tx[k+1] * (int32_t)w[k+1];
            acc += pair >> 8;                       // psrad 8 on the pair-sum
        }
        int32_t e = acc >> 3;                       // sar eax,3
        W.tx2[b] = sat16(e);
        W.pr[b] = squash(e);
    }

    // final mix (3 weighted inputs)
    {
        int32_t acc = 0;
        for (int k = 0; k < 3; k++)
            acc += (int32_t)W.tx2[k] * (int32_t)W.wx2[k];
        acc >>= 16;
        W.pr[3] = squash(acc);
    }

    // ---- APM stage ----
    int32_t p = W.pr[3];
    {
        uint32_t g = (uint32_t)(-(int32_t)W.bit) & 0x100fe;
        int32_t *api = &W.APM[16 + W.APMi];
        api[0] += (int32_t)(g - api[0]) >> 8;
        api[1] += (int32_t)(g - api[1]) >> 8;

        uint32_t pv = prevByte();
        uint32_t ix = ((pv << 4) + pv + W.c0) & (APMSIZE - 1);
        ix = ix * 33;
        int32_t st = (int32_t)STRETCH[p];
        ix += (uint32_t)(st >> 7);
        W.APMi = ix;
        uint32_t frac = (uint32_t)st & 127;
        int32_t *ap = &W.APM[16 + ix];
        int32_t a0 = ap[0];
        int32_t a1 = ap[1];
        p = ((a0 << 7) + (a1 - a0) * (int32_t)frac) >> 11;
    }

    // return value: asm does "cmp ah,8; adc eax,0" -> +1 iff (p>>8)&0xff < 8.
    if ((((uint32_t)p >> 8) & 0xff) < 8) p++;
    return (uint32_t)p;
}
