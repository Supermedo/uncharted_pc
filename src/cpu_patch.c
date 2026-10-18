/* Instructions the PS4's CPU (AMD Jaguar) has and older PC CPUs lack: BMI1 (ANDN, BEXTR, BLSI,
 * BLSMSK, BLSR, TZCNT), LZCNT and MOVBE, all missing before Intel Haswell (Ivy Bridge and Sandy
 * Bridge Core i and Xeon E3/E5 v1/v2). Guest code runs natively, so there the first BMI1
 * instruction stops the game with an illegal instruction (0xc000001d), and LZCNT runs as BSR
 * (another result, no fault). Before the guest starts, the executable segments are disassembled
 * (Zydis, linear sweep; clang keeps jump tables out of code) and each such instruction jumps to
 * a thunk near the image that computes the same result and the same defined flags with older
 * instructions, then jumps back. A jump takes 5 bytes: a shorter instruction (lzcnt eax, ecx is
 * 4) moves its neighbours into the thunk when no direct branch lands between them, else it
 * becomes a breakpoint that the exception handler sends to the thunk. An illegal instruction the
 * sweep missed runs from a thunk too (cpu_patch_illegal). Nothing changes on a CPU with them. */
#define _GNU_SOURCE
#include "cpu_patch.h"
#include <Zydis/Zydis.h>
#include <cpuid.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

enum { NEED_BMI1=1, NEED_LZCNT=2, NEED_MOVBE=4 };
enum { LZCNT, TZCNT, ANDN, BLSR, BLSMSK, BLSI, BEXTR, MOVBE_LOAD, MOVBE_STORE, KINDS };
static const char *const kind_names[KINDS]={"LZCNT","TZCNT","ANDN","BLSR","BLSMSK","BLSI","BEXTR","MOVBE","MOVBE"};
enum { CF=0x1, ZF=0x40, SF=0x80, OF=0x800 };
/* Bytes a thunk may take per instruction (the C call is the largest), and per region. */
enum { ITEM_MAX=256, REGION_ITEMS=4, CHUNK=1<<20 };

static unsigned char *image;
static uint64_t image_size;
static unsigned need;
static ZydisDecoder decoder;
static unsigned char *chunk;
static size_t chunk_used=CHUNK;
typedef struct { uint64_t address, thunk; } Breakpoint;
static Breakpoint *breakpoints, *missed;
static size_t breakpoint_count, missed_count;
static pthread_mutex_t missed_lock=PTHREAD_MUTEX_INITIALIZER;

static unsigned host_missing(void) {
    unsigned a,b,c,d,missing=0;
    if (!__get_cpuid_count(7,0,&a,&b,&c,&d) || !(b&(1u<<3))) missing|=NEED_BMI1;
    if (!__get_cpuid(0x80000001,&a,&b,&c,&d) || !(c&(1u<<5))) missing|=NEED_LZCNT;
    if (!__get_cpuid(1,&a,&b,&c,&d) || !(c&(1u<<22))) missing|=NEED_MOVBE;
    return missing;
}
/* The rest of Jaguar's instruction set has no rewrite: name what is missing. */
static void report_unsupported(void) {
    static const struct { unsigned bit; const char *name; } leaf1[]={
        {0,"SSE3"},{1,"PCLMULQDQ"},{9,"SSSE3"},{19,"SSE4.1"},{20,"SSE4.2"},{23,"POPCNT"},{25,"AES"},
        {26,"XSAVE"},{28,"AVX"},{29,"F16C"}};
    unsigned a,b,c,d;
    char list[160]="";
    if (!__get_cpuid(1,&a,&b,&c,&d)) return;
    for (size_t i=0;i<sizeof(leaf1)/sizeof(*leaf1);++i)
        if (!(c&(1u<<leaf1[i].bit))) { strcat(list," "); strcat(list,leaf1[i].name); }
    if (*list) printf("CPU: this processor lacks%s, which the PS4 has; the game will likely stop with an illegal instruction\n",list);
}
static int wanted(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_ANDN: case ZYDIS_MNEMONIC_BEXTR: case ZYDIS_MNEMONIC_BLSI: case ZYDIS_MNEMONIC_BLSMSK:
    case ZYDIS_MNEMONIC_BLSR: case ZYDIS_MNEMONIC_TZCNT: return need&NEED_BMI1;
    case ZYDIS_MNEMONIC_LZCNT: return need&NEED_LZCNT;
    case ZYDIS_MNEMONIC_MOVBE: return need&NEED_MOVBE;
    default: return 0;
    }
}

/* Thunk memory: within rel32 reach of the whole image. */
static unsigned char *near_image(size_t size) {
    const uint64_t base=(uint64_t)(uintptr_t)image, start=(base+image_size+(48u<<20)+0xffff)&~(uint64_t)0xffff;
    for (uint64_t at=start; at+size-base<UINT64_C(0x7ff00000); at+=1u<<16) {
#ifdef _WIN32
        void *p=VirtualAlloc((void *)(uintptr_t)at,size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (p) return p;
#else
        void *p=mmap((void *)(uintptr_t)at,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if (p==(void *)(uintptr_t)at) return p;
        if (p!=MAP_FAILED) munmap(p,size);
#endif
    }
    return NULL;
}
/* Room for one region's thunk; thunk_commit keeps what it used. */
static unsigned char *thunk_reserve(void) {
    if (chunk_used+REGION_ITEMS*ITEM_MAX+16>CHUNK) {
        unsigned char *next=near_image(CHUNK);
        if (!next) return NULL;
        chunk=next; chunk_used=0;
    }
    return chunk+chunk_used;
}
static void thunk_commit(size_t used) { chunk_used+=(used+15)&~(size_t)15; }

/* x86-64 encoding. Registers are numbered as in ModRM (rax 0 ... r15 15). */
typedef struct { unsigned char *p; size_t n; } Code;
static void b(Code *c, unsigned v) { c->p[c->n++]=(unsigned char)v; }
static void d32(Code *c, uint32_t v) { memcpy(c->p+c->n,&v,4); c->n+=4; }
static void d64(Code *c, uint64_t v) { memcpy(c->p+c->n,&v,8); c->n+=8; }
static void prefixes(Code *c, int w, int r, int x, int base) {
    if (w==16) b(c,0x66);
    const unsigned rex=0x40u|(w==64 ? 8u : 0u)|(unsigned)(r>>3&1)<<2|(unsigned)(x>>3&1)<<1|(unsigned)(base>>3&1);
    if (rex!=0x40) b(c,rex);
}
/* op with ModRM register form; `reg` is a register or an opcode extension. */
static void rr(Code *c, int w, const char *op, int reg, int rm) {
    prefixes(c,w,reg,0,rm);
    for (; *op; ++op) b(c,(unsigned char)*op);
    b(c,0xc0u|(unsigned)(reg&7)<<3|(unsigned)(rm&7));
}
/* op with a memory operand; `adjust` is added to an rsp base (pushes before it), `imm` is the
 * number of immediate bytes that follow (RIP-relative displacements count from the end). */
static int rm(Code *c, int w, const char *op, int reg, const CpuPatchMem *m, int64_t adjust, int imm) {
    const int base=m->base, index=m->index;
    const int64_t disp=m->disp+(base==4 ? adjust : 0);
    prefixes(c,w,reg,index<0 ? 0 : index,base<0 ? 0 : base);
    for (; *op; ++op) b(c,(unsigned char)*op);
    if (m->rip) {
        b(c,(unsigned)(reg&7)<<3|5u);
        const int64_t rel=(int64_t)m->absolute-(int64_t)(uintptr_t)(c->p+c->n+4+imm);
        if (rel!=(int32_t)rel) return 0;
        d32(c,(uint32_t)(int32_t)rel);
        return 1;
    }
    if (disp!=(int32_t)disp) return 0;
    const int sib=index>=0 || base<0 || (base&7)==4;
    const int mod=base<0 ? 0 : disp==0 && (base&7)!=5 ? 0 : disp==(int8_t)disp ? 1 : 2;
    b(c,(unsigned)mod<<6|(unsigned)(reg&7)<<3|(unsigned)(sib ? 4 : base&7));
    if (sib) {
        const unsigned scale=m->scale==8 ? 3 : m->scale==4 ? 2 : m->scale==2 ? 1 : 0;
        b(c,scale<<6|(unsigned)((index>=0 ? index : 4)&7)<<3|(unsigned)(base<0 ? 5 : base&7));
    }
    if (mod==1) b(c,(unsigned)(disp&0xff));
    else if (mod==2 || base<0) d32(c,(uint32_t)(int32_t)disp);
    return 1;
}
static size_t jump8(Code *c, unsigned op) { b(c,op); b(c,0); return c->n-1; }
static void land(Code *c, size_t at) { c->p[at]=(unsigned char)(int8_t)(c->n-at-1); }
static void mov_imm(Code *c, int w, int r, int64_t v) {
    if (w==64 && v<0) { prefixes(c,64,0,0,r); b(c,0xc7); b(c,0xc0u|(unsigned)(r&7)); d32(c,(uint32_t)v); return; }
    prefixes(c,w==16 ? 16 : 32,0,0,r);
    b(c,0xb8u|(unsigned)(r&7));
    if (w==16) { b(c,(unsigned)(v&0xff)); b(c,(unsigned)(v>>8&0xff)); }
    else d32(c,(uint32_t)v);
}
static void push(Code *c, int r) { if (r>=8) b(c,0x41); b(c,0x50u+(unsigned)(r&7)); }
static void pop(Code *c, int r) { if (r>=8) b(c,0x41); b(c,0x58u+(unsigned)(r&7)); }
static void bswap(Code *c, int w, int r) { prefixes(c,w,0,0,r); b(c,0x0f); b(c,0xc8u+(unsigned)(r&7)); }
/* A scratch register outside the red zone: lea rsp, [rsp-128]; push t ... pop t; lea rsp, [rsp+128]
 * (neither touches the flags). Memory operands based on rsp move by SAVED. */
enum { SAVED=136 };
static void save(Code *c, int t) { static const unsigned char lea[]={0x48,0x8d,0x64,0x24,0x80}; memcpy(c->p+c->n,lea,5); c->n+=5; push(c,t); }
static void restore(Code *c, int t) {
    static const unsigned char lea[]={0x48,0x8d,0xa4,0x24,0x80,0,0,0};
    pop(c,t); memcpy(c->p+c->n,lea,8); c->n+=8;
}
static int scratch(const CpuPatchOp *op) {
    unsigned used=1u<<4;
    for (int i=0;i<3;++i) if (op->reg[i]>=0) used|=1u<<op->reg[i];
    if (op->mem.base>=0) used|=1u<<op->mem.base;
    if (op->mem.index>=0) used|=1u<<op->mem.index;
    for (int r=0;r<16;++r) if (!(used>>r&1)) return r;
    return -1;
}

/* LZCNT/TZCNT: BSR/BSF (ZF when the source is 0, the destination kept), then the count and its
 * flags (ZF for a zero result, CF for a zero source). */
static int gen_count(Code *c, const CpuPatchOp *op) {
    const int w=op->width, d=op->reg[0], s=op->reg[1];
    const char *scan=op->kind==LZCNT ? "\x0f\xbd" : "\x0f\xbc";
    if (s>=0) rr(c,w,scan,d,s);
    else if (!rm(c,w,scan,d,&op->mem,0,0)) return 0;
    const size_t zero=jump8(c,0x74);
    if (op->kind==LZCNT) { rr(c,w,"\x83",6,d); b(c,(unsigned)(w-1)); }  /* xor d, w-1 */
    else rr(c,w,"\x85",d,d);                                            /* test d, d */
    const size_t done=jump8(c,0xeb);
    land(c,zero);
    mov_imm(c,w,d,w);
    rr(c,w,"\x83",7,d); b(c,(unsigned)(w+1));                           /* cmp d, w+1: CF, not ZF */
    land(c,done);
    return 1;
}
/* ANDN d, s1, s2: d = ~s1 & s2; SF, ZF from the result, CF and OF clear (AND's flags). */
static int gen_andn(Code *c, const CpuPatchOp *op) {
    const int w=op->width, d=op->reg[0], s1=op->reg[1];
    int s2=op->reg[2], t=-1;
    if (s2<0) {
        t=scratch(op);
        save(c,t);
        if (!rm(c,w,"\x8b",t,&op->mem,SAVED,0)) return 0;                 /* mov t, [m] */
        s2=t;
    }
    if (d==s1 && d==s2) rr(c,32,"\x31",d,d);                            /* xor d, d */
    else if (d==s2) {                                                   /* not s1; and d, s1; not s1 */
        rr(c,64,"\xf7",2,s1); rr(c,w,"\x21",s1,d); rr(c,64,"\xf7",2,s1);
    } else {                                                            /* mov d, s1; not d; and d, s2 */
        if (d!=s1) rr(c,w,"\x89",s1,d);
        rr(c,w,"\xf7",2,d); rr(c,w,"\x21",s2,d);
    }
    if (t>=0) restore(c,t);
    return 1;
}
/* BLSR (s & (s-1)), BLSMSK (s ^ (s-1)), BLSI (s & -s); CF: BLSR, BLSMSK for a zero source, BLSI
 * for a nonzero one; SF and ZF from the result (BLSMSK's ZF is always clear); OF clear. */
static int gen_bls(Code *c, const CpuPatchOp *op) {
    const int w=op->width, d=op->reg[0];
    int s=op->reg[1], t=-1;
    if (s<0 || s==d) {
        t=scratch(op);
        save(c,t);
        if (s<0) { if (!rm(c,w,"\x8b",t,&op->mem,SAVED,0)) return 0; }
        else rr(c,64,"\x89",s,t);
        s=t;
    }
    const CpuPatchMem minus_one={s,-1,1,0,-1,0};
    rr(c,w,"\x85",s,s);                                                 /* test s, s */
    const size_t zero=jump8(c,0x74);
    if (op->kind==BLSI) { rr(c,w,"\x89",s,d); rr(c,w,"\xf7",3,d); rr(c,w,"\x21",s,d); b(c,0xf9); }
    else {
        rm(c,w,"\x8d",d,&minus_one,0,0);                                /* lea d, [s-1] */
        rr(c,w,op->kind==BLSR ? "\x21" : "\x31",s,d);
    }
    const size_t done=jump8(c,0xeb);
    land(c,zero);
    if (op->kind==BLSMSK) { mov_imm(c,w,d,-1); rr(c,w,"\x85",d,d); b(c,0xf9); }
    else { rr(c,32,"\x31",d,d); if (op->kind==BLSR) b(c,0xf9); }
    land(c,done);
    if (t>=0) restore(c,t);
    return 1;
}
/* MOVBE: mov and bswap, no flags. */
static int gen_movbe(Code *c, const CpuPatchOp *op) {
    const int w=op->width;
    if (op->kind==MOVBE_LOAD) {
        if (!rm(c,w,"\x8b",op->reg[0],&op->mem,0,0)) return 0;
        bswap(c,w,op->reg[0]);
        return 1;
    }
    const int t=scratch(op);
    save(c,t);
    rr(c,64,"\x89",op->reg[1],t);
    bswap(c,w,t);
    if (!rm(c,w,"\x89",t,&op->mem,SAVED,0)) return 0;
    restore(c,t);
    return 1;
}

/* BEXTR and 16-bit MOVBE run in C: the thunk saves the registers and flags as {rax ... r15,
 * rflags} (the rsp slot unused), xmm0-5 and calls this with the Microsoft x64 convention. */
static uint64_t load(uint64_t address, int w) {
    uint64_t v=0;
    memcpy(&v,(const void *)(uintptr_t)address,(size_t)w/8);
    return v;
}
static uint64_t swap(uint64_t v, int w) {
    return w==16 ? (uint64_t)(uint16_t)(v<<8|(v>>8&0xff)) : w==32 ? __builtin_bswap32((uint32_t)v) : __builtin_bswap64(v);
}
static void __attribute__((ms_abi)) emulate(uint64_t *regs, const CpuPatchOp *op) {
    const uint64_t rsp=(uint64_t)(uintptr_t)regs+17*8+128;
    const int w=op->width;
    const uint64_t mask=w==64 ? ~UINT64_C(0) : (UINT64_C(1)<<w)-1;
    const CpuPatchMem *m=&op->mem;
    uint64_t address=m->rip ? m->absolute : (uint64_t)m->disp;
    if (!m->rip && m->base>=0) address+=m->base==4 ? rsp : regs[m->base];
    if (!m->rip && m->index>=0) address+=regs[m->index]*(uint64_t)m->scale;
    if (op->kind==BEXTR) {
        const uint64_t source=(op->reg[1]>=0 ? regs[op->reg[1]] : load(address,w))&mask, control=regs[op->reg[2]];
        const unsigned start=(unsigned)(control&0xff), length=(unsigned)(control>>8&0xff);
        uint64_t r=start>=(unsigned)w ? 0 : source>>start;
        if (length<(unsigned)w) r&=(UINT64_C(1)<<length)-1;
        regs[op->reg[0]]=r;
        regs[16]=(regs[16]&~(uint64_t)(CF|ZF|OF))|(r ? 0 : ZF);
    } else if (op->kind==MOVBE_LOAD) {
        const uint64_t v=swap(load(address,w),w);
        regs[op->reg[0]]=w==16 ? (regs[op->reg[0]]&~UINT64_C(0xffff))|v : v;
    } else {
        const uint64_t v=swap(regs[op->reg[1]]&mask,w);
        memcpy((void *)(uintptr_t)address,&v,(size_t)w/8);
    }
}
static int gen_call(Code *c, const CpuPatchOp *op) {
    CpuPatchOp *copy=malloc(sizeof(*copy));
    if (!copy) return 0;
    *copy=*op;
    static const unsigned char enter[]={0x48,0x8d,0x64,0x24,0x80,0x9c,                 /* lea rsp, [rsp-128]; pushfq */
        0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x41,0x53,0x41,0x52,0x41,0x51,0x41,0x50, /* push r15 ... r8 */
        0x57,0x56,0x55,0x54,0x53,0x52,0x51,0x50,                                       /* push rdi ... rax */
        0x48,0x89,0xe3,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec,0x80,0,0,0};                 /* mov rbx, rsp; and rsp, -16; sub rsp, 0x80 */
    static const unsigned char leave[]={0x48,0x89,0xdc,0x58,0x59,0x5a,0x5b,            /* mov rsp, rbx; pop rax rcx rdx rbx */
        0x48,0x8d,0x64,0x24,0x08,0x5d,0x5e,0x5f,                                       /* lea rsp, [rsp+8]; pop rbp rsi rdi */
        0x41,0x58,0x41,0x59,0x41,0x5a,0x41,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f, /* pop r8 ... r15 */
        0x9d,0x48,0x8d,0xa4,0x24,0x80,0,0,0};                                          /* popfq; lea rsp, [rsp+128] */
    memcpy(c->p+c->n,enter,sizeof(enter)); c->n+=sizeof(enter);
    for (unsigned x=0;x<6;++x) { b(c,0xf3); b(c,0x0f); b(c,0x7f); b(c,0x84|x<<3); b(c,0x24); d32(c,0x20+16*x); }
    b(c,0x48); b(c,0x89); b(c,0xd9);                                    /* mov rcx, rbx */
    b(c,0x48); b(c,0xba); d64(c,(uint64_t)(uintptr_t)copy);             /* movabs rdx, op */
    b(c,0x48); b(c,0xb8); d64(c,(uint64_t)(uintptr_t)emulate);          /* movabs rax, emulate */
    b(c,0xff); b(c,0xd0);                                               /* call rax */
    for (unsigned x=0;x<6;++x) { b(c,0xf3); b(c,0x0f); b(c,0x6f); b(c,0x84|x<<3); b(c,0x24); d32(c,0x20+16*x); }
    memcpy(c->p+c->n,leave,sizeof(leave)); c->n+=sizeof(leave);
    return 1;
}

/* Decoded instructions and what a rewrite needs from them. */
typedef struct { uint64_t at; ZydisDecodedInstruction in; ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]; int rewrite; } Item;
static int gpr(const ZydisDecodedOperand *o) {
    const ZydisRegisterClass k=ZydisRegisterGetClass(o->reg.value);
    if (k!=ZYDIS_REGCLASS_GPR16 && k!=ZYDIS_REGCLASS_GPR32 && k!=ZYDIS_REGCLASS_GPR64) return -1;
    const int id=ZydisRegisterGetId(o->reg.value);
    return id==4 ? -1 : id;                                             /* rsp operands: left alone */
}
static int memory(const ZydisDecodedOperand *o, uint64_t next, CpuPatchMem *m) {
    if (o->mem.type!=ZYDIS_MEMOP_TYPE_MEM || o->mem.segment==ZYDIS_REGISTER_FS || o->mem.segment==ZYDIS_REGISTER_GS) return 0;
    m->rip=o->mem.base==ZYDIS_REGISTER_RIP;
    m->base=m->index=-1;
    if (!m->rip && o->mem.base!=ZYDIS_REGISTER_NONE) {
        if (ZydisRegisterGetClass(o->mem.base)!=ZYDIS_REGCLASS_GPR64) return 0;
        m->base=ZydisRegisterGetId(o->mem.base);
    }
    if (o->mem.index!=ZYDIS_REGISTER_NONE) {
        if (m->rip || ZydisRegisterGetClass(o->mem.index)!=ZYDIS_REGCLASS_GPR64) return 0;
        m->index=ZydisRegisterGetId(o->mem.index);
    }
    m->scale=o->mem.scale ? o->mem.scale : 1;
    m->disp=o->mem.disp.value;
    m->absolute=m->rip ? next+(uint64_t)m->disp : 0;
    return 1;
}
static int describe(const Item *it, CpuPatchOp *op) {
    static const struct { ZydisMnemonic mnemonic; int kind, operands; } table[]={
        {ZYDIS_MNEMONIC_LZCNT,LZCNT,2},{ZYDIS_MNEMONIC_TZCNT,TZCNT,2},{ZYDIS_MNEMONIC_ANDN,ANDN,3},
        {ZYDIS_MNEMONIC_BLSR,BLSR,2},{ZYDIS_MNEMONIC_BLSMSK,BLSMSK,2},{ZYDIS_MNEMONIC_BLSI,BLSI,2},
        {ZYDIS_MNEMONIC_BEXTR,BEXTR,3},{ZYDIS_MNEMONIC_MOVBE,MOVBE_LOAD,2}};
    memset(op,0,sizeof(*op));
    op->kind=-1;
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (table[i].mnemonic==it->in.mnemonic && table[i].operands==it->in.operand_count_visible) op->kind=table[i].kind;
    op->width=it->in.operand_width;
    if (op->kind<0 || (op->width!=32 && op->width!=64 && !(op->width==16 && (op->kind<=TZCNT || op->kind==MOVBE_LOAD)))) return 0;
    int memory_at=-1;
    for (int i=0;i<3;++i) {
        op->reg[i]=-1;
        if (i>=it->in.operand_count_visible) continue;
        const ZydisDecodedOperand *o=&it->ops[i];
        if (o->type==ZYDIS_OPERAND_TYPE_REGISTER) { if ((op->reg[i]=gpr(o))<0) return 0; }
        else if (o->type==ZYDIS_OPERAND_TYPE_MEMORY && memory_at<0 && memory(o,it->at+it->in.length,&op->mem)) memory_at=i;
        else return 0;
    }
    if (memory_at<0) op->mem.base=op->mem.index=-1;
    if (op->kind==MOVBE_LOAD && memory_at==0) op->kind=MOVBE_STORE;
    return op->reg[0]>=0 || op->kind==MOVBE_STORE;
}
/* Moving an instruction into a thunk: no branches, RIP-relative operands are re-aimed. */
static int movable(const ZydisDecodedInstruction *in) {
    if (in->meta.branch_type!=ZYDIS_BRANCH_TYPE_NONE || in->raw.imm[0].is_relative || in->raw.imm[1].is_relative) return 0;
    switch (in->meta.category) {
    case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_CALL: case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_INTERRUPT: case ZYDIS_CATEGORY_SYSCALL: case ZYDIS_CATEGORY_SYSTEM: return 0;
    default: break;
    }
    return in->mnemonic!=ZYDIS_MNEMONIC_UD2 && in->mnemonic!=ZYDIS_MNEMONIC_UD0 && in->mnemonic!=ZYDIS_MNEMONIC_UD1 &&
           in->mnemonic!=ZYDIS_MNEMONIC_HLT && in->mnemonic!=ZYDIS_MNEMONIC_INT3;
}
static int rip_relative(const ZydisDecodedInstruction *in) {
    return (in->attributes&ZYDIS_ATTRIB_HAS_MODRM) && in->raw.modrm.mod==0 && in->raw.modrm.rm==5;
}
static int decode(uint64_t offset, uint64_t end, Item *it) {
    it->at=(uint64_t)(uintptr_t)(image+offset);
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,image+offset,(ZyanUSize)(end-offset),&it->in,it->ops))) return 0;
    it->rewrite=wanted(it->in.mnemonic);
    if (it->rewrite) { CpuPatchOp op; return describe(it,&op); }
    return movable(&it->in) && (!rip_relative(&it->in) || it->in.raw.disp.size==32);
}
static unsigned rewritten[KINDS];
static int emit_item(Code *c, const Item *it) {
    if (!it->rewrite) {
        const unsigned length=it->in.length;
        memcpy(c->p+c->n,(const void *)(uintptr_t)it->at,length);
        if (rip_relative(&it->in)) {
            int32_t disp;
            memcpy(&disp,c->p+c->n+it->in.raw.disp.offset,4);
            const int64_t rel=(int64_t)it->at+length+disp-(int64_t)(uintptr_t)(c->p+c->n+length);
            if (rel!=(int32_t)rel) return 0;
            disp=(int32_t)rel;
            memcpy(c->p+c->n+it->in.raw.disp.offset,&disp,4);
        }
        c->n+=length;
        return 1;
    }
    CpuPatchOp op;
    if (!describe(it,&op)) return 0;
    int ok;
    switch (op.kind) {
    case LZCNT: case TZCNT: ok=gen_count(c,&op); break;
    case ANDN: ok=gen_andn(c,&op); break;
    case BLSR: case BLSMSK: case BLSI: ok=gen_bls(c,&op); break;
    case MOVBE_LOAD: case MOVBE_STORE: ok=op.width==16 ? gen_call(c,&op) : gen_movbe(c,&op); break;
    default: ok=gen_call(c,&op); break;
    }
    if (ok) ++rewritten[op.kind];
    return ok;
}
/* The thunk for items[0..n): their code, then a jump to the end of the last. Thunk addresses of
 * the items go to starts[]. NULL when something cannot be encoded. */
static unsigned char *emit_thunk(const Item *items, size_t n, uint64_t *starts) {
    unsigned char *t=thunk_reserve();
    if (!t) return NULL;
    Code c={t,0};
    unsigned saved[KINDS];
    memcpy(saved,rewritten,sizeof(saved));
    for (size_t i=0;i<n;++i) {
        starts[i]=(uint64_t)(uintptr_t)(t+c.n);
        if (!emit_item(&c,&items[i])) { memcpy(rewritten,saved,sizeof(saved)); return NULL; }
    }
    const uint64_t back=items[n-1].at+items[n-1].in.length;
    const int64_t rel=(int64_t)back-(int64_t)(uintptr_t)(t+c.n+5);
    if (rel!=(int32_t)rel) { memcpy(rewritten,saved,sizeof(saved)); return NULL; }
    b(&c,0xe9); d32(&c,(uint32_t)(int32_t)rel);
    thunk_commit(c.n);
    return t;
}

static int add_breakpoint(uint64_t address, uint64_t thunk) {
    static size_t capacity;
    if (breakpoint_count==capacity) {
        Breakpoint *more=realloc(breakpoints,(capacity=capacity ? capacity*2 : 256)*sizeof(*more));
        if (!more) return 0;
        breakpoints=more;
    }
    breakpoints[breakpoint_count++]=(Breakpoint){address,thunk};
    return 1;
}
static int by_address(const void *a, const void *b) {
    const uint64_t x=((const Breakpoint *)a)->address, y=((const Breakpoint *)b)->address;
    return x<y ? -1 : x>y;
}
static int bit(const unsigned char *bits, uint64_t offset) { return bits[offset>>3]>>(offset&7)&1; }

typedef struct { uint64_t offset, previous[2]; } Site;
void cpu_patch_image(unsigned char *base, uint64_t size, const uint64_t (*ranges)[2], size_t count) {
    const char *mode=getenv("BB_CPU_PATCH");
    if (mode && !strcmp(mode,"0")) return;
    const unsigned missing=host_missing();
    need=mode && !strcmp(mode,"all") ? NEED_BMI1|NEED_LZCNT|NEED_MOVBE : missing;
    if (mode && !strcmp(mode,"all")) printf("CPU: BB_CPU_PATCH=all: rewriting BMI1, LZCNT and MOVBE instructions\n");
    report_unsupported();
    image=base; image_size=size;
    ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
    if (!need) return;
    /* Pass 1: direct branch targets and the instructions to rewrite (with the two before them). */
    unsigned char *targets=calloc(size/8+1,1);
    Site *sites=NULL;
    size_t site_count=0, site_capacity=0;
    if (!targets) { printf("CPU: out of memory, instructions not rewritten\n"); return; }
    for (size_t r=0;r<count;++r) {
        const uint64_t end=ranges[r][0]+ranges[r][1];
        uint64_t previous[2]={UINT64_MAX,UINT64_MAX};
        ZydisDecodedInstruction in;
        ZydisDecoderContext context;
        for (uint64_t at=ranges[r][0]; at<end;) {
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder,&context,image+at,(ZyanUSize)(end-at),&in))) {
                previous[0]=previous[1]=UINT64_MAX; ++at; continue;
            }
            if (in.meta.branch_type!=ZYDIS_BRANCH_TYPE_NONE && in.raw.imm[0].is_relative) {
                const uint64_t target=at+in.length+(uint64_t)in.raw.imm[0].value.s;
                if (target<size) targets[target>>3]|=(unsigned char)(1u<<(target&7));
            }
            if (wanted(in.mnemonic)) {
                if (site_count==site_capacity) {
                    Site *more=realloc(sites,(site_capacity=site_capacity ? site_capacity*2 : 1024)*sizeof(*more));
                    if (!more) break;
                    sites=more;
                }
                sites[site_count++]=(Site){at,{previous[0],previous[1]}};
            }
            previous[1]=previous[0]; previous[0]=at;
            at+=in.length;
        }
    }
    /* Pass 2: a jump to a thunk over 5+ bytes (the instruction, else with its neighbours), else
     * a breakpoint. Sites are in segment order; `covered` ends the last rewritten region. */
    unsigned jumps=0, moved=0, traps=0, left=0;
    uint64_t covered=0;
    size_t r=0;
    for (size_t i=0;i<site_count;++i) {
        const uint64_t o=sites[i].offset;
        while (r<count && !(o>=ranges[r][0] && o<ranges[r][0]+ranges[r][1])) r=(r+1)%count;
        if (i && sites[i-1].offset>o) covered=0;                         /* next segment */
        if (o<covered) continue;
        const uint64_t start=ranges[r][0], end=start+ranges[r][1];
        Item items[REGION_ITEMS];
        size_t n=1;
        if (!decode(o,end,&items[0])) { ++left; continue; }
        unsigned length=items[0].in.length;
        uint64_t first=o;
        /* Short: the following instructions, else the one or two before. */
        for (uint64_t k=o+length; length<5 && n<REGION_ITEMS && k<end && !bit(targets,k) && decode(k,end,&items[n]); ++n) {
            length+=items[n].in.length;
            k+=items[n].in.length;
        }
        if (length<5) {
            const Item site=items[0];
            const uint64_t p0=sites[i].previous[0], p1=sites[i].previous[1];
            Item before[2];
            n=1; length=site.in.length;
            if (!bit(targets,o) && p0!=UINT64_MAX && p0>=covered && p0>=start && decode(p0,end,&before[1])) {
                length+=before[1].in.length;
                if (length<5 && !bit(targets,p0) && p1!=UINT64_MAX && p1>=covered && p1>=start && decode(p1,end,&before[0])) {
                    length+=before[0].in.length;
                    items[0]=before[0]; items[1]=before[1]; items[2]=site; n=3; first=p1;
                } else { items[0]=before[1]; items[1]=site; n=2; first=p0; }
            }
            if (length<5) { items[0]=site; n=1; first=o; length=site.in.length; }
        }
        uint64_t starts[REGION_ITEMS];
        unsigned char *thunk=emit_thunk(items,n,starts);
        if (!thunk) { ++left; continue; }
        if (length<5) {
            if (!add_breakpoint((uint64_t)(uintptr_t)(image+o),starts[0])) { ++left; continue; }
            image[o]=0xcc;
            ++traps;
        } else {
            unsigned char *f=image+first;
            const int32_t to=(int32_t)((int64_t)(uintptr_t)thunk-(int64_t)(uintptr_t)(f+5));
            for (size_t k=0;k<n;++k)
                if (items[k].at>=(uint64_t)(uintptr_t)(f+5)) add_breakpoint(items[k].at,starts[k]);
            f[0]=0xe9; memcpy(f+1,&to,4); memset(f+5,0xcc,length-5);
            ++jumps;
            moved+=(unsigned)(n-1);
        }
        covered=first+length;
    }
    qsort(breakpoints,breakpoint_count,sizeof(*breakpoints),by_address);
    char kinds[256]="";
    unsigned total=0;
    for (int k=0;k<KINDS;++k) total+=rewritten[k];
    for (int k=0;k<KINDS;++k)
        if (rewritten[k] && (k!=MOVBE_STORE || !rewritten[MOVBE_LOAD]))
            snprintf(kinds+strlen(kinds),sizeof(kinds)-strlen(kinds),"%s%s %u",*kinds ? ", " : "",kind_names[k],
                     k==MOVBE_LOAD ? rewritten[k]+rewritten[MOVBE_STORE] : rewritten[k]);
    printf("CPU: %s%s%s%s: %u instructions rewritten (%s); %u jumps with %u moved neighbours, %u breakpoints, %u left as they were\n",
           missing ? "this processor lacks the PS4's" : "rewrite forced",missing&NEED_BMI1 ? " BMI1" : "",
           missing&NEED_LZCNT ? " LZCNT" : "",missing&NEED_MOVBE ? " MOVBE" : "",
           total,*kinds ? kinds : "none",jumps,moved,traps,left);
    free(targets);
    free(sites);
}

int cpu_patch_breakpoint(uint64_t *rip, uint64_t address) {
    size_t low=0, high=breakpoint_count;
    while (low<high) {
        const size_t middle=(low+high)/2;
        if (breakpoints[middle].address<address) low=middle+1; else high=middle;
    }
    if (low==breakpoint_count || breakpoints[low].address!=address) return 0;
    *rip=breakpoints[low].thunk;
    return 1;
}
int cpu_patch_illegal(uint64_t *rip) {
    if (!image || *rip-(uint64_t)(uintptr_t)image>=image_size) return 0;
    const char *mode=getenv("BB_CPU_PATCH");
    if (mode && !strcmp(mode,"0")) return 0;
    pthread_mutex_lock(&missed_lock);
    uint64_t thunk=0;
    for (size_t i=0;i<missed_count && !thunk;++i) if (missed[i].address==*rip) thunk=missed[i].thunk;
    if (!thunk) {
        const unsigned saved=need;
        need=NEED_BMI1|NEED_LZCNT|NEED_MOVBE;
        Item item;
        uint64_t start;
        const uint64_t offset=*rip-(uint64_t)(uintptr_t)image;
        Breakpoint *more=realloc(missed,(missed_count+1)*sizeof(*more));
        if (more && (missed=more) && decode(offset,image_size,&item) && item.rewrite && emit_thunk(&item,1,&start)) {
            thunk=start;
            missed[missed_count++]=(Breakpoint){*rip,thunk};
            printf("CPU: %s at guest offset %#" PRIx64 " runs from a thunk (not found by the scan)\n",
                   ZydisMnemonicGetString(item.in.mnemonic),offset);
            fflush(stdout);
        }
        need=saved;
    }
    pthread_mutex_unlock(&missed_lock);
    if (!thunk) return 0;
    *rip=thunk;
    return 1;
}
