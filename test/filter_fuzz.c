// Adversarial symmetry test: X86Unfilter(X86Filter(x)) == x must hold for ANY
// input, not just valid code. Throws random bytes, biased instruction-like
// streams, and boundary sizes at the filter and checks byte-exact recovery.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/x86filter.h"

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int check(const uint8_t *in, uint32_t n, uint32_t va, const char *tag) {
    uint32_t fs = 0;
    uint8_t *f = X86Filter(in, n, va, &fs);
    uint8_t *back = malloc(n + 32);
    memset(back, 0xAB, n + 32);
    uint32_t got = X86Unfilter(f, back, va);
    int ok = (got == n) && (n == 0 || memcmp(back, in, n) == 0);
    if (!ok) {
        uint32_t i = 0; while (i < n && back[i] == in[i]) i++;
        printf("FAIL %-12s n=%u got=%u firstdiff=%u\n", tag, n, got, i);
    }
    free(f); free(back);
    return ok;
}

int main(void) {
    int fail = 0, total = 0;
    uint8_t buf[8192];

    // 1. pure random bytes, many sizes
    for (int t = 0; t < 4000; t++) {
        uint32_t n = rnd() % sizeof(buf);
        for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd();
        total++; fail += !check(buf, n, 0x401000 + (rnd() & 0xff000), "random");
    }

    // 2. instruction-biased: lots of e8/e9/ff/0f/66 prefixes + modrm/sib
    static const uint8_t ops[] = {0xe8,0xe9,0xff,0x0f,0x66,0x8b,0x89,0xc3,0xcc,0x24,0x05,0x40,0x80};
    for (int t = 0; t < 4000; t++) {
        uint32_t n = rnd() % sizeof(buf);
        for (uint32_t i = 0; i < n; i++)
            buf[i] = (rnd() & 3) ? ops[rnd() % sizeof(ops)] : (uint8_t)rnd();
        total++; fail += !check(buf, n, 0x401000, "biased");
    }

    // 3. tiny boundary sizes 0..40
    for (uint32_t n = 0; n <= 40; n++) {
        for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd();
        total++; fail += !check(buf, n, 0x401000, "tiny");
    }

    // 4. all-same-byte runs (stresses match/escape paths)
    for (int b = 0; b < 256; b += 17) {
        uint32_t n = 1000 + (rnd() % 3000);
        memset(buf, b, n);
        total++; fail += !check(buf, n, 0x401000, "runs");
    }

    printf("%s: %d cases, %d failed\n", fail ? "FAIL" : "PASS", total, fail);
    return fail ? 1 : 0;
}
