// Range coder front-end for the kkrunchy context-mixing model.
#ifndef REKK_CODEC_H
#define REKK_CODEC_H

#include <stdint.h>

// Compress in[0..inSize) -> malloc'd buffer (caller frees); *outSize = length.
// Output = [4-byte LE original size][range-coded stream]. NULL on alloc failure.
uint8_t *Compress(const uint8_t *in, uint32_t inSize, uint32_t *outSize);

// Decompress a Compress() buffer -> malloc'd original (caller frees).
uint8_t *Decompress(const uint8_t *in, uint32_t inSize, uint32_t *outSize);

#endif
