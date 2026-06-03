// Differential test for the standalone asm unfilter (rekk_unfilter) vs the
// proven C X86Unfilter: filter real/fuzz inputs, unfilter both ways, compare.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/x86filter.h"

extern int rekk_unfilter(const uint8_t *packed, uint8_t *dst);

static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }

static int one(const uint8_t *in, uint32_t n, uint32_t va, const char *tag) {
    uint32_t fs = 0;
    uint8_t *f = X86Filter(in, n, va, &fs);

    uint8_t *cref = malloc(n + 64), *aout = malloc(n + 64);
    memset(cref, 0xC3, n + 64);
    memset(aout, 0xC3, n + 64);              // asm needs zeroed? no: filtered streams write every byte

    uint32_t cn = X86Unfilter(f, cref, va);
    int an = rekk_unfilter(f, aout);

    int ok = (cn == n) && ((uint32_t)an == n) && memcmp(cref, in, n) == 0 && memcmp(aout, in, n) == 0;
    if (!ok) {
        uint32_t i = 0; while (i < n && aout[i] == in[i]) i++;
        printf("FAIL %-10s n=%u cref=%u asm=%d firstdiff=%u\n", tag, n, cn, an, i);
    }
    free(f); free(cref); free(aout);
    return ok;
}

int main(int argc, char **argv) {
    int fail = 0, total = 0;

    // file arguments first (real x86 .text)
    for (int a = 1; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb"); if (!fp) continue;
        fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
        uint8_t *b=malloc(n); if(fread(b,1,n,fp)!=(size_t)n){fclose(fp);continue;} fclose(fp);
        total++; int ok = one(b, (uint32_t)n, 0x401000, argv[a]); fail += !ok;
        if (ok) printf("PASS %s (%ld bytes)\n", argv[a], n);
        free(b);
    }

    // fuzz: random + instruction-biased
    static const uint8_t ops[]={0xe8,0xe9,0xff,0x0f,0x66,0x8b,0x89,0xc3,0xcc,0x24,0x05,0x40};
    uint8_t buf[8192];
    for (int t=0;t<3000;t++){
        uint32_t n = rnd()%sizeof(buf);
        for(uint32_t i=0;i<n;i++) buf[i] = (rnd()&3)?ops[rnd()%sizeof(ops)]:(uint8_t)rnd();
        total++; fail += !one(buf, n, 0x401000, "fuzz");
    }
    for (uint32_t n=0;n<=40;n++){
        for(uint32_t i=0;i<n;i++) buf[i]=(uint8_t)rnd();
        total++; fail += !one(buf, n, 0x401000, "tiny");
    }

    printf("%s: %d cases, %d failed\n", fail?"FAIL":"PASS", total, fail);
    return fail?1:0;
}
