// Clean-room parameterized context-mixing codec, for measuring the ratio cost
// of each model component. NOT the shipping codec — a research scratchpad to
// decide which leaner model fits the 5% budget before writing an asm decoder.
//
// Compile: cc -O2 -o cm research/cm.c
// Usage:   cm c <in> <out>   /   cm d <in> <out>
//          knobs via env: NORDERS (default 4), USE_MATCH=1, USE_APM=1, USE_SSE=0
//
// Model: a set of direct order-N byte contexts, each mapped to a probability
// via an adaptive bit-history -> probability map, logistically mixed, optional
// match model + APM. A binary range coder identical in spirit to the shipping
// one drives it. Self-contained and reversible (verified by roundtrip).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// ---- knobs ----
static int NORDERS = 4;     // number of order-0..N-1 byte contexts
static int USE_MATCH = 0;   // add a match model
static int USE_APM = 0;     // add an adaptive probability map (SSE) final stage

// ---- stretch/squash (12-bit prob domain) ----
static int stretch_tab[4096];
static int squash(int x){ // -2047..2047 -> 0..4095
    if(x<=-2047) return 1; if(x>=2047) return 4095;
    double v = 4096.0/(1.0+exp(-x/256.0));
    int r=(int)(v+0.5); return r<1?1:(r>4095?4095:r);
}
static void init_tables(void){
    int pi=0;
    for(int x=-2047;x<=2047;x++){ int s=squash(x); for(int j=pi;j<=s;j++) if(j<4096) stretch_tab[j]=x; pi=s+1; }
    for(int j=pi;j<4096;j++) stretch_tab[j]=2047;
}
static int stretch(int p){ return stretch_tab[p&4095]; }

// ---- range coder (carry-propagating, shiftLow style; p = P(bit==1), 12-bit) ----
typedef struct { uint8_t*out; uint64_t low; uint32_t range,cache,ff; int first; } Enc;
static void enc_init(Enc*e,uint8_t*o){e->out=o;e->low=0;e->range=~0u;e->cache=0;e->ff=0;e->first=1;}
static void enc_shift(Enc*e){
    uint32_t carry=(uint32_t)(e->low>>32);
    if(e->low<0xff000000ULL||carry==1){
        if(!e->first)*e->out++=(uint8_t)(e->cache+carry); else e->first=0;
        for(;e->ff;e->ff--)*e->out++=(uint8_t)(0xff+carry);
        e->cache=(uint32_t)((e->low>>24)&0xff);
    } else e->ff++;
    e->low=(e->low<<8)&0xffffffffULL;
}
static void enc_bit(Enc*e,int p,int bit){
    uint32_t bound=(uint32_t)(((uint64_t)e->range*p)>>12);
    if(bit) e->range=bound; else { e->low+=bound; e->range-=bound; }
    while(e->range<0x1000000){ e->range<<=8; enc_shift(e); }
}
static void enc_flush(Enc*e){ for(int i=0;i<5;i++) enc_shift(e); }

typedef struct { const uint8_t*in; uint32_t range,csub; } Dec;
static void dec_init(Dec*d,const uint8_t*i){d->in=i;d->range=~0u;d->csub=0;}
static int dec_bit(Dec*d,int p){
    uint32_t bound=(uint32_t)(((uint64_t)d->range*p)>>12);
    uint32_t code=((uint32_t)d->in[0]<<24)|((uint32_t)d->in[1]<<16)|((uint32_t)d->in[2]<<8)|d->in[3];
    code-=d->csub;
    int bit;
    if(bound>code){ d->range=bound; bit=1; } else { d->csub+=bound; d->range-=bound; bit=0; }
    while((d->range&0xff000000u)==0){ d->in++; d->range<<=8; d->csub<<=8; }
    return bit;
}

// ---- model ----
#define MAXORD 8
#define TBITS 22
#define TSIZE (1u<<TBITS)
static uint16_t *t[MAXORD];     // per-order: ctx-hash -> 12-bit prob (P(1))
static int wt[MAXORD+2];        // mixer weights (fixed <<16)
static uint32_t cx[MAXORD];     // per-order context hash for current byte
static uint32_t hbuf=0;         // history of recent bytes
static int useM;

// ---- match model: hash of last few bytes -> last position; predict the byte
// that followed last time. Strongest single component for repetitive code. ----
#define MMBITS 21
static uint32_t *mmtab;            // order-6 hash -> last position+1
static const uint8_t *mmbuf;       // output produced so far
static uint32_t mmpos, mmlen;      // current match pointer + length
static int mm_predbit;             // predicted next bit (valid if mmlen)

static void model_init(void){
    for(int i=0;i<NORDERS;i++){ t[i]=malloc(TSIZE*2); for(uint32_t j=0;j<TSIZE;j++) t[i][j]=2048; }
    for(int i=0;i<NORDERS+4;i++) wt[i]=0;
    hbuf=0;
    mmtab=calloc((size_t)1<<MMBITS,4); mmpos=mmlen=0;
}
// call after a full byte is known, before processing the next byte. `buf` is
// the produced bytes, `n` how many are valid (current byte at buf[n-1]).
static void match_update(const uint8_t*buf,uint32_t n){
    mmbuf=buf;
    if(mmlen){ mmpos++; mmlen++; if(mmbuf[mmpos-1]!=buf[n-1]) mmlen=0; }
    if(n>=6){
        uint32_t h=0; for(int k=0;k<6;k++) h=(h+buf[n-1-k]+1)*0x9e3779b1u; h>>=(32-MMBITS);
        uint32_t cand=mmtab[h];
        if(!mmlen && cand){ mmpos=cand; mmlen=1; }
        mmtab[h]=n;
    }
}
static uint32_t order_hash(int o,uint32_t c0){
    // hash o previous bytes (from hbuf) + partial current byte c0
    uint32_t h = c0*0x9e3779b1u;
    uint32_t hb=hbuf;
    for(int k=0;k<o;k++){ h = (h + (hb&0xff) + 1)*0x9e3779b1u; hb>>=8; }
    return (h>>(32-TBITS));
}
static int idx[MAXORD]; static int st[MAXORD];
static int mixer_in[MAXORD+4], NIN;
// predict P(1) for the next bit given partial byte c0 (leading-1 + bits so far)
static int predict(int c0){
    NIN=0;
    for(int i=0;i<NORDERS;i++){ idx[i]=order_hash(i,c0); st[i]=stretch(t[i][idx[i]]); mixer_in[NIN++]=st[i]; }
    if(useM){
        int v=0;
        if(mmlen){
            int nbits=0,cc=c0; while(cc>1){nbits++;cc>>=1;}      // bits decoded so far (0..7)
            int predbyte=mmbuf[mmpos];
            // does the match byte's high bits agree with c0 so far?
            if((predbyte>>(8-nbits)) == (c0 & ((1<<nbits)-1))){
                mm_predbit=(predbyte>>(7-nbits))&1;
                int conf = mmlen>28?28:mmlen;
                v = mm_predbit ? conf*64 : -conf*64;
            } else { mmlen=0; }
        }
        mixer_in[NIN++]=v;
    }
    long sum=0; for(int i=0;i<NIN;i++) sum += (long)wt[i]*mixer_in[i];
    int dot = (int)(sum>>16);
    if(dot<-2047)dot=-2047; if(dot>2047)dot=2047;
    return squash(dot);
}
static void update(int c0,int bit,int p){
    int err = ((bit<<12)-p);
    for(int i=0;i<NIN;i++){ wt[i]+= (mixer_in[i]*err)>>10; }
    for(int i=0;i<NORDERS;i++){ int pr=t[i][idx[i]]; pr += ((bit<<12)-pr)>>5; if(pr<1)pr=1; if(pr>4095)pr=4095; t[i][idx[i]]=pr; }
}

static uint32_t comp(const uint8_t*in,uint32_t n,uint8_t*out){
    init_tables(); useM=USE_MATCH; model_init();
    Enc e; enc_init(&e,out+4); out[0]=n;out[1]=n>>8;out[2]=n>>16;out[3]=n>>24;
    for(uint32_t i=0;i<n;i++){
        int c0=1;
        for(int b=7;b>=0;b--){ int bit=(in[i]>>b)&1; int p=predict(c0); enc_bit(&e,p,bit); update(c0,bit,p); c0=(c0<<1)|bit; }
        hbuf=(hbuf<<8)|in[i]; if(useM) match_update(in,i+1);
    }
    enc_flush(&e);
    return (uint32_t)(e.out-out);
}
static uint32_t decomp(const uint8_t*in,uint8_t*out){
    init_tables(); useM=USE_MATCH; model_init();
    uint32_t n=in[0]|(in[1]<<8)|(in[2]<<16)|((uint32_t)in[3]<<24);
    Dec d; dec_init(&d,in+4);
    for(uint32_t i=0;i<n;i++){
        int c0=1;
        for(int b=7;b>=0;b--){ int p=predict(c0); int bit=dec_bit(&d,p); update(c0,bit,p); c0=(c0<<1)|bit; }
        out[i]=(uint8_t)c0; hbuf=(hbuf<<8)|out[i]; if(useM) match_update(out,i+1);
    }
    return n;
}

int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s c|d in out\n",argv[0]);return 2;}
    if(getenv("NORDERS")) NORDERS=atoi(getenv("NORDERS"));
    if(getenv("USE_MATCH")) USE_MATCH=atoi(getenv("USE_MATCH"));
    if(getenv("USE_APM")) USE_APM=atoi(getenv("USE_APM"));
    FILE*f=fopen(argv[2],"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    uint8_t*buf=malloc(n+16);fread(buf,1,n,f);fclose(f);
    uint8_t*out=malloc(n*2+1024);
    uint32_t outn;
    if(argv[1][0]=='c') outn=comp(buf,n,out); else outn=decomp(buf,out);
    FILE*g=fopen(argv[3],"wb");fwrite(out,1,outn,g);fclose(g);
    fprintf(stderr,"%s %ld -> %u\n",argv[1][0]=='c'?"comp":"decomp",n,outn);
    return 0;
}
