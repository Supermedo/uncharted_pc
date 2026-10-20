/* Windows and the System V red zone. PS4 code (System V ABI) may keep data in the 128 bytes
 * below rsp in a function that calls nothing; signals on FreeBSD and Linux are delivered below
 * them. Windows puts an exception's frame right below rsp: with the thread's AVX state in use
 * (the upper halves of the YMM registers) it overwrites the stack from the 64-byte boundary at
 * or below rsp down, 72 to 128 of those bytes (Xeon E3-1240 v2, Windows 10;
 * tests/test_red_zone.c). Guest faults are routine here: the GPU code protects guest pages it
 * tracks and the access goes on after the fault. One inside such a function lost its data.
 * U1 1.02's constant table builder at 0x63df10 (U2 0xd645f0, U3 0x12d6cf0) keeps its entry
 * counters and store pointers there: an entry stayed unwritten, and a later draw called the
 * null member function its type picked ("execute access to 0", #0 guest offset 0x641532).
 *
 * Before the guest starts, the functions of each module come from its .eh_frame_hdr. In one
 * that uses memory below rsp ([rsp-n], or [rbp-n] past its frame) and calls nothing, each
 * access that can fault (not the stack, fs/gs or read-only image data) runs from a thunk
 * between lea rsp,[rsp-128] and lea rsp,[rsp+128] (lea keeps the flags), so a fault's frame
 * lands below the data. A jump takes 5 bytes: a shorter access moves its neighbours along when
 * no direct branch lands between them, else it stays as it was (a breakpoint would be an
 * exception itself). Functions with indirect jumps, data in their code (jump tables), calls or
 * exception tables are left alone. Runs after every other code patch: a jump out of the image
 * there (a hook, a rewritten instruction) is not moved, and its thunk may come back to the
 * instruction after its padding. */
#include "red_zone.h"
#include <Zydis/Zydis.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Instructions per region, bytes a region's thunk may take, thunk memory per allocation. */
enum { REGION_ITEMS=6, REGION_MAX=256, CHUNK=1<<20 };

typedef struct { uint64_t start, end; int lsda; } Function;
typedef struct { uint64_t thunk, end, site; unsigned char seen; } Region;
/* access: a memory access that can fault, run with rsp lowered; rsp: the instruction uses rsp. */
typedef struct { uint64_t at; ZydisDecodedInstruction in; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]; int access, rsp; } Item;
typedef struct { unsigned char *p; size_t n; } Code;

static unsigned char *image;
static uint64_t image_size;
static const RedZoneSegment *segments;
static size_t segment_count;
static ZydisDecoder decoder;
static unsigned char *chunk;
static size_t chunk_used=CHUNK;
static Region *regions;
static size_t region_count, region_capacity;
static uint64_t faults_kept;
static int listing;                         /* BB_RED_ZONE=list: each function and access */
/* What keeps data below rsp and was not rewritten (functions left alone, accesses left as they
 * were), sorted by start: a fault there is reported. */
typedef struct { uint64_t start, end; const char *why; } Unprotected;
static Unprotected *unprotected;
static size_t unprotected_count, unprotected_capacity;
static uint64_t unprotected_faults;
static void note_unprotected(uint64_t start, uint64_t end, const char *why) {
    if (unprotected_count==unprotected_capacity) {
        Unprotected *more=realloc(unprotected,(unprotected_capacity=unprotected_capacity ? unprotected_capacity*2 : 256)*sizeof(*more));
        if (!more) return;
        unprotected=more;
    }
    unprotected[unprotected_count++]=(Unprotected){start,end,why};
}

static const RedZoneSegment *segment_of(uint64_t offset) {
    for (size_t i=0;i<segment_count;++i)
        if (offset-segments[i].address<segments[i].size) return &segments[i];
    return NULL;
}
static int bit(const unsigned char *bits, uint64_t offset) { return bits[offset>>3]>>(offset&7)&1; }
static void mark(unsigned char *bits, uint64_t offset) { if (offset<image_size) bits[offset>>3]|=(unsigned char)(1u<<(offset&7)); }

/* .eh_frame (DWARF CFI with the GNU 'z' augmentation): a function's length and whether it has
 * an exception table (LSDA). */
static uint64_t uleb(const unsigned char **p, const unsigned char *end) {
    uint64_t v=0;
    for (unsigned shift=0; *p<end && shift<64; shift+=7) { const unsigned char c=*(*p)++; v|=(uint64_t)(c&0x7f)<<shift; if (!(c&0x80)) break; }
    return v;
}
static unsigned encoded_size(unsigned encoding) {
    switch (encoding&0x0f) {
    case 0x00: case 0x04: case 0x0c: return 8;
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    default: return 0;
    }
}
static uint64_t read_value(const unsigned char *p, unsigned size) { uint64_t v=0; memcpy(&v,p,size); return v; }
typedef struct { unsigned fde_encoding, lsda_encoding; int augmented, lsda; } Cie;
static int parse_cie(const unsigned char *cie, Cie *out) {
    const unsigned char *limit=image+image_size;
    uint32_t length, id;
    if (cie<image || cie+8>limit) return 0;
    memcpy(&length,cie,4); memcpy(&id,cie+4,4);
    if (!length || length==0xffffffffu || id || cie+4+length>limit) return 0;
    const unsigned char *p=cie+8, *end=cie+4+length;
    const unsigned version=*p++;
    const char *augmentation=(const char *)p;
    const size_t n=strnlen(augmentation,(size_t)(end-p));
    if (p+n>=end || strchr(augmentation,'e')) return 0;                 /* "eh": old GCC data */
    p+=n+1;
    uleb(&p,end); uleb(&p,end);                                          /* code and data alignment */
    if (version==1) ++p; else uleb(&p,end);                              /* return address register */
    *out=(Cie){0,0xff,augmentation[0]=='z',0};
    if (!out->augmented) return 1;
    uleb(&p,end);
    for (const char *a=augmentation+1; *a; ++a) {
        if (p>=end) return 0;
        if (*a=='R') out->fde_encoding=*p++;
        else if (*a=='L') { out->lsda=1; out->lsda_encoding=*p++; }
        else if (*a=='P') { const unsigned size=encoded_size(*p++); if (!size) return 0; p+=size; }
        else if (*a!='S' && *a!='B') return 0;
    }
    return 1;
}
static int parse_fde(uint64_t offset, uint64_t *range, int *lsda) {
    const unsigned char *fde=image+offset, *limit=image+image_size;
    uint32_t length, back;
    if (offset+8>image_size) return 0;
    memcpy(&length,fde,4); memcpy(&back,fde+4,4);
    if (!length || length==0xffffffffu || !back || fde+4+length>limit) return 0;
    Cie cie;
    if ((uint64_t)back>offset+4 || !parse_cie(fde+4-back,&cie)) return 0;
    const unsigned size=encoded_size(cie.fde_encoding);
    const unsigned char *p=fde+8+size, *end=fde+4+length;
    if (!size || p+size>end) return 0;
    *range=read_value(p,size);
    p+=size;
    *lsda=0;
    if (cie.augmented && cie.lsda && cie.lsda_encoding!=0xff) {
        uleb(&p,end);
        const unsigned lsda_size=encoded_size(cie.lsda_encoding);
        *lsda=!lsda_size || p+lsda_size>end || read_value(p,lsda_size)!=0;
    }
    return 1;
}
/* Every module's .eh_frame_hdr: version 1, pcrel sdata4 .eh_frame pointer, udata4 count and a
 * table of datarel sdata4 pairs (function start, FDE), sorted by start. */
static Function *find_functions(size_t *count) {
    Function *functions=NULL;
    size_t n=0, capacity=0;
    for (size_t s=0;s<segment_count;++s) {
        if (!(segments[s].flags&4)) continue;
        const uint64_t end=segments[s].address+segments[s].size;
        for (uint64_t at=(segments[s].address+3)&~(uint64_t)3; at+12<=end; at+=4) {
            const unsigned char *h=image+at;
            if (h[0]!=1 || h[1]!=0x1b || h[2]!=0x03 || h[3]!=0x3b) continue;
            uint32_t entries;
            memcpy(&entries,h+8,4);
            if (!entries || at+12+(uint64_t)entries*8>end) continue;
            const size_t first=n;
            uint64_t previous=0;
            int good=1;
            for (uint32_t i=0;i<entries && good;++i) {
                int32_t location, fde;
                memcpy(&location,h+12+8*(uint64_t)i,4); memcpy(&fde,h+16+8*(uint64_t)i,4);
                const uint64_t start=at+(uint64_t)(int64_t)location;
                const RedZoneSegment *code=segment_of(start);
                uint64_t range=0;
                int lsda=0;
                if (!code || !(code->flags&1) || start<previous || !parse_fde(at+(uint64_t)(int64_t)fde,&range,&lsda)) { good=0; break; }
                previous=start;
                if (n==capacity) {
                    Function *more=realloc(functions,(capacity=capacity ? capacity*2 : 4096)*sizeof(*more));
                    if (!more) { good=0; break; }
                    functions=more;
                }
                const uint64_t limit=code->address+code->size;
                functions[n++]=(Function){start,range<limit-start ? start+range : limit,lsda};
            }
            if (!good) { n=first; continue; }                            /* not a header after all */
            at+=12+(uint64_t)entries*8-4;
        }
    }
    *count=n;
    return functions;
}

/* Thunk memory: within rel32 reach of the whole image. */
static unsigned char *near_image(size_t size) {
    const uint64_t base=(uint64_t)(uintptr_t)image, start=(base+image_size+(64u<<20)+0xffff)&~(uint64_t)0xffff;
    for (uint64_t at=start; at+size-base<UINT64_C(0x7ff00000); at+=1u<<16) {
        void *p=VirtualAlloc((void *)(uintptr_t)at,size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return NULL;
}
static unsigned char *thunk_reserve(void) {
    if (chunk_used+REGION_MAX>CHUNK) {
        unsigned char *next=near_image(CHUNK);
        if (!next) return NULL;
        chunk=next; chunk_used=0;
    }
    return chunk+chunk_used;
}

static int is_rsp(ZydisRegister r) {
    return r!=ZYDIS_REGISTER_NONE && ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,r)==ZYDIS_REGISTER_RSP;
}
static int rip_relative(const ZydisDecodedInstruction *in) {
    return (in->attributes&ZYDIS_ATTRIB_HAS_MODRM) && in->raw.modrm.mod==0 && in->raw.modrm.rm==5;
}
/* Moving an instruction into a thunk: no branches, RIP-relative operands are re-aimed. The
 * instructions cpu_patch.c rewrites stay in the image, where its handlers find them. */
static int movable(const Item *it) {
    const ZydisDecodedInstruction *in=&it->in;
    if (in->meta.branch_type!=ZYDIS_BRANCH_TYPE_NONE || in->raw.imm[0].is_relative || in->raw.imm[1].is_relative) return 0;
    switch (in->meta.category) {
    case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_CALL: case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_INTERRUPT: case ZYDIS_CATEGORY_SYSCALL: case ZYDIS_CATEGORY_SYSTEM: return 0;
    default: break;
    }
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_UD0: case ZYDIS_MNEMONIC_UD1: case ZYDIS_MNEMONIC_UD2: case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_LZCNT: case ZYDIS_MNEMONIC_TZCNT: case ZYDIS_MNEMONIC_ANDN:
    case ZYDIS_MNEMONIC_BEXTR: case ZYDIS_MNEMONIC_BLSI: case ZYDIS_MNEMONIC_BLSMSK: case ZYDIS_MNEMONIC_BLSR:
    case ZYDIS_MNEMONIC_MOVBE: return 0;
    default: break;
    }
    return !rip_relative(in) || in->raw.disp.size==32;
}
/* What the item does with memory and rsp; *red: it uses memory below rsp (or below its frame
 * under rbp: `frame` bytes pushed or reserved after mov rbp, rsp, -1 without a frame pointer). */
static void classify(Item *it, int64_t frame, int *red) {
    it->access=it->rsp=0;
    const int ignored=it->in.meta.category==ZYDIS_CATEGORY_NOP || it->in.meta.category==ZYDIS_CATEGORY_WIDENOP ||
                      it->in.meta.category==ZYDIS_CATEGORY_PREFETCH || it->in.meta.category==ZYDIS_CATEGORY_PREFETCHWT1;
    for (unsigned i=0;i<it->in.operand_count;++i) {
        const ZydisDecodedOperand *o=&it->ops[i];
        if (o->type==ZYDIS_OPERAND_TYPE_REGISTER) { if (is_rsp(o->reg.value)) it->rsp=1; continue; }
        if (o->type!=ZYDIS_OPERAND_TYPE_MEMORY) continue;
        const int64_t disp=o->mem.disp.value;
        if (is_rsp(o->mem.base) || is_rsp(o->mem.index)) {
            it->rsp=1;
            if (o->visibility==ZYDIS_OPERAND_VISIBILITY_EXPLICIT && disp<0) *red=1;
            continue;
        }
        if (o->visibility==ZYDIS_OPERAND_VISIBILITY_EXPLICIT && frame>=0 && o->mem.base==ZYDIS_REGISTER_RBP && -disp>frame) *red=1;
        if (o->mem.type!=ZYDIS_MEMOP_TYPE_MEM && o->mem.type!=ZYDIS_MEMOP_TYPE_VSIB) continue;
        if (ignored || o->mem.segment==ZYDIS_REGISTER_FS || o->mem.segment==ZYDIS_REGISTER_GS) continue;
        if (o->mem.base==ZYDIS_REGISTER_RIP) {
            const RedZoneSegment *data=segment_of(it->at+it->in.length+(uint64_t)disp);
            if (data && !(data->flags&2)) continue;                     /* read-only image data */
        }
        it->access=1;
    }
}

static void put(Code *c, const void *bytes, size_t n) { memcpy(c->p+c->n,bytes,n); c->n+=n; }
static int emit_item(Code *c, const Item *it) {
    const unsigned length=it->in.length;
    put(c,image+it->at,length);
    if (rip_relative(&it->in)) {
        int32_t disp;
        memcpy(&disp,c->p+c->n-length+it->in.raw.disp.offset,4);
        const int64_t rel=(int64_t)(uintptr_t)(image+it->at+length)+disp-(int64_t)(uintptr_t)(c->p+c->n);
        if (rel!=(int32_t)rel) return 0;
        disp=(int32_t)rel;
        memcpy(c->p+c->n-length+it->in.raw.disp.offset,&disp,4);
    }
    return 1;
}
/* The thunk for items[0..n): the accesses between lea rsp,[rsp-128] and lea rsp,[rsp+128],
 * the others as they were, then a jump back after the last. */
static unsigned char *emit_thunk(const Item *items, size_t n) {
    static const unsigned char lower[]={0x48,0x8d,0x64,0x24,0x80}, raise[]={0x48,0x8d,0xa4,0x24,0x80,0,0,0};
    unsigned char *t=thunk_reserve();
    if (!t) return NULL;
    Code c={t,0};
    int lowered=0;
    for (size_t i=0;i<n;++i) {
        if (items[i].access!=lowered) { if (lowered) put(&c,raise,sizeof(raise)); else put(&c,lower,sizeof(lower)); lowered=!lowered; }
        if (!emit_item(&c,&items[i])) return NULL;
    }
    if (lowered) put(&c,raise,sizeof(raise));
    const int64_t rel=(int64_t)(uintptr_t)(image+items[n-1].at+items[n-1].in.length)-(int64_t)(uintptr_t)(t+c.n+5);
    if (rel!=(int32_t)rel) return NULL;
    const unsigned char jmp=0xe9;
    const int32_t rel32=(int32_t)rel;
    put(&c,&jmp,1); put(&c,&rel32,4);
    if (region_count==region_capacity) {
        Region *more=realloc(regions,(region_capacity=region_capacity ? region_capacity*2 : 1024)*sizeof(*more));
        if (!more) return NULL;
        regions=more;
    }
    regions[region_count++]=(Region){(uint64_t)(uintptr_t)t,(uint64_t)(uintptr_t)(t+c.n),items[0].at,0};
    chunk_used+=(c.n+15)&~(size_t)15;
    return t;
}

typedef struct { unsigned functions, red, calls, indirect, lsda, undecodable, accesses, jumps, moved, left; } Stats;
/* One function: decode it, and if it keeps data below rsp, send its accesses to thunks. */
static void patch_function(const Function *f, const unsigned char *targets, Item **items, size_t *capacity, Stats *st) {
    size_t n=0;
    int red=0, calls=0, indirect=0;
    uint64_t undecoded=0;
    int64_t frame=-1;                                                    /* bytes below rbp, -1: no frame pointer */
    for (uint64_t at=f->start; at<f->end;) {
        if (n==*capacity) {
            Item *more=realloc(*items,(*capacity=*capacity ? *capacity*2 : 1024)*sizeof(*more));
            if (!more) { ++st->undecodable; return; }
            *items=more;
        }
        Item *it=&(*items)[n];
        it->at=at;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,image+at,(ZyanUSize)(f->end-at),&it->in,it->ops))) {
            undecoded=at;                                               /* data: a jump table after the code */
            break;
        }
        /* push rbp; mov rbp, rsp opens a frame; pushes and sub rsp, imm after it extend it. */
        if (it->in.mnemonic==ZYDIS_MNEMONIC_MOV && it->ops[0].type==ZYDIS_OPERAND_TYPE_REGISTER &&
            it->ops[0].reg.value==ZYDIS_REGISTER_RBP && it->ops[1].type==ZYDIS_OPERAND_TYPE_REGISTER &&
            it->ops[1].reg.value==ZYDIS_REGISTER_RSP) frame=0;
        else if (frame>=0 && it->in.mnemonic==ZYDIS_MNEMONIC_PUSH) frame+=8;
        else if (frame>=0 && it->in.mnemonic==ZYDIS_MNEMONIC_SUB && it->ops[0].type==ZYDIS_OPERAND_TYPE_REGISTER &&
                 it->ops[0].reg.value==ZYDIS_REGISTER_RSP && it->ops[1].type==ZYDIS_OPERAND_TYPE_IMMEDIATE)
            frame+=(int64_t)it->ops[1].imm.value.u;
        else if (frame>=0 && it->in.mnemonic!=ZYDIS_MNEMONIC_SUB && it->in.mnemonic!=ZYDIS_MNEMONIC_PUSH) {
            /* rsp moved some other way (and rsp, -32): the frame's size is not known. */
            for (unsigned i=0;i<it->in.operand_count_visible;++i)
                if (it->ops[i].type==ZYDIS_OPERAND_TYPE_REGISTER && it->ops[i].reg.value==ZYDIS_REGISTER_RSP &&
                    (it->ops[i].actions&ZYDIS_OPERAND_ACTION_MASK_WRITE)) frame=INT64_MAX;
        }
        classify(it,frame==INT64_MAX ? -1 : frame,&red);
        if (it->in.meta.category==ZYDIS_CATEGORY_CALL) calls=1;
        if ((it->in.meta.category==ZYDIS_CATEGORY_UNCOND_BR || it->in.meta.category==ZYDIS_CATEGORY_COND_BR) &&
            !it->in.raw.imm[0].is_relative) indirect=1;
        at+=it->in.length;
        ++n;
    }
    if (!red) return;
    ++st->red;
    if (f->lsda || calls || indirect || undecoded) {
        const char *why=f->lsda ? "exception table" : calls ? "calls" : indirect ? "indirect jump" : "data in its code";
        if (listing) printf("Red zone: function %#" PRIx64 "-%#" PRIx64 ": left alone (%s)\n",f->start,f->end,why);
        if (f->lsda) ++st->lsda; else if (calls) ++st->calls; else if (indirect) ++st->indirect; else ++st->undecodable;
        note_unprotected(f->start,f->end,why);
        return;
    }
    const unsigned accesses=st->accesses, left=st->left;
    Item *v=*items;
    uint64_t covered=0;
    for (size_t i=0;i<n;++i) {
        if (!v[i].access || v[i].at<covered) continue;
        if (v[i].rsp || !movable(&v[i])) {
            if (listing) printf("Red zone:   access at %#" PRIx64 " left (%s)\n",v[i].at,v[i].rsp ? "uses rsp" : "not movable");
            ++st->left;
            note_unprotected(v[i].at,v[i].at+v[i].in.length,v[i].rsp ? "an access that uses rsp" : "an access that cannot move");
            continue;
        }
        /* The access and the instructions after it, else the one or two before it. */
        size_t a=i, b=i+1;
        unsigned length=v[i].in.length;
        while (length<5 && b<n && b-a<REGION_ITEMS && movable(&v[b]) && !bit(targets,v[b].at)) length+=v[b++].in.length;
        /* An access right after comes along: its own region could start no earlier. */
        while (length>=5 && b<n && b-a<REGION_ITEMS && v[b].access && !v[b].rsp && movable(&v[b]) && !bit(targets,v[b].at))
            length+=v[b++].in.length;
        if (length<5) {
            b=i+1; length=v[i].in.length;
            if (!bit(targets,v[i].at) && i>0 && v[i-1].at>=covered && movable(&v[i-1])) {
                a=i-1; length+=v[a].in.length;
                if (length<5 && !bit(targets,v[a].at) && a>0 && v[a-1].at>=covered && movable(&v[a-1])) { --a; length+=v[a].in.length; }
            }
        }
        if (length<5) {
            if (listing) printf("Red zone:   access at %#" PRIx64 " left (no room for a jump)\n",v[i].at);
            ++st->left;
            note_unprotected(v[i].at,v[i].at+v[i].in.length,"an access without room for a jump");
            continue;
        }
        for (size_t k=a;k<b;++k) if (v[k].rsp) v[k].access=0;           /* moved, not under the lowered rsp */
        unsigned char *thunk=emit_thunk(v+a,b-a);
        if (!thunk) { ++st->left; note_unprotected(v[i].at,v[i].at+v[i].in.length,"an access without thunk memory"); continue; }
        unsigned char *site=image+v[a].at;
        const int32_t to=(int32_t)((int64_t)(uintptr_t)thunk-(int64_t)(uintptr_t)(site+5));
        site[0]=0xe9; memcpy(site+1,&to,4); memset(site+5,0xcc,length-5);
        for (size_t k=a;k<b;++k) st->accesses+=(unsigned)v[k].access;
        ++st->jumps;
        st->moved+=(unsigned)(b-a-1);
        covered=v[a].at+length;
        i=b-1;
    }
    if (listing) printf("Red zone: function %#" PRIx64 "-%#" PRIx64 ": %u accesses lowered, %u left\n",f->start,f->end,
                        st->accesses-accesses,st->left-left);
}

/* Marks the targets of direct branches in [start, end); `stop`: at the first byte that does not
 * decode, else skip it. A jump out of the image (another patch) may come back after the nops
 * or breakpoints that pad it. */
static void branch_targets(uint64_t start, uint64_t end, int stop, unsigned char *targets) {
    ZydisDecodedInstruction in;
    ZydisDecoderContext context;
    for (uint64_t at=start; at<end;) {
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder,&context,image+at,(ZyanUSize)(end-at),&in))) {
            if (stop) return;
            ++at;
            continue;
        }
        if (in.meta.branch_type!=ZYDIS_BRANCH_TYPE_NONE && in.raw.imm[0].is_relative) {
            const uint64_t target=at+in.length+(uint64_t)in.raw.imm[0].value.s;
            if (target<image_size) mark(targets,target);
            else if (in.mnemonic==ZYDIS_MNEMONIC_JMP) {
                uint64_t after=at+in.length;
                while (after<end && (image[after]==0x90 || image[after]==0xcc)) ++after;
                mark(targets,after);
            }
        }
        at+=in.length;
    }
}

static int by_start(const void *a, const void *b) {
    const uint64_t x=((const Unprotected *)a)->start, y=((const Unprotected *)b)->start;
    return x<y ? -1 : x>y;
}
static int by_thunk(const void *a, const void *b) {
    const uint64_t x=((const Region *)a)->thunk, y=((const Region *)b)->thunk;
    return x<y ? -1 : x>y;
}
void red_zone_patch(unsigned char *base, uint64_t size, const RedZoneSegment *segs, size_t count) {
    const char *mode=getenv("BB_RED_ZONE");
    if (mode && !strcmp(mode,"0")) { printf("Red zone: BB_RED_ZONE=0, guest functions left as they are\n"); return; }
    listing=mode && !strcmp(mode,"list");
    image=base; image_size=size; segments=segs; segment_count=count;
    ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
    size_t function_count=0;
    Function *functions=find_functions(&function_count);
    unsigned char *targets=calloc(size/8+1,1);
    if (!functions || !targets) { printf("Red zone: no function table found, guest functions left as they are\n"); free(functions); free(targets); return; }
    /* Direct branch targets: a sweep over each executable segment, and one from each function's
     * start (data after a function, such as a jump table, can put the first out of step). */
    for (size_t s=0;s<count;++s)
        if (segs[s].flags&1) branch_targets(segs[s].address,segs[s].address+segs[s].size,0,targets);
    for (size_t i=0;i<function_count;++i) branch_targets(functions[i].start,functions[i].end,1,targets);
    Stats st={0};
    st.functions=(unsigned)function_count;
    Item *items=NULL;
    size_t capacity=0;
    for (size_t i=0;i<function_count;++i) patch_function(&functions[i],targets,&items,&capacity,&st);
    qsort(regions,region_count,sizeof(*regions),by_thunk);
    qsort(unprotected,unprotected_count,sizeof(*unprotected),by_start);
    printf("Red zone: %u of %u guest functions keep data below rsp; %u memory accesses in them run with rsp "
           "128 bytes lower (%u jumps, %u moved neighbours), %u left as they were; skipped: %u with calls, "
           "%u with indirect jumps, %u with exception tables, %u with data in their code\n",
           st.red,st.functions,st.accesses,st.jumps,st.moved,st.left,st.calls,st.indirect,st.lsda,st.undecodable);
    free(items);
    free(targets);
    free(functions);
    segments=NULL; segment_count=0;                                      /* the caller's */
}

static size_t region_of(uint64_t address) {
    size_t low=0, high=region_count;
    while (low<high) {
        const size_t middle=(low+high)/2;
        if (regions[middle].end<=address) low=middle+1; else high=middle;
    }
    return low<region_count && address>=regions[low].thunk ? low : SIZE_MAX;
}
int red_zone_site(uint64_t address, uint64_t *site) {
    const size_t r=region_of(address);
    if (r==SIZE_MAX) return 0;
    *site=regions[r].site;
    return 1;
}
/* A fault in a thunk: its data was kept (each thunk's first, then counts). A fault at an access
 * left as it was, or in a function left alone: its data may be lost. */
void red_zone_fault_handled(uint64_t rip) {
    const size_t r=region_of(rip);
    if (r!=SIZE_MAX) {
        const uint64_t n=__atomic_add_fetch(&faults_kept,1,__ATOMIC_RELAXED);
        if (!__atomic_exchange_n(&regions[r].seen,1,__ATOMIC_RELAXED) || n==1000 || n==10000 || n==100000)
            printf("Red zone: guest page fault %" PRIu64 " inside a function that keeps data below rsp (guest offset "
                   "%#" PRIx64 "); the data was kept\n",n,regions[r].site);
        return;
    }
    const uint64_t offset=rip-(uint64_t)(uintptr_t)image;
    if (!image || offset>=image_size) return;
    size_t low=0, high=unprotected_count;
    while (low<high) {
        const size_t middle=(low+high)/2;
        if (unprotected[middle].end<=offset) low=middle+1; else high=middle;
    }
    if (low==unprotected_count || offset<unprotected[low].start) return;
    const uint64_t n=__atomic_add_fetch(&unprotected_faults,1,__ATOMIC_RELAXED);
    if (n<=50 || n==100 || n==1000 || n==10000)
        printf("Red zone: guest page fault %" PRIu64 " at guest offset %#" PRIx64 ", in a function that keeps data "
               "below rsp, at %s (not rewritten): its data may be lost\n",n,offset,unprotected[low].why);
}
