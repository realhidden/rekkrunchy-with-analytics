// Standalone roundtrip test for the x86 split-stream filter:
//   X86Unfilter(X86Filter(code)) == code
// Run on real x86 .text sections. Reports PASS/FAIL + stream expansion.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/x86filter.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <x86-code-file> [va]\n", argv[0]); return 2; }
    uint32_t va = (argc > 2) ? (uint32_t)strtoul(argv[2], 0, 0) : 0x401000;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *code = malloc(n);
    if (fread(code, 1, n, f) != (size_t)n) { perror("read"); return 2; }
    fclose(f);

    uint32_t fsize = 0;
    uint8_t *filtered = X86Filter(code, (uint32_t)n, va, &fsize);

    uint8_t *back = malloc(n + 16);
    memset(back, 0xCC, n + 16);
    uint32_t got = X86Unfilter(filtered, back, va);

    int ok = (got == (uint32_t)n) && memcmp(back, code, n) == 0;
    if (!ok) {
        long i = 0; while (i < n && back[i] == code[i]) i++;
        printf("FAIL %-18s got %u/%ld, first diff @ %ld\n", argv[1], got, n, i);
    } else {
        printf("PASS %-18s %ld -> filtered %u bytes\n", argv[1], n, fsize);
    }
    free(code); free(filtered); free(back);
    return ok ? 0 : 1;
}
