#define _GNU_SOURCE
#include "runtime.h"
#include <stdlib.h>

/* The settings menu of the GPU library restarts through probe.c, which tests do not link. */
void runtime_restart(void) { abort(); }
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int32_t (ABI *Initialize)(int64_t,uint32_t *);
typedef int32_t (ABI *Register)(uint32_t,uint32_t,int64_t);
typedef int32_t (ABI *Create)(uint32_t,uint32_t,uint64_t,uint32_t *);
#define GET(t,n) ((t)runtime_resolve(n,0))
int main(void) {
    runtime_start(1);
    Initialize init=GET(Initialize,"dl+4eHSzUu4#N#O");
    Register reg=GET(Register,"Q3dyFuwGn64#N#O");
    Create create=GET(Create,"AxoDrINp4J8#N#O");
    assert(init && reg && create);
    uint32_t context=0;
    assert(init(0,&context)==0 && context==1);
    assert(reg(context,0,0)==0 && reg(context,1,0)==0 && reg(context,2,0)==0);
    /* AAC (codec 2) is not implemented: an error code comes back and the process keeps running. */
    uint32_t out=0xdeadbeef;
    assert((uint32_t)create(context,2,1,&out)==0x80930005 && out==0xdeadbeef);
    /* MP3 (codec 0) and ATRAC9 (codec 1) still create instances. */
    assert(create(context,0,1,&out)==0 && out==0x1);
    assert(create(context,1,1,&out)==0 && out==0x4002);
    puts("PASS: Ajm unsupported codec returns an error");
    return 0;
}
