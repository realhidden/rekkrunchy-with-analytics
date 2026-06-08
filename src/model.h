// Portable C port of kkrunchy's context-mixing model (model_asm.asm).
// Written by Fabian "ryg" Giesen. Public domain. C port keeps it byte-exact.
#ifndef REKK_MODEL_H
#define REKK_MODEL_H

#include <stdint.h>

// Model constants — shared between model.c and PaqWorkspace layout.
#define PAQ_MEMSHIFT 23
#define PAQ_MEM      (1u << PAQ_MEMSHIFT)
#define PAQ_NMODEL   11
#define PAQ_NINPUT   48
#define PAQ_NWEIGHT  (256 + 256 + 16 + 128)  /* 640 */
#define PAQ_MAXLEN   2047
#define PAQ_APMSIZE  8192

typedef struct { uint8_t *cpr, *cps; uint32_t ctx, st; } PaqContextModel;

// Workspace-allocated model state. Sized for BSS allocation in the stub.
// model.c has a _Static_assert that this matches the internal layout exactly.
typedef struct {
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
    PaqContextModel cm[PAQ_NMODEL];
    const uint8_t *match[PAQ_MEM / 16];
    uint8_t  modelMem[PAQ_MEM];
    int16_t  tx[PAQ_NINPUT];
    int16_t  wx[PAQ_NINPUT * PAQ_NWEIGHT];
    int16_t  tx2[4];
    int16_t  wx2[8];
    uint32_t stateMap[256];
    uint32_t stretch[4096];  /* inverse of squash; runtime-generated in stub */
    int32_t  APM[PAQ_APMSIZE * 33];
} PaqWorkspace;

// Existing host API — uses an internal static PaqWorkspace.
void ModelInit(const uint8_t *bufStart);
void ModelSetPtr(const uint8_t *p);
uint32_t ModelUpdate(int bit);

// Workspace-pointer variants — caller supplies PaqWorkspace, no globals needed.
void     ModelInitBuf(PaqWorkspace *w, const uint8_t *bufStart);
void     ModelSetPtrBuf(PaqWorkspace *w, const uint8_t *p);
uint32_t ModelUpdateBuf(PaqWorkspace *w, int bit);

#endif
