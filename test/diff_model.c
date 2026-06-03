// Differential test: drive the same bitstream through the asm oracle (model_c)
// and the portable C port (ModelUpdate); report the first probability mismatch.
// Build/run in the linux/386 docker. Dev-only.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../src/model.h"

extern void     model_init_c(void);
extern uint32_t model_c(int bit);
extern void     set_bufptrs(void *buf, void *start);

#define PAD 16

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 65536) n = 65536;                 // cap: this test is O(n) bits
    uint8_t *base = calloc(PAD + n, 1);
    fread(base + PAD, 1, n, f); fclose(f);
    uint8_t *data = base + PAD;                // bytes below `data` are zero pad

    ModelInit(data);
    model_init_c();

    const uint8_t *bp_c = data, *bp_a = data;
    uint32_t pc = 2048, pa = 2048;             // initial prob (unused first cmp)
    long mismatches = 0;
    for (long pos = 0; pos < n; pos++) {
        for (int i = 0; i < 8; i++) {
            int bit = (data[pos] >> (7 - i)) & 1;
            if (i == 7) { bp_c++; bp_a++; }    // rangecoder advances at last bit
            ModelSetPtr(bp_c);
            set_bufptrs((void *)bp_a, (void *)data);
            pc = ModelUpdate(bit);
            pa = model_c(bit);
            if (pc != pa) {
                if (mismatches < 10)
                    fprintf(stderr, "MISMATCH bit %ld.%d: C=%u asm=%u\n",
                            pos, i, pc, pa);
                mismatches++;
            }
        }
    }
    if (mismatches == 0) { printf("OK: %ld bytes, models match bit-exact\n", n); return 0; }
    printf("FAIL: %ld mismatching bits of %ld\n", mismatches, n * 8);
    return 1;
}
