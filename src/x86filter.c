// Split-stream x86 preprocessor. Port of dis.cpp's DisFilter/DisUnFilter with
// the analytics (source maps / reorder buffer) removed. Public domain.
#include "x86filter.h"
#include <stdlib.h>
#include <string.h>

#define NBUFFERS 20

// instruction format flags (mirrors dis.cpp)
#define fNM   0x0   // no modrm
#define fAM   0x1   // no modrm, address mode
#define fMR   0x2   // modrm
#define fMO   0x3   // modrm + extra opcode
#define fMODE 0x3
#define fNI   0x0   // no immediate
#define fBI   0x4   // byte immediate
#define fDI   0x8   // dword immediate
#define fWI   0xc   // word immediate
#define fTYPE 0xc
#define fAD   0x0   // address
#define fBR   0x4   // byte relative
#define fDR   0xc   // dword relative
#define fERR  0x9   // error

static const uint8_t Table0[256] = {
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,
  fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,
  fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,
  fNM|fNI,fNM|fNI,fMR|fNI,fMR|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fDI,fMR|fDI,fNM|fBI,fMR|fBI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,
  fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,
  fMR|fBI,fMR|fDI,fMR|fBI,fMR|fBI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fERR,   fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,
  fAM|fAD,fAM|fAD,fAM|fAD,fAM|fAD,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fBI,fNM|fDI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,
  fNM|fBI,fNM|fBI,fNM|fBI,fNM|fBI,fNM|fBI,fNM|fBI,fNM|fBI,fNM|fBI,fNM|fDI,fNM|fDI,fNM|fDI,fNM|fDI,fNM|fDI,fNM|fDI,fNM|fDI,fNM|fDI,
  fMR|fBI,fMR|fBI,fNM|fWI,fNM|fNI,fMR|fNI,fMR|fNI,fMR|fBI,fMR|fDI,fERR,   fNM|fNI,fNM|fWI,fNM|fNI,fNM|fNI,fNM|fBI,fERR,   fNM|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fBI,fNM|fBI,fNM|fNI,fNM|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fAM|fBR,fAM|fBR,fAM|fBR,fAM|fBR,fNM|fBI,fNM|fBI,fNM|fBI,fNM|fBI,fAM|fDR,fAM|fDR,fAM|fAD,fAM|fBR,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,
  fNM|fNI,fERR,   fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fMO|fNI,fMO|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fMO|fNI,fMO|fNI,
};

static const uint8_t Table0f[256] = {
  fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fERR,fERR,fERR,fERR,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,fERR,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fMR|fBI,fMR|fBI,fMR|fBI,fMR|fBI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fNI,fERR,fERR,fERR,fERR,fERR,fERR,fMR|fNI,fMR|fNI,
  fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,fAM|fDR,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fNM|fNI,fNM|fNI,fNM|fNI,fMR|fNI,fMR|fBI,fMR|fNI,fMR|fNI,fMR|fNI,fERR,fERR,fERR,fMR|fNI,fMR|fBI,fMR|fNI,fERR,fMR|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fERR,fERR,fERR,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,fNM|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fERR,
};

static const uint8_t Tablefx[32] = {
  fMR|fBI,fERR,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fDI,fERR,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,fMR|fNI,
  fMR|fNI,fMR|fNI,fERR,fERR,fERR,fERR,fERR,fERR,fMR|fNI,fMR|fNI,fMR|fNI,fERR,fMR|fNI,fERR,fMR|fNI,fERR,
};

static inline uint32_t bswap32(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
}

// ---- growable byte buffer ---------------------------------------------------

typedef struct { uint8_t *data; uint32_t size, max; } Buf;

static void buf_init(Buf *b) { b->size = 0; b->max = 16; b->data = malloc(16); }
static void buf_put(Buf *b, const void *src, uint32_t n) {
    if (b->size + n > b->max) {
        b->max = (b->max * 2 < b->size + n) ? b->size + n : b->max * 2;
        b->data = realloc(b->data, b->max);
    }
    memcpy(b->data + b->size, src, n);
    b->size += n;
}
static void buf_u8(Buf *b, uint8_t v)  { buf_put(b, &v, 1); }
static void buf_u32(Buf *b, uint32_t v){ buf_put(b, &v, 4); }

// ---- instruction length (first pass) ----------------------------------------

static uint32_t countInstr(const uint8_t *instr) {
    const uint8_t *start = instr;
    int o16 = 0, code, code2, flags, modrm, sib;
    code = *instr++;
    if (code == 0x66) { o16 = 1; code = *instr++; }
    if (code == 0x0f) { code2 = *instr++; flags = Table0f[code2]; }
    else flags = Table0[code];
    if ((flags & fMODE) == fMO)
        flags = Tablefx[((*instr & 0x38) >> 3) | ((code & 0x01) << 3) | ((code & 0x08) << 1)];
    if (flags == fERR) return 1;
    if (flags & fMR) {
        modrm = *instr++;
        sib = ((modrm & 0x07) == 4 && (modrm & 0xc0) != 0xc0) ? *instr++ : 0;
        if ((modrm & 0xc0) == 0x40) instr++;
        if ((modrm & 0xc0) == 0x80 || (modrm & 0xc7) == 0x05 ||
            ((modrm & 0xc0) == 0 && (sib & 0x07) == 5)) instr += 4;
    }
    if ((flags & fMODE) == fAM) {
        switch (flags & fTYPE) { case fAD: instr += 4; break; case fBR: instr += 1; break; case fDR: instr += 4; break; }
    } else {
        switch (flags & fTYPE) { case fBI: instr += 1; break; case fDI: instr += o16 ? 2 : 4; break; case fWI: instr += 2; break; }
    }
    return (uint32_t)(instr - start);
}

// ---- filter (encode) --------------------------------------------------------

uint8_t *X86Filter(const uint8_t *input, uint32_t size, uint32_t va, uint32_t *outSize) {
    // Work on a zero-padded copy: a truncated trailing instruction (escaped via
    // `rest`) can make the length decoder read a few operand bytes past the end,
    // and the jump-table scan reads dwords until one is out of range. 16 bytes
    // of zero padding keeps both in-bounds (zeros are below `va`, so they stop
    // the scan and decode as harmless opcodes that get escaped anyway).
    uint8_t *code = malloc((size_t)size + 16);
    memcpy(code, input, size);
    memset(code + size, 0, 16);

    Buf B[NBUFFERS];
    for (int i = 0; i < NBUFFERS; i++) buf_init(&B[i]);

    uint32_t funcTable[255];
    for (int i = 0; i < 255; i++) funcTable[i] = ~0U;
    int funcTablePos = 0, nextFunc = 1;
    uint32_t lastJump = 0, jumpTable = ~0U;

    // first pass: find the size of the final (possibly partial) instruction so
    // the trailing bytes that don't form a whole instruction are escaped.
    // Signed counter: the last instruction can overshoot `size`, which must end
    // the loop (an unsigned counter would wrap and walk off the buffer).
    int64_t curSize = size, lastSize = size;
    const uint8_t *cur = code;
    while (curSize > 0) { lastSize = curSize; uint32_t p = countInstr(cur); cur += p; curSize -= p; }
    uint32_t rest = (uint32_t)lastSize;

    // signed: an instruction (or jump table) may overshoot the boundary, which
    // must end the loop rather than wrap around (matches the original sInt).
    uint32_t memory = 0;
    int64_t left = (int64_t)size - rest;
    cur = code;
    while (left > 0) {
        const uint8_t *instr = cur;
        const uint8_t *start;
        uint8_t code1, code2 = 0, modrm = 0, sib = 0, flags;
        int o16 = 0;
        uint32_t val, processed;

        // jump table: a run of in-range dwords (bounded by the remaining input)
        if (memory + va == jumpTable) {
            const uint32_t *table = (const uint32_t *)instr;
            int64_t maxEntries = left / 4;
            int count = 0;
            while (count < maxEntries &&
                   table[count] >= va && table[count] < va + size) count++;
            buf_u32(&B[15], count);
            for (int i = 0; i < count; i++) buf_u32(&B[15], table[i]);
            jumpTable = ~0U;
            processed = (uint32_t)count * 4;
            memory += processed; cur += processed; left -= processed;
            continue;
        } else if (memory + va > jumpTable) jumpTable = ~0U;

        start = instr;
        code1 = *instr++;
        if (nextFunc && code1 != 0xcc) {
            funcTable[funcTablePos] = memory;
            if (++funcTablePos == 255) funcTablePos = 0;
            nextFunc = 0;
        }
        if (code1 == 0x66) { o16 = 1; code1 = *instr++; }
        if (code1 == 0x0f) { code2 = *instr++; flags = Table0f[code2]; }
        else flags = Table0[code1];
        if (code1 == 0xc2 || code1 == 0xc3 || code1 == 0xcc) nextFunc = 1;
        if ((flags & fMODE) == fMO)
            flags = Tablefx[((*instr & 0x38) >> 3) | ((code1 & 0x01) << 3) | ((code1 & 0x08) << 1)];

        if (flags == fERR) {
            buf_u8(&B[0], 0xce);            // escape
            buf_u8(&B[0], *start);
            processed = 1;
            memory += processed; cur += processed; left -= processed;
            continue;
        }

        if (o16) buf_u8(&B[0], 0x66);
        buf_u8(&B[0], code1);
        if (code1 == 0x0f) buf_u8(&B[0], code2);

        if ((flags & fMODE) == fMR) {
            modrm = *instr++;
            buf_u8(&B[0], modrm);
            if ((modrm & 0x07) == 4 && (modrm & 0xc0) != 0xc0) { sib = *instr++; buf_u8(&B[19], sib); }
            if ((modrm & 0xc0) == 0x40) buf_u8(&B[(modrm & 0x07) + 1], *instr++);
            if ((modrm & 0xc0) == 0x80 || (modrm & 0xc7) == 0x05 ||
                ((modrm & 0xc0) == 0 && (sib & 0x07) == 5)) {
                memcpy(&val, instr, 4); instr += 4;
                buf_u32(&B[(modrm & 0xc7) == 5 ? 14 : 13], bswap32(val));
                if (code1 == 0xff && modrm == 0x24 && val < jumpTable) jumpTable = val;
            }
        }

        if ((flags & fMODE) == fAM) {
            switch (flags & fTYPE) {
            case fAD: memcpy(&val, instr, 4); instr += 4; buf_u32(&B[15], val); break;
            case fBR: buf_u8(&B[9], *instr++); break;
            case fDR: {
                memcpy(&val, instr, 4); instr += 4;
                val += (uint32_t)(instr - start) + memory;
                if (code1 != 0xe8) {              // jmp/jcc rel32: zigzag delta
                    // zigzag-encode the signed delta; inverse of the decoder's
                    // `(t&1) ? ~(t>>1) : (t>>1)`. All unsigned: no signed overflow.
                    uint32_t d = val - lastJump;
                    uint32_t tmp = (d & 0x80000000u) ? (~d << 1) | 1 : d << 1;
                    buf_u32(&B[17], tmp);
                    lastJump = val;
                } else {                          // call rel32: index into FuncTable
                    int i;
                    for (i = 0; i < 255; i++) if (funcTable[i] == val) break;
                    buf_u8(&B[16], (uint8_t)(i + 1));
                    if (i == 255) {
                        buf_u32(&B[18], val);
                        funcTable[funcTablePos] = val;
                        if (++funcTablePos == 255) funcTablePos = 0;
                    }
                }
                break; }
            }
        } else {
            switch (flags & fTYPE) {
            case fBI: buf_u8(&B[10], *instr++); break;
            case fDI: if (!o16) { buf_put(&B[12], instr, 4); instr += 4; break; }
                      /* fall through */
            case fWI: buf_put(&B[11], instr, 2); instr += 2; break;
            }
        }

        processed = (uint32_t)(instr - start);
        memory += processed; cur += processed; left -= processed;
    }

    // trailing partial instruction: escape each remaining byte
    while (rest--) { buf_u8(&B[0], 0xce); buf_u8(&B[0], *cur++); }

    // assemble output: [va][20 stream sizes][20 streams]
    Buf out; buf_init(&out);
    buf_u32(&out, va);
    for (int i = 0; i < NBUFFERS; i++) buf_u32(&out, B[i].size);
    for (int i = 0; i < NBUFFERS; i++) buf_put(&out, B[i].data, B[i].size);
    for (int i = 0; i < NBUFFERS; i++) free(B[i].data);
    free(code);

    *outSize = out.size;
    return out.data;
}

// ---- unfilter (decode) ------------------------------------------------------

uint32_t X86Unfilter(const uint8_t *packed, uint8_t *dest, uint32_t va_unused) {
    (void)va_unused;
    const uint8_t *p = packed;
    uint32_t va; memcpy(&va, p, 4); p += 4;

    const uint8_t *buffer[NBUFFERS];
    const uint8_t *streams = p + NBUFFERS * 4;
    for (int i = 0; i < NBUFFERS; i++) {
        uint32_t sz; memcpy(&sz, p, 4); p += 4;
        buffer[i] = streams; streams += sz;
    }
    const uint8_t *finish = buffer[1];   // opcode stream ends where stream 1 begins

    uint8_t *oldDest = dest;
    uint32_t funcTable[256];
    int funcTablePos = 1, nextFunc = 1;
    uint32_t lastJump = 0, memory = 0, jumpTable = ~0U, val;

    while (buffer[0] < finish) {
        uint8_t *start = dest;

        if (memory + va == jumpTable) {
            int count; memcpy(&count, buffer[15], 4); buffer[15] += 4;
            memcpy(dest, buffer[15], (size_t)count * 4);
            buffer[15] += count * 4; dest += count * 4; memory += (uint32_t)count * 4;
            jumpTable = ~0U;
            continue;
        } else if (memory + va > jumpTable) jumpTable = ~0U;

        uint8_t code = *buffer[0]++;
        if (nextFunc && code != 0xcc) {
            funcTable[funcTablePos] = memory;
            if (++funcTablePos == 256) funcTablePos = 1;
            nextFunc = 0;
        }

        if (code == 0xce) {                 // escape: literal byte
            *dest++ = *buffer[0]++;
        } else {
            uint8_t modrm = 0, sib = 0, code2;
            int o16 = 0, flags;
            *dest++ = code;
            if (code == 0x66) { o16 = 1; code = *buffer[0]++; *dest++ = code; }
            if (code == 0xc2 || code == 0xc3 || code == 0xcc) nextFunc = 1;
            if (code == 0x0f) { code2 = *buffer[0]++; *dest++ = code2; flags = Table0f[code2]; }
            else flags = Table0[code];

            if (flags & fMR) {
                modrm = *buffer[0]++; *dest++ = modrm;
                if ((flags & fMODE) == fMO) {
                    flags = fMR | fNI;
                    if (!(modrm & 0x38) && !(code & 8)) flags += (code & 1) ? fDI : fBI;
                }
                if ((modrm & 0x07) == 4 && (modrm & 0xc0) != 0xc0) { sib = *buffer[19]++; *dest++ = sib; }
                if ((modrm & 0xc0) == 0x40) *dest++ = *buffer[(modrm & 0x07) + 1]++;
                if ((modrm & 0xc0) == 0x80 || (modrm & 0xc7) == 0x05 ||
                    ((modrm & 0xc0) == 0 && (sib & 0x07) == 5)) {
                    int i = (modrm & 0xc7) == 5 ? 14 : 13;
                    memcpy(&val, buffer[i], 4); buffer[i] += 4; val = bswap32(val);
                    memcpy(dest, &val, 4); dest += 4;
                    if (code == 0xff && modrm == 0x24 && val < jumpTable) jumpTable = val;
                }
            }

            if ((flags & fMODE) == fAM) {
                switch (flags & fTYPE) {
                case fAD: memcpy(&val, buffer[15], 4); buffer[15] += 4; memcpy(dest, &val, 4); dest += 4; break;
                case fBR: *dest++ = *buffer[9]++; break;
                case fDR:
                    if (code == 0xe8) {
                        int i = *buffer[16]++;
                        if (i) val = funcTable[i];
                        else {
                            memcpy(&val, buffer[18], 4); buffer[18] += 4;
                            funcTable[funcTablePos] = val;
                            if (++funcTablePos == 256) funcTablePos = 1;
                        }
                    } else {
                        memcpy(&val, buffer[17], 4); buffer[17] += 4;
                        val = (val & 1) ? ~(val >> 1) : (val >> 1);
                        lastJump += val; val = lastJump;
                    }
                    val -= (uint32_t)(dest + 4 - start) + memory;
                    memcpy(dest, &val, 4); dest += 4;
                    break;
                }
            } else {
                switch (flags & fTYPE) {
                case fBI: *dest++ = *buffer[10]++; break;
                case fDI: if (!o16) { memcpy(dest, buffer[12], 4); dest += 4; buffer[12] += 4; break; }
                          /* fall through */
                case fWI: memcpy(dest, buffer[11], 2); dest += 2; buffer[11] += 2; break;
                }
            }
        }
        memory += (uint32_t)(dest - start);
    }
    return (uint32_t)(dest - oldDest);
}
