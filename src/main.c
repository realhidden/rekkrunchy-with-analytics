// rekkrunchy — cross-platform context-mixing compressor.
// Portable C port of rygs kkrunchy codec. Standard library only.
//
//   rekkrunchy -c  <in> <out>        compress
//   rekkrunchy -cx <in> <out> [va]   compress with the x86 split-stream filter
//   rekkrunchy -d  <in> <out>        decompress (auto-detects the filter)
//
// Container: [flag:1][orig size:4, only if x86][codec blob]. The flag records
// whether the x86 filter was applied; the filter's load address lives inside
// the codec blob, so decompression needs no extra arguments.
#include "codec.h"
#include "x86filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLAG_RAW 0
#define FLAG_X86 1
#define DEFAULT_VA 0x401000u   // typical PE .text base; any value roundtrips

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

static int usage(const char *p) {
    fprintf(stderr,
        "rekkrunchy — context-mixing compressor (kkrunchy codec)\n"
        "usage:\n"
        "  %s -c  <in> <out>        compress\n"
        "  %s -cx <in> <out> [va]   compress with x86 split-stream filter\n"
        "  %s -d  <in> <out>        decompress\n", p, p, p);
    return 1;
}

// compressed file = [flag][orig size if x86][Compress() blob]
static uint8_t *do_compress(const uint8_t *in, uint32_t inSize, int x86,
                            uint32_t va, uint32_t *outSize) {
    const uint8_t *payload = in;
    uint8_t *filtered = NULL;
    uint32_t payloadSize = inSize;
    if (x86) {
        filtered = X86Filter(in, inSize, va, &payloadSize);
        payload = filtered;
    }

    uint32_t blobSize = 0;
    uint8_t *blob = Compress(payload, payloadSize, &blobSize);
    free(filtered);
    if (!blob) return NULL;

    uint32_t hdr = x86 ? 5 : 1;
    uint8_t *out = malloc(hdr + blobSize);
    if (!out) { free(blob); return NULL; }
    out[0] = x86 ? FLAG_X86 : FLAG_RAW;
    if (x86) memcpy(out + 1, &inSize, 4);     // original (pre-filter) size
    memcpy(out + hdr, blob, blobSize);
    free(blob);
    *outSize = hdr + blobSize;
    return out;
}

static uint8_t *do_decompress(const uint8_t *in, uint32_t inSize, uint32_t *outSize) {
    int x86 = in[0] == FLAG_X86;
    uint32_t hdr = x86 ? 5 : 1;
    uint32_t origSize = 0;
    if (x86) memcpy(&origSize, in + 1, 4);

    uint32_t blobSize = 0;
    uint8_t *blob = Decompress(in + hdr, inSize - hdr, &blobSize);
    if (!blob) return NULL;
    if (!x86) { *outSize = blobSize; return blob; }

    uint8_t *out = malloc(origSize ? origSize : 1);
    if (!out) { free(blob); return NULL; }
    uint32_t got = X86Unfilter(blob, out, 0);   // va is read from the blob
    free(blob);
    *outSize = got;
    return out;
}

int main(int argc, char **argv) {
    if (argc < 4) return usage(argv[0]);
    const char *mode = argv[1];
    int compress = !strcmp(mode, "-c") || !strcmp(mode, "-cx");
    int x86 = !strcmp(mode, "-cx");
    int decompress = !strcmp(mode, "-d");
    if ((!compress && !decompress) || (decompress && argc != 4)) return usage(argv[0]);

    uint32_t va = (x86 && argc > 5) ? (uint32_t)strtoul(argv[5], 0, 0) : DEFAULT_VA;

    uint32_t inSize = 0;
    uint8_t *in = load(argv[2], &inSize);
    if (!in) return 1;

    uint32_t outSize = 0;
    uint8_t *out = compress ? do_compress(in, inSize, x86, va, &outSize)
                            : do_decompress(in, inSize, &outSize);
    if (!out) { fprintf(stderr, "error: out of memory\n"); free(in); return 1; }

    int ok = store(argv[3], out, outSize);
    if (ok)
        printf("%s %u -> %u bytes%s\n", compress ? "compressed" : "decompressed",
               inSize, outSize, x86 ? " (x86 filter)" : "");
    free(in);
    free(out);
    return ok ? 0 : 1;
}
