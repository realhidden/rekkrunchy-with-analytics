// Stream-transform experiment harness.
//
// For each experiment we take the real X86Filter output (the 20-stream blob),
// apply a length-preserving, reversible transform to one or more stream regions,
// then compress the WHOLE blob through the actual shipping model (codec.c). This
// is the only valid signal: the model runs continuously over the blob, so we
// must measure the real -cx cost, not isolated per-stream compression.
// Each transform is verified invertible (restores the original stream bytes).
//
//   cc -O2 -Isrc -o se research/streamexp.c src/x86filter.c src/codec.c \
//      src/model.c src/tables.c
//   ./se corpus/*_text.bin
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "x86filter.h"
#include "codec.h"

#define NB 20

// ---- length-preserving reversible transforms on a byte range [p,p+n) ----

// byte-plane (AoS->SoA) over dwords: [b0b1b2b3][b0b1b2b3]... -> [all b0][all b1]...
static void byteplane(uint8_t *p, uint32_t n) {
    uint32_t m = n & ~3u; if (!m) return;
    uint8_t *t = malloc(m); uint32_t k = m / 4;
    for (uint32_t i = 0; i < k; i++) for (int b = 0; b < 4; b++) t[b*k + i] = p[i*4 + b];
    memcpy(p, t, m); free(t);
}
static void unbyteplane(uint8_t *p, uint32_t n) {
    uint32_t m = n & ~3u; if (!m) return;
    uint8_t *t = malloc(m); uint32_t k = m / 4;
    for (uint32_t i = 0; i < k; i++) for (int b = 0; b < 4; b++) t[i*4 + b] = p[b*k + i];
    memcpy(p, t, m); free(t);
}
// dword delta
static void ddelta(uint8_t *p, uint32_t n) {
    uint32_t k = n / 4, prev = 0;
    for (uint32_t i = 0; i < k; i++) { uint32_t v; memcpy(&v,p+i*4,4); uint32_t d=v-prev; prev=v; memcpy(p+i*4,&d,4); }
}
static void unddelta(uint8_t *p, uint32_t n) {
    uint32_t k = n / 4, prev = 0;
    for (uint32_t i = 0; i < k; i++) { uint32_t d; memcpy(&d,p+i*4,4); prev+=d; memcpy(p+i*4,&prev,4); }
}
// byte delta
static void bdelta(uint8_t *p, uint32_t n){ uint8_t pr=0; for(uint32_t i=0;i<n;i++){uint8_t v=p[i]; p[i]=v-pr; pr=v;} }
static void unbdelta(uint8_t *p, uint32_t n){ uint8_t pr=0; for(uint32_t i=0;i<n;i++){ pr+=p[i]; p[i]=pr; } }
// move-to-front
static void mtf(uint8_t *p, uint32_t n){ uint8_t t[256]; for(int i=0;i<256;i++)t[i]=i;
    for(uint32_t i=0;i<n;i++){ uint8_t c=p[i],j=0; while(t[j]!=c)j++; p[i]=j; memmove(t+1,t,j); t[0]=c; } }
static void unmtf(uint8_t *p, uint32_t n){ uint8_t t[256]; for(int i=0;i<256;i++)t[i]=i;
    for(uint32_t i=0;i<n;i++){ uint8_t j=p[i],c=t[j]; p[i]=c; memmove(t+1,t,j); t[0]=c; } }

enum { T_NONE, T_BYTEPLANE, T_DDELTA, T_BDELTA, T_MTF };
static void apply(int t, uint8_t*p, uint32_t n){
    if(t==T_BYTEPLANE)byteplane(p,n); else if(t==T_DDELTA)ddelta(p,n);
    else if(t==T_BDELTA)bdelta(p,n); else if(t==T_MTF)mtf(p,n);
}
static void invert(int t, uint8_t*p, uint32_t n){
    if(t==T_BYTEPLANE)unbyteplane(p,n); else if(t==T_DDELTA)unddelta(p,n);
    else if(t==T_BDELTA)unbdelta(p,n); else if(t==T_MTF)unmtf(p,n);
}

typedef struct { const char *name; int stream; int transform; } Exp;
static Exp experiments[] = {
    {"byteplane imm32(#12)",   12, T_BYTEPLANE},
    {"byteplane disp32(#13)",  13, T_BYTEPLANE},
    {"byteplane rel32(#17)",   17, T_BYTEPLANE},
    {"byteplane abs(#15)",     15, T_BYTEPLANE},
    {"byteplane call-new(#18)",18, T_BYTEPLANE},
    {"ddelta   imm32(#12)",    12, T_DDELTA},
    {"ddelta   disp32(#13)",   13, T_DDELTA},
    {"ddelta   rel32(#17)",    17, T_DDELTA},
    {"ddelta   call-new(#18)", 18, T_DDELTA},
    {"bdelta   rel8(#9)",       9, T_BDELTA},
    {"bdelta   imm8(#10)",     10, T_BDELTA},
    {"mtf      call-index(#16)",16, T_MTF},
    {"mtf      opcodes(#0)",    0, T_MTF},
    {"bdelta   disp8/r4(#5)",   5, T_BDELTA},
    {"mtf      rel8(#9)",       9, T_MTF},
    {"bdelta   call-index(#16)",16, T_BDELTA},
};
#define NEXP ((int)(sizeof(experiments)/sizeof(experiments[0])))

static uint32_t comp(const uint8_t*p,uint32_t n){ uint32_t o=0; uint8_t*c=Compress(p,n,&o); free(c); return o; }

int main(int argc,char**argv){
    long totbase=0; long totexp[NEXP]; for(int e=0;e<NEXP;e++) totexp[e]=0;
    int rtfail[NEXP]; memset(rtfail,0,sizeof rtfail);

    for(int a=1;a<argc;a++){
        FILE*f=fopen(argv[a],"rb"); if(!f)continue;
        fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
        uint8_t*code=malloc(n); if(fread(code,1,n,f)!=(size_t)n){fclose(f);continue;} fclose(f);
        uint32_t fs=0; uint8_t*blob=X86Filter(code,(uint32_t)n,0x401000,&fs);
        // stream offsets
        uint32_t sz[NB],off[NB]; const uint8_t*hp=blob+4; uint32_t acc=4+NB*4;
        for(int i=0;i<NB;i++){ memcpy(&sz[i],hp,4); hp+=4; off[i]=acc; acc+=sz[i]; }
        uint32_t base=comp(blob,fs); totbase+=base;

        for(int e=0;e<NEXP;e++){
            int s=experiments[e].stream, t=experiments[e].transform;
            uint8_t*mod=malloc(fs); memcpy(mod,blob,fs);
            uint8_t*reg=mod+off[s];
            // save, transform, compress
            uint8_t*orig=malloc(sz[s]); memcpy(orig,reg,sz[s]);
            apply(t,reg,sz[s]);
            totexp[e]+=comp(mod,fs);
            // verify invertible
            invert(t,reg,sz[s]);
            if(memcmp(reg,orig,sz[s])!=0) rtfail[e]++;
            free(orig); free(mod);
        }
        free(code); free(blob);
    }

    printf("baseline -cx total: %ld\n\n", totbase);
    printf("%-26s %10s %8s %s\n","experiment","total","delta","rt");
    for(int e=0;e<NEXP;e++)
        printf("%-26s %10ld %+8ld %s\n", experiments[e].name, totexp[e],
               totexp[e]-totbase, rtfail[e]?"FAIL":"ok");
    return 0;
}
