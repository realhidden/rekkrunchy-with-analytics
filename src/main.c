// rekkrunchy — cross-platform context-mixing compressor.
// Portable C port of rygs kkrunchy codec. Standard library only.
//
//   rekkrunchy -c <in> <out>    compress
//   rekkrunchy -d <in> <out>    decompress
//
#include "codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *name, uint32_t *size) {
    FILE *f = fopen(name, "rb");
    if (!f) { perror(name); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        perror(name); free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

static int store(const char *name, const uint8_t *data, uint32_t size) {
    FILE *f = fopen(name, "wb");
    if (!f) { perror(name); return 0; }
    int ok = (size == 0) || (fwrite(data, 1, size, f) == size);
    if (!ok) perror(name);
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "-c") && strcmp(argv[1], "-d"))) {
        fprintf(stderr,
            "rekkrunchy — context-mixing compressor (kkrunchy codec)\n"
            "usage:\n"
            "  %s -c <in> <out>   compress\n"
            "  %s -d <in> <out>   decompress\n", argv[0], argv[0]);
        return 1;
    }

    uint32_t inSize = 0;
    uint8_t *in = load(argv[2], &inSize);
    if (!in) return 1;

    uint32_t outSize = 0;
    uint8_t *out = (argv[1][1] == 'c')
        ? Compress(in, inSize, &outSize)
        : Decompress(in, inSize, &outSize);
    if (!out) { fprintf(stderr, "error: out of memory\n"); free(in); return 1; }

    int ok = store(argv[3], out, outSize);
    if (ok) {
        if (argv[1][1] == 'c')
            printf("compressed %u -> %u bytes\n", inSize, outSize);
        else
            printf("decompressed %u -> %u bytes\n", inSize, outSize);
    }
    free(in);
    free(out);
    return ok ? 0 : 1;
}
