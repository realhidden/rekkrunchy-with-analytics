// Per-stream cost analysis for the x86 split-stream filter.
// Filters a real x86 .text file, then for each of the 20 streams reports its
// raw size and its compressed cost (run through the actual shipping model).
// Used to decide whether to split/merge/drop streams.
//
//   cc -O2 -Isrc -o ss research/streamstats.c src/x86filter.c src/codec.c \
//      src/model.c src/tables.c
//   ./ss <x86-code-file> [va]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "x86filter.h"
#include "codec.h"

#define NB 20
static const char *names[NB] = {
  "opcodes","disp8/reg0","disp8/reg1","disp8/reg2","disp8/reg3","disp8/reg4",
  "disp8/reg5","disp8/reg6","disp8/reg7","rel8","imm8","imm16","imm32",
  "disp32","disp32-nobase","abs+jumptab","call-index","jmp-rel32","call-new","sib"
};

static uint32_t csize(const uint8_t *p, uint32_t n) {
    if (!n) return 0;
    uint32_t o = 0; uint8_t *c = Compress(p, n, &o); free(c);
    return o;                       // includes 4-byte header; subtract for net
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file> [va]\n", argv[0]); return 2; }
    uint32_t va = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 0x401000;

    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *code = malloc(n); if (fread(code, 1, n, f) != (size_t)n) return 2; fclose(f);

    uint32_t fs = 0; uint8_t *blob = X86Filter(code, (uint32_t)n, va, &fs);
    const uint8_t *p = blob + 4;                       // skip va
    uint32_t sizes[NB]; for (int i = 0; i < NB; i++) { memcpy(&sizes[i], p, 4); p += 4; }
    const uint8_t *streams = p;

    // reference points
    uint32_t whole_raw = csize(code, (uint32_t)n) - 4;          // plain -c
    uint32_t whole_filt = csize(blob, fs) - 4;                  // -cx (filter then compress all)

    printf("file=%s  raw=%ld  filtered=%u\n", argv[1], n, fs);
    printf("plain   -c  compressed: %u\n", whole_raw);
    printf("filtered-cx compressed: %u\n\n", whole_filt);
    printf("%-16s %9s %9s %6s\n", "stream", "raw", "comp", "ratio");

    const uint8_t *q = streams;
    uint32_t sum_raw = 0, sum_comp = 0;
    for (int i = 0; i < NB; i++) {
        uint32_t r = sizes[i];
        uint32_t c = r ? csize(q, r) - 4 : 0;          // net compressed bytes
        sum_raw += r; sum_comp += c;
        printf("%2d %-13s %9u %9u  %5.1f%%\n", i, names[i], r, c, r ? 100.0*c/r : 0);
        q += r;
    }
    printf("%-16s %9u %9u\n", "SUM(streams)", sum_raw, sum_comp);
    free(code); free(blob);
    return 0;
}
