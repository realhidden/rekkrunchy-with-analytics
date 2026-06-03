// Portable C port of kkrunchy's context-mixing model (model_asm.asm).
// Written by Fabian "ryg" Giesen. Public domain. C port keeps it byte-exact.
#ifndef REKK_MODEL_H
#define REKK_MODEL_H

#include <stdint.h>

// Initialize the model. `buf` is the (de)compressed byte buffer; predictions
// read already-produced context bytes from it. `bufStart` marks the low bound.
void ModelInit(const uint8_t *bufStart);

// Set the current read cursor (bufPtr) inside the buffer. Predictions look at
// bytes below this cursor; the coder advances it one byte every 8 bits.
void ModelSetPtr(const uint8_t *p);

// Feed the just-coded bit, return the probability (12-bit, 0..4095+) for the
// next bit. Mirrors modelASM(bit).
uint32_t ModelUpdate(int bit);

#endif
