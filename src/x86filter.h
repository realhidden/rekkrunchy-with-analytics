// Split-stream x86 preprocessor for the kkrunchy codec.
//
// Disassembles 32-bit x86 code and demultiplexes instruction fields into 20
// separate byte streams (opcodes, modrm/sib, displacements, immediates, jump
// targets, ...), converting relative call/jump targets to absolute and delta-
// coding them. Feeding the reordered streams to the context-mixing model
// compresses real x86 code noticeably better than the raw bytes.
//
// Ported from the original dis.cpp (DisFilter/DisUnFilter); the analytics-only
// machinery (source maps, reorder buffer) is dropped. Public domain.
#ifndef REKK_X86FILTER_H
#define REKK_X86FILTER_H

#include <stdint.h>

// Filter `size` bytes of x86 code loaded at virtual address `va`. Returns a
// malloc'd reordered stream (caller frees); *outSize gets its length. `va` only
// affects jump-table handling and must match the value passed to X86Unfilter.
uint8_t *X86Filter(const uint8_t *code, uint32_t size, uint32_t va, uint32_t *outSize);

// Reverse X86Filter into `dest` (caller allocates the original size). `va` must
// equal the filter-time value. Returns the number of bytes written.
uint32_t X86Unfilter(const uint8_t *packed, uint8_t *dest, uint32_t va);

#endif
