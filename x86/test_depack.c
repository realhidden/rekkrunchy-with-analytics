// Harness for the standalone x86 self-decompressor (rekk_depack).
// Decompresses a file produced by `rekkrunchy -c` and compares to the original.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int rekk_depack(const uint8_t *src, uint8_t *dst);  // -> decoded size

static uint8_t *slurp(const char *name, long *size) {
    FILE *f = fopen(name, "rb");
    if (!f) { perror(name); return NULL; }
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*size ? *size : 1);
    if (*size && fread(b, 1, *size, f) != (size_t)*size) { perror(name); free(b); fclose(f); return NULL; }
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <compressed> <original>\n", argv[0]); return 2; }
    long cs, es;
    uint8_t *comp = slurp(argv[1], &cs);
    uint8_t *exp  = slurp(argv[2], &es);
    if (!comp || !exp) return 2;

    // The size-optimized depacker does not write the bytes of an all-zero 8K
    // page (it just advances the cursor), mirroring the original kkrunchy
    // contract that output lands in a zero-initialized region. So zero the
    // buffer first, exactly as a real PE/.bss target would be.
    uint8_t *out = calloc(es + 16, 1);
    int n = rekk_depack(comp, out);

    if (n != es) { printf("FAIL: size %d != %ld\n", n, es); return 1; }
    if (memcmp(out, exp, es) != 0) {
        for (long i = 0; i < es; i++)
            if (out[i] != exp[i]) { printf("FAIL: first diff at %ld\n", i); return 1; }
    }
    printf("PASS: %ld -> %ld bytes\n", cs, es);
    return 0;
}
