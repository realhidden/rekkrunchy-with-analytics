#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern void  model_init_c(void);
extern void *get_workdata(void);
extern void  get_offsets(int *out);

int main(void){
    model_init_c();
    uint8_t *wd = (uint8_t*)get_workdata();
    int off[32]; memset(off,0,sizeof(off));  // oracle writes up to index 18
    get_offsets(off);
    int o_runTable=off[0], o_stateCode=off[1], o_stateNext=off[2],
        o_stateMap=off[3], o_stretch=off[4], o_APM=off[5], o_cm=off[6],
        o_worksize=off[7], o_cmsize=off[8], o_modelMem=off[9];
    fprintf(stderr,"// Work.size=%d ContextModel.size=%d\n", o_worksize, o_cmsize);
    fprintf(stderr,"// offs run=%d stateCode=%d stateNext=%d stateMap=%d stretch=%d APM=%d cm=%d modelMem=%d\n",
        o_runTable,o_stateCode,o_stateNext,o_stateMap,o_stretch,o_APM,o_cm,o_modelMem);

    uint32_t *runTable = (uint32_t*)(wd+o_runTable);
    uint8_t  *stateCode= (uint8_t *)(wd+o_stateCode);   // 256*2 bytes
    uint8_t  *stateNext= (uint8_t *)(wd+o_stateNext);   // 256*2 bytes
    uint32_t *stateMap = (uint32_t*)(wd+o_stateMap);    // 256
    uint32_t *stretch  = (uint32_t*)(wd+o_stretch);     // 4096

    printf("// Auto-generated from model_asm.asm init. Do not edit.\n");
    printf("#include <stdint.h>\n\n");

    printf("const uint32_t RUNTABLE[256] = {\n");
    for(int i=0;i<256;i++){ printf("%uU,", runTable[i]); if(i%8==7)printf("\n"); }
    printf("};\n\n");

    printf("const uint8_t STATECODE[512] = {\n");
    for(int i=0;i<512;i++){ printf("%u,", stateCode[i]); if(i%16==15)printf("\n"); }
    printf("};\n\n");

    printf("const uint8_t STATENEXT[512] = {\n");
    for(int i=0;i<512;i++){ printf("%u,", stateNext[i]); if(i%16==15)printf("\n"); }
    printf("};\n\n");

    printf("const uint32_t STATEMAP_INIT[256] = {\n");
    for(int i=0;i<256;i++){ printf("%uU,", stateMap[i]); if(i%8==7)printf("\n"); }
    printf("};\n\n");

    printf("const uint32_t STRETCH[4096] = {\n");
    for(int i=0;i<4096;i++){ printf("%uU,", stretch[i]); if(i%8==7)printf("\n"); }
    printf("};\n\n");
    return 0;
}
