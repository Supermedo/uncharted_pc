#include "runtime.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__attribute__((noreturn)) void runtime_quit(int code) { exit(code); }
static const char *user;
const char *runtime_file_user_dir(void) { return user; }
typedef int32_t (ABI *ModuleFn)(uint16_t);
typedef int32_t (ABI *InitFn)(const unsigned char *,unsigned char *);
typedef int32_t (ABI *ListFn)(uint32_t,void *,uint32_t,uint32_t *);
typedef int32_t (ABI *ParamFn)(uint32_t,int32_t *);
int main(int argc,char **argv) {
    assert(argc==4);
    user=!strcmp(argv[1],"NULL")?NULL:(!strcmp(argv[1],"EMPTY")?"":argv[1]); unsigned expected=(unsigned)atoi(argv[2]);
    uint32_t profile[5]={getenv("TEST_TRIAL")?1u:3u,(uint32_t)atoi(argv[3]),11,22,33};
    runtime_content_configure(profile);
    ModuleFn load=(ModuleFn)runtime_content_resolve("g8cM39EUZ6o#M#N");
    ModuleFn loaded=(ModuleFn)runtime_content_resolve("fMP5NHUOaMk#M#N");
    ModuleFn unload=(ModuleFn)runtime_content_resolve("eR2bZFAAU0Q#M#N");
    InitFn init=(InitFn)runtime_content_resolve("R9lA82OraNs#c#d");
    ListFn list=(ListFn)runtime_content_resolve("xnd8BJzAxmk#c#d");
    ParamFn param=(ParamFn)runtime_content_resolve("99b82IKXpH4#c#d");
    assert(load && loaded && unload && init && list && param);
    if(getenv("TEST_BEFORE_LOAD"))list(0,NULL,0,NULL);
    assert(load(0xb4)==0); assert(loaded(0xb4)==0);
    if(getenv("TEST_BEFORE_INIT"))list(0,NULL,0,NULL);
    assert(load(0xb4)==0);
    unsigned char init_data[32]={0},boot[40];
    assert(init(NULL,boot)==(int32_t)0x80d90002);
    assert(init(init_data,NULL)==(int32_t)0x80d90002);
    init_data[0]=1;assert(init(init_data,boot)==(int32_t)0x80d90002);init_data[0]=0;
    memset(boot,0xa5,sizeof(boot)); assert(init(init_data,boot)==0);
    for(unsigned i=0;i<40;i++) assert(!boot[i]);
    assert(init(init_data,boot)==(int32_t)0x80d90003);
    int32_t value=0; assert(param(5,&value)==(int32_t)0x80d90002); assert(param(0,NULL)==(int32_t)0x80d90002); for(unsigned i=0;i<5;i++){assert(param(i,&value)==0);assert((uint32_t)value==profile[i]);}
    uint32_t hits=999; assert(list(0,NULL,0,&hits)==0 && hits==expected);
    unsigned char buffer[72]; memset(buffer,0xa5,sizeof(buffer));
    assert(list(0,buffer,0,&hits)==0 && hits==expected);
    for(unsigned i=0;i<sizeof(buffer);i++) assert(buffer[i]==0xa5);
    assert(list(0,buffer,2,&hits)==0 && hits==expected);
    if(expected){
        assert(!memcmp(buffer,"NATHANDRAKEPACK1",17));
        for(unsigned i=17;i<20;i++) assert(!buffer[i]);
        uint32_t status;memcpy(&status,buffer+20,4);assert(status==4);
    }
    for(unsigned i=expected?24:0;i<sizeof(buffer);i++) assert(buffer[i]==0xa5);
    assert(list(0,NULL,99,&hits)==0 && hits==expected);
    assert(list(0,NULL,0,NULL)==(int32_t)0x80d90002);
    assert(list(0,buffer,0,NULL)==(int32_t)0x80d90002);
    memset(buffer,0xa5,sizeof(buffer));
    assert(list(0,buffer,1,&hits)==0 && hits==expected);
    for(unsigned i=expected?24:0;i<sizeof(buffer);i++)assert(buffer[i]==0xa5);
    memset(buffer,0xa5,sizeof(buffer));assert(list(0,buffer,1,NULL)==0);
    for(unsigned i=expected?24:0;i<sizeof(buffer);i++)assert(buffer[i]==0xa5);
    if(getenv("TEST_BAD_SERVICE")) list(1,buffer,1,&hits);
    assert(unload(0xb4)==0);assert(loaded(0xb4)==0);
    assert(list(0,NULL,0,&hits)==0 && hits==expected);
    assert(unload(0xb4)==0);
    assert(loaded(0xb4)==(int32_t)0x805a1001);
    if(getenv("TEST_REMOVE_METADATA")){ char path[4096]; snprintf(path,sizeof(path),"%s/addcont/CUSA%05u/NATHANDRAKEPACK1/sce_sys/param.sfo",user,profile[1]);assert(remove(path)==0);expected=0;}
    assert(load(0xb4)==0); assert(init(init_data,boot)==0);
    assert(list(0,NULL,0,&hits)==0 && hits==expected); assert(unload(0xb4)==0);
    runtime_content_report(); puts("API PASS");
    return 0;
}
