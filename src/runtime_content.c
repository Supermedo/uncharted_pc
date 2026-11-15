/* Offline AppContent provider for the configured base-game profile.
 * Nathan Drake Pack metadata is reported installed for the matching title only.
 * No package mounting, downloads or license emulation. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); runtime_quit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); runtime_quit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); runtime_quit(21); }
}
/* This entitlement-only pack has no separate game-data files to mount.  Bind its
 * metadata to the configured collection title; never enumerate other titles. */
#define NATHAN_PACK_LABEL "NATHANDRAKEPACK1"
typedef struct { char label[17]; char padding[3]; uint32_t status; } AddonInfo;
_Static_assert(sizeof(AddonInfo)==24, "SceAppContentAddcontInfo ABI");
_Static_assert(sizeof(NATHAN_PACK_LABEL)==17, "entitlement label ABI");
static unsigned nathan_pack_installed;
static uint16_t psf_u16(const unsigned char *p) {
    return (uint16_t)p[0] | (uint16_t)p[1]<<8;
}
static uint32_t psf_u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static int metadata_rejected(void) {
    puts("Runtime: Nathan Drake Pack metadata rejected: invalid identity or bounds"); return 0;
}
static int nathan_pack_present(void) {
    const char *user=runtime_file_user_dir();
    /* Uncharted profiles without USER_DEFINED_PARAM_1 carry the CUSA number
     * in slot 1 (the upstream content_profile.py fallback). This provider
     * targets that collection layout; it is not a general title-ID accessor. */
    if (!user || !*user || !parameters[1] || parameters[1]>99999) {
        puts("Runtime: Nathan Drake Pack not configured: missing user root or valid title number"); return 0;
    }
    char title[10], suffix[40];
    static char path[4096];
    snprintf(title,sizeof(title),"CUSA%05u",parameters[1]);
    int used=snprintf(path,sizeof(path),"%s/addcont/%s/%s/sce_sys/param.sfo",user,title,NATHAN_PACK_LABEL);
    if (used<0 || (size_t)used>=sizeof(path)) return 0;
    snprintf(suffix,sizeof(suffix),"-%s_00-%s",title,NATHAN_PACK_LABEL);
    FILE *f=fopen(path,"rb");
    if (!f) { puts("Runtime: Nathan Drake Pack metadata not found or unreadable"); return 0; }
    static unsigned char data[65536];
    size_t size=fread(data,1,sizeof(data),f);
    int bad=ferror(f) || fgetc(f)!=EOF;
    fclose(f);
    if (bad) { puts("Runtime: Nathan Drake Pack metadata read error or oversize"); return 0; }
    puts("Runtime: Nathan Drake Pack metadata found; validating identity and bounds");
    if (size<20 || memcmp(data,"\0PSF",4) || psf_u32(data+4)!=0x101) return metadata_rejected();
    uint32_t keys=psf_u32(data+8), values=psf_u32(data+12), count=psf_u32(data+16);
    if (count>(size-20)/16 || keys<20+(uint64_t)count*16 || keys>values || values>size) return metadata_rejected();
    unsigned found=0;
    for (uint32_t i=0;i<count;++i) {
        const unsigned char *entry=data+20+(size_t)i*16;
        uint16_t key_offset=psf_u16(entry), format=psf_u16(entry+2);
        uint32_t length=psf_u32(entry+4), maximum=psf_u32(entry+8), offset=psf_u32(entry+12);
        if (key_offset>=values-keys || length>maximum || offset>size-values || maximum>size-values-offset) return metadata_rejected();
        if (key_offset && data[keys+key_offset-1]) return metadata_rejected();
        const char *key=(const char *)data+keys+key_offset;
        if (!memchr(key,0,values-keys-key_offset)) return metadata_rejected();
        const unsigned char *value=data+values+offset;
        unsigned bit=0;
        if (!strcmp(key,"TITLE_ID")) {
            bit=1;
            if (format!=0x204 || length!=strlen(title)+1 || memcmp(value,title,length)) return metadata_rejected();
        } else if (!strcmp(key,"CATEGORY")) {
            bit=2;
            if (format!=0x204 || length!=3 || memcmp(value,"ac",3)) return metadata_rejected();
        } else if (!strcmp(key,"CONTENT_ID")) {
            bit=4;
            if (format!=0x204 || length!=6+strlen(suffix)+1 || memcmp(value+6,suffix,strlen(suffix)+1)) return metadata_rejected();
            for (unsigned j=0;j<6;++j) if (j<2 ? !(value[j]>='A' && value[j]<='Z') : !(value[j]>='0' && value[j]<='9')) return metadata_rejected();
        }
        if (bit && (found&bit)) return metadata_rejected();
        found|=bit;
    }
    return found==7 ? 1 : metadata_rejected();
}

static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    nathan_pack_installed=nathan_pack_present();
    printf("Runtime: AppContent initialized; Nathan Drake Pack installed=%u for CUSA%05u\n",nathan_pack_installed,parameters[1]);
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); runtime_quit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); runtime_quit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    /* Capacity zero/null list is a count query; never touch unused slots. */
    if (nathan_pack_installed && list && capacity) {
        AddonInfo info={{0},{0},4};
        memcpy(info.label,NATHAN_PACK_LABEL,sizeof(NATHAN_PACK_LABEL));
        memcpy(list,&info,sizeof(info));
    }
    if (hits) *hits=nathan_pack_installed;
    if (lists<4) printf("Runtime: AppContent add-on query: installed=%u capacity=%u buffer=%u written=%u\n",nathan_pack_installed,capacity,list!=NULL,(nathan_pack_installed && list && capacity)?1u:0u);
    ++lists; return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#M#N")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#M#N")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#M#N")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#c#d")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#c#d")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#c#d")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
