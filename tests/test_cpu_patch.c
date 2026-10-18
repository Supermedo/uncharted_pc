/* cpu_patch.c on a CPU that has BMI1, LZCNT and MOVBE: each case runs natively, then rewritten
 * (BB_CPU_PATCH=all), from the same registers, flags and memory; the results must match in every
 * register, the defined flags and memory. x86-64 Linux and Windows. */
#define _GNU_SOURCE
#include "cpu_patch.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

/* Instruction bytes from the assembler: name[] .. name_end[]. */
#define SNIP(name,text) __asm__(".pushsection .data\n" #name ":\n" text "\n" #name "_end:\n.popsection"); \
    extern const unsigned char name[], name##_end[];
SNIP(none,"")
SNIP(jmp0,"jmp 1f\n1:")
SNIP(nop1,"nop")
SNIP(push2,"push $0x7a5a3c1e\npush $-0x2b4c6d8f")
SNIP(pop2,"lea 16(%rsp),%rsp")
SNIP(load_rip,"mov 0(%rip),%r8")
SNIP(lz32,"lzcnt %ecx,%eax")
SNIP(lz64,"lzcnt %rcx,%rax")
SNIP(lz_same,"lzcnt %eax,%eax")
SNIP(lz16,"lzcnt %cx,%ax")
SNIP(lz_mem,"lzcnt (%r12),%esi")
SNIP(lz_rip,"lzcnt 0(%rip),%r9")
SNIP(lz_pair,"lzcnt %ecx,%eax\nlzcnt %edx,%ebx")
SNIP(lz_high,"lzcnt %r14d,%r15d")
SNIP(tz32,"tzcnt %ecx,%eax")
SNIP(tz64,"tzcnt %r8,%r9")
SNIP(tz16,"tzcnt %si,%di")
SNIP(tz_mem,"tzcnt 8(%r12,%r13,8),%rbx")
SNIP(tz_rsp,"tzcnt 8(%rsp),%rbp")
SNIP(andn64,"andn %rsi,%rdi,%rax")
SNIP(andn32,"andn %esi,%edi,%eax")
SNIP(andn_d_s2,"andn %rax,%rdi,%rax")
SNIP(andn_d_s1,"andn %esi,%eax,%eax")
SNIP(andn_all,"andn %eax,%eax,%eax")
SNIP(andn_hi32,"andn %r10d,%r11d,%r10d")
SNIP(andn_mem,"andn (%r12),%rdi,%rax")
SNIP(andn_mem_d_s1,"andn 16(%r12),%rax,%rax")
SNIP(andn_mem_index,"andn (%r12,%r13,8),%r13,%r13")
SNIP(andn_rsp,"andn 8(%rsp),%rbx,%rcx")
SNIP(andn_rip,"andn 0(%rip),%rbx,%rdx")
SNIP(blsr64,"blsr %rsi,%rax")
SNIP(blsr_same,"blsr %eax,%eax")
SNIP(blsr_mem,"blsr (%r12),%rdx")
SNIP(blsr_index,"blsr 4(%r12,%r13,4),%r13d")
SNIP(blsmsk64,"blsmsk %rsi,%rax")
SNIP(blsmsk_same,"blsmsk %eax,%eax")
SNIP(blsmsk_mem,"blsmsk (%r12),%edx")
SNIP(blsi64,"blsi %rsi,%rax")
SNIP(blsi_same,"blsi %r9d,%r9d")
SNIP(blsi_mem,"blsi 8(%r12),%r11")
SNIP(bextr64,"bextr %rdx,%rsi,%rax")
SNIP(bextr32_mem,"bextr %ecx,(%r12),%eax")
SNIP(bextr_same,"bextr %eax,%eax,%eax")
SNIP(movbe_l32,"movbe (%r12),%eax")
SNIP(movbe_l64,"movbe (%r12),%rax")
SNIP(movbe_l16,"movbe (%r12),%ax")
SNIP(movbe_s32,"movbe %ecx,8(%r12)")
SNIP(movbe_s64,"movbe %rcx,(%r12,%r13,8)")
SNIP(movbe_s16,"movbe %cx,(%r12)")
SNIP(movbe_rip,"movbe 0(%rip),%r10")
SNIP(movbe_index,"movbe %r13,(%r12,%r13,8)")

enum { CF=0x1, PF=0x4, AF=0x10, ZF=0x40, SF=0x80, OF=0x800, ALL=CF|PF|AF|ZF|SF|OF };
enum { COUNT=CF|ZF, LOGIC=SF|ZF|CF|OF, BEXTR_FLAGS=ZF|CF|OF };
/* expect: J jump at the instruction, P jump at the one before, B breakpoint, - any. rip: the
 * last instruction of the body (1) or of post (2) reads the data at +16 RIP-relative. */
typedef struct {
    const char *name; const unsigned char *pre, *pre_end, *body, *body_end, *post, *post_end;
    unsigned flags; char expect; int rip;
} Case;
#define PART(x) x, x##_end
static const Case cases[]={
    {"lzcnt r32",PART(none),PART(lz32),PART(none),COUNT,'J',0},
    {"lzcnt r64",PART(none),PART(lz64),PART(none),COUNT,'J',0},
    {"lzcnt same",PART(none),PART(lz_same),PART(none),COUNT,'J',0},
    {"lzcnt r16",PART(none),PART(lz16),PART(none),COUNT,'J',0},
    {"lzcnt mem",PART(none),PART(lz_mem),PART(none),COUNT,'J',0},
    {"lzcnt rip",PART(none),PART(lz_rip),PART(none),COUNT,'J',1},
    {"lzcnt pair",PART(none),PART(lz_pair),PART(none),COUNT,'J',0},
    {"lzcnt r14d",PART(none),PART(lz_high),PART(none),COUNT,'J',0},
    {"lzcnt breakpoint",PART(jmp0),PART(lz32),PART(jmp0),COUNT,'B',0},
    {"lzcnt previous",PART(nop1),PART(lz32),PART(jmp0),COUNT,'P',0},
    {"lzcnt rip neighbour",PART(none),PART(lz32),PART(load_rip),COUNT,'J',2},
    {"tzcnt r32",PART(none),PART(tz32),PART(none),COUNT,'J',0},
    {"tzcnt r64",PART(none),PART(tz64),PART(none),COUNT,'J',0},
    {"tzcnt r16",PART(none),PART(tz16),PART(none),COUNT,'J',0},
    {"tzcnt mem",PART(none),PART(tz_mem),PART(none),COUNT,'J',0},
    {"tzcnt rsp",PART(push2),PART(tz_rsp),PART(pop2),COUNT,'J',0},
    {"tzcnt breakpoint",PART(jmp0),PART(tz32),PART(jmp0),COUNT,'B',0},
    {"andn r64",PART(none),PART(andn64),PART(none),LOGIC,'J',0},
    {"andn r32",PART(none),PART(andn32),PART(none),LOGIC,'J',0},
    {"andn d=s2",PART(none),PART(andn_d_s2),PART(none),LOGIC,'J',0},
    {"andn d=s1",PART(none),PART(andn_d_s1),PART(none),LOGIC,'J',0},
    {"andn all",PART(none),PART(andn_all),PART(none),LOGIC,'J',0},
    {"andn r10d d=s2",PART(none),PART(andn_hi32),PART(none),LOGIC,'J',0},
    {"andn mem",PART(none),PART(andn_mem),PART(none),LOGIC,'J',0},
    {"andn mem d=s1",PART(none),PART(andn_mem_d_s1),PART(none),LOGIC,'J',0},
    {"andn mem index=d",PART(none),PART(andn_mem_index),PART(none),LOGIC,'J',0},
    {"andn rsp",PART(push2),PART(andn_rsp),PART(pop2),LOGIC,'J',0},
    {"andn rip",PART(none),PART(andn_rip),PART(none),LOGIC,'J',1},
    {"blsr r64",PART(none),PART(blsr64),PART(none),LOGIC,'J',0},
    {"blsr same",PART(none),PART(blsr_same),PART(none),LOGIC,'J',0},
    {"blsr mem",PART(none),PART(blsr_mem),PART(none),LOGIC,'J',0},
    {"blsr index=d",PART(none),PART(blsr_index),PART(none),LOGIC,'J',0},
    {"blsmsk r64",PART(none),PART(blsmsk64),PART(none),LOGIC,'J',0},
    {"blsmsk same",PART(none),PART(blsmsk_same),PART(none),LOGIC,'J',0},
    {"blsmsk mem",PART(none),PART(blsmsk_mem),PART(none),LOGIC,'J',0},
    {"blsi r64",PART(none),PART(blsi64),PART(none),LOGIC,'J',0},
    {"blsi same",PART(none),PART(blsi_same),PART(none),LOGIC,'J',0},
    {"blsi mem",PART(none),PART(blsi_mem),PART(none),LOGIC,'J',0},
    {"bextr r64",PART(none),PART(bextr64),PART(none),BEXTR_FLAGS,'J',0},
    {"bextr mem",PART(none),PART(bextr32_mem),PART(none),BEXTR_FLAGS,'J',0},
    {"bextr same",PART(none),PART(bextr_same),PART(none),BEXTR_FLAGS,'J',0},
    {"movbe load32",PART(none),PART(movbe_l32),PART(none),ALL,'J',0},
    {"movbe load64",PART(none),PART(movbe_l64),PART(none),ALL,'J',0},
    {"movbe load16",PART(none),PART(movbe_l16),PART(none),ALL,'J',0},
    {"movbe store32",PART(none),PART(movbe_s32),PART(none),ALL,'J',0},
    {"movbe store64",PART(none),PART(movbe_s64),PART(none),ALL,'J',0},
    {"movbe store16",PART(none),PART(movbe_s16),PART(none),ALL,'J',0},
    {"movbe rip",PART(none),PART(movbe_rip),PART(none),ALL,'J',1},
    {"movbe index=src",PART(none),PART(movbe_index),PART(none),ALL,'J',0},
};
enum { CASES=sizeof(cases)/sizeof(*cases), TRIALS=400, DATA=0x80000, CODE_END=0x40000, MISSED=0x60000, IMAGE_SIZE=0x100000 };

static uint64_t in[17], out[17];
static unsigned char *image;
typedef void (*Harness)(void);
/* push callee-saved (both conventions); registers and flags from in[]; pre, body, post; registers
 * and flags to out[]. */
static size_t emit(unsigned char *p, const Case *c, size_t *body_at) {
    size_t n=0;
    static const unsigned char enter[]={0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    memcpy(p,enter,sizeof(enter)); n+=sizeof(enter);
    p[n++]=0x48; p[n++]=0xb8; uint64_t a=(uint64_t)(uintptr_t)in; memcpy(p+n,&a,8); n+=8;
    p[n++]=0xff; p[n++]=0xb0; p[n++]=0x80; p[n++]=0; p[n++]=0; p[n++]=0; p[n++]=0x9d;   /* push [rax+128]; popfq */
    for (int r=1;r<16;++r) {
        if (r==4) continue;
        p[n++]=(unsigned char)(0x48|(r>=8 ? 4 : 0)); p[n++]=0x8b; p[n++]=(unsigned char)(0x40|(r&7)<<3); p[n++]=(unsigned char)(8*r);
    }
    p[n++]=0x48; p[n++]=0x8b; p[n++]=0x00;                                                /* mov rax, [rax] */
    const unsigned char *parts[3][2]={{c->pre,c->pre_end},{c->body,c->body_end},{c->post,c->post_end}};
    for (int i=0;i<3;++i) {
        const size_t length=(size_t)(parts[i][1]-parts[i][0]);
        if (i==1) *body_at=n;
        memcpy(p+n,parts[i][0],length); n+=length;
        if (c->rip && c->rip==i) {                                                         /* 1 body, 2 post */
            const int32_t disp=(int32_t)((int64_t)(uintptr_t)(image+DATA+16)-(int64_t)(uintptr_t)(p+n));
            memcpy(p+n-4,&disp,4);
        }
    }
    p[n++]=0x9c;                                                                           /* pushfq */
    p[n++]=0x48; p[n++]=0xa3; a=(uint64_t)(uintptr_t)out; memcpy(p+n,&a,8); n+=8;         /* mov [out], rax */
    p[n++]=0x48; p[n++]=0xb8; memcpy(p+n,&a,8); n+=8;                                     /* movabs rax, out */
    for (int r=1;r<16;++r) {
        if (r==4) continue;
        p[n++]=(unsigned char)(0x48|(r>=8 ? 4 : 0)); p[n++]=0x89; p[n++]=(unsigned char)(0x40|(r&7)<<3); p[n++]=(unsigned char)(8*r);
    }
    p[n++]=0x8f; p[n++]=0x80; p[n++]=0x80; p[n++]=0; p[n++]=0; p[n++]=0;                  /* pop [rax+128] */
    static const unsigned char leave[]={0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0x5b,0xc3};
    memcpy(p+n,leave,sizeof(leave)); n+=sizeof(leave);
    return n;
}

static uint64_t state=0x9e3779b97f4a7c15;
static uint64_t next(void) { state^=state<<13; state^=state>>7; state^=state<<17; return state; }
static const unsigned edges[]={0,1,7,8,15,16,17,31,32,33,63,64,65,255};
enum { EDGES=sizeof(edges)/sizeof(*edges) };
static uint64_t edge(void) { return next()%3 ? edges[next()%EDGES] : next()%256; }
static uint64_t value(void) {
    switch (next()%10) {
    case 0: case 1: return 0;
    case 2: return UINT64_C(1)<<(next()%64);
    case 3: return ~UINT64_C(0);
    case 4: return next()&0xffffffff;
    case 5: return edge()|edge()<<8|(next()%2 ? 0 : next()<<16);                           /* BEXTR controls */
    case 6: return next()>>(next()%64);
    default: return next();
    }
}
typedef struct { uint64_t in[17], out[17]; unsigned char data[128], after[128]; } Trial;
static void run(Harness h, Trial *t) {
    memcpy(in,t->in,sizeof(in));
    memcpy(image+DATA,t->data,sizeof(t->data));
    h();
    memcpy(t->out,out,sizeof(out));
    memcpy(t->after,image+DATA,sizeof(t->after));
}

static volatile unsigned traps;
#ifdef _WIN32
static LONG CALLBACK trap(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t rip=info->ContextRecord->Rip;
    if (!cpu_patch_breakpoint(&rip,rip)) { fputs("unexpected breakpoint\n",stderr); _exit(1); }
    info->ContextRecord->Rip=rip;
    ++traps;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#else
static void trap(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    ucontext_t *uc=context;
    uint64_t rip=(uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    if (!cpu_patch_breakpoint(&rip,rip-1)) { static const char m[]="unexpected breakpoint\n"; (void)!write(2,m,sizeof(m)-1); _exit(1); }
    uc->uc_mcontext.gregs[REG_RIP]=(greg_t)rip;
    ++traps;
}
#endif

int main(void) {
#ifdef _WIN32
    image=VirtualAlloc((void *)(uintptr_t)0x20000000,IMAGE_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    AddVectoredExceptionHandler(1,trap);
#else
    image=mmap((void *)(uintptr_t)0x20000000,IMAGE_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    struct sigaction sa={0};
    sa.sa_sigaction=trap; sa.sa_flags=SA_SIGINFO;
    sigaction(SIGTRAP,&sa,NULL);
#endif
    assert(image==(void *)(uintptr_t)0x20000000);
    memset(image,0xcc,IMAGE_SIZE);
    Harness harness[CASES+1];
    size_t body[CASES+1], at=0x1000;
    for (size_t i=0;i<CASES;++i) {
        harness[i]=(Harness)(void *)(image+at);
        const size_t length=emit(image+at,&cases[i],&body[i]);
        body[i]+=at;
        at=(at+length+15)&~(size_t)15;
        assert(at<CODE_END);
    }
    /* Outside the swept range: an ANDN the scan missed, for cpu_patch_illegal. */
    const Case missed={"andn missed",PART(none),PART(andn_mem_index),PART(none),LOGIC,'-',0};
    harness[CASES]=(Harness)(void *)(image+MISSED);
    emit(image+MISSED,&missed,&body[CASES]);
    body[CASES]+=MISSED;

    static Trial trials[CASES+1][TRIALS];
    for (size_t i=0;i<=CASES;++i)
        for (int k=0;k<TRIALS;++k) {
            Trial *t=&trials[i][k];
            for (int r=0;r<16;++r) t->in[r]=value();
            t->in[4]=0;
            t->in[12]=(uint64_t)(uintptr_t)(image+DATA);
            t->in[13]=next()%4;
            t->in[16]=0x202|(next()&ALL);
            if (i<CASES && !strncmp(cases[i].name,"bextr",5)) {                         /* every start x length edge */
                const uint64_t control=edges[k%EDGES]|(uint64_t)edges[k/EDGES%EDGES]<<8;
                t->in[0]=t->in[1]=t->in[2]=control;
                t->in[6]=k&1 ? ~UINT64_C(0)>>(k%3) : next();
            }
            for (size_t j=0;j<sizeof(t->data);j+=8) { uint64_t v=value(); memcpy(t->data+j,&v,8); }
            run(harness[i],t);
        }

#ifdef _WIN32
    _putenv("BB_CPU_PATCH=all");
#else
    setenv("BB_CPU_PATCH","all",1);
#endif
    const uint64_t ranges[1][2]={{0x1000,CODE_END-0x1000}};
    cpu_patch_image(image,IMAGE_SIZE,ranges,1);
    for (size_t i=0;i<CASES;++i) {
        const unsigned char at_body=image[body[i]], before=image[body[i]-1];
        const char e=cases[i].expect;
        if ((e=='J' && at_body!=0xe9) || (e=='B' && at_body!=0xcc) || (e=='P' && before!=0xe9)) {
            printf("FAIL %s: expected %c, found %02x (before %02x)\n",cases[i].name,e,at_body,before);
            return 1;
        }
    }
    /* The missed ANDN faults on an old CPU and the handler moves rip to a thunk; here a jmp over
     * its first 5 bytes (it has 6) stands in for the fault. */
    unsigned char *site=image+body[CASES];
    uint64_t rip=(uint64_t)(uintptr_t)site;
    assert(cpu_patch_illegal(&rip) && rip!=(uint64_t)(uintptr_t)site);
    assert(site[0]==0xc4 && andn_mem_index_end-andn_mem_index==6);
    const int32_t to=(int32_t)((int64_t)rip-(int64_t)(uintptr_t)(site+5));
    site[0]=0xe9; memcpy(site+1,&to,4);

    int failures=0;
    for (size_t i=0;i<=CASES;++i) {
        const Case *c=i<CASES ? &cases[i] : &missed;
        for (int k=0;k<TRIALS;++k) {
            Trial *t=&trials[i][k], again=*t;
            run(harness[i],&again);
            int bad=memcmp(t->after,again.after,sizeof(t->after))!=0 || ((t->out[16]^again.out[16])&c->flags);
            for (int r=0;r<16;++r) if (r!=4 && t->out[r]!=again.out[r]) bad=1;
            if (bad && failures++<10) {
                printf("FAIL %s trial %d\n",c->name,k);
                for (int r=0;r<17;++r)
                    if (r!=4) printf("  %2d in %016llx native %016llx rewritten %016llx%s\n",r,(unsigned long long)t->in[r],
                                     (unsigned long long)t->out[r],(unsigned long long)again.out[r],t->out[r]!=again.out[r] ? " <" : "");
            }
        }
    }
    if (failures) { printf("%d failures\n",failures); return 1; }
    printf("cpu_patch: %d cases x %d trials match (%u breakpoints taken)\n",(int)CASES+1,TRIALS,traps);
    return 0;
}
