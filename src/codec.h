// Range coder front-end for the kkrunchy context-mixing model.
#ifndef REKK_CODEC_H
#define REKK_CODEC_H

#include <stdint.h>
#include "model.h"

// Compress in[0..inSize) -> malloc'd buffer (caller frees); *outSize = length.
// Output = [4-byte LE original size][range-coded stream]. NULL on alloc failure.
uint8_t *Compress(const uint8_t *in, uint32_t inSize, uint32_t *outSize);

// Thread-safe variant: uses caller-supplied workspace instead of global state.
// bytes_done: if non-NULL, atomically incremented by ZEROPAGE after each chunk (progress tracking).
#ifndef STUB_PAQ
#include <stdatomic.h>
uint8_t *CompressBuf(const uint8_t *in, uint32_t inSize, uint32_t *outSize,
                     PaqWorkspace *w, atomic_size_t *bytes_done);
#endif

// Decompress a Compress() buffer -> malloc'd original (caller frees).
uint8_t *Decompress(const uint8_t *in, uint32_t inSize, uint32_t *outSize);

// Stub-safe variant: caller supplies output buffer and workspace (no malloc).
// Returns 0 on success, -1 on error.
int Decompress_buf(const uint8_t *in, uint32_t inSize,
                   uint8_t *out, uint32_t outSize, PaqWorkspace *ws);

#endif
