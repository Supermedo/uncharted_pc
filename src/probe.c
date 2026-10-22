/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <cpuid.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
#ifdef _WIN32
/* BB_WATCH=offset:type,... (diagnosis): guest image variables printed every 5 seconds.
 * Types: f float, d double, b byte, i int32, q int64. */
/* BB_POKE=offset:type=value,... (experiments): written before each print. */
static void apply_pokes(void) {
    const char *p=getenv("BB_POKE");
    while (p && *p) {
        char *end; uint64_t offset=strtoull(p,&end,16);
        char type=*end==':' ? end[1] : 'f';
        const char *value=strchr(end,'=');
        if (!value) break;
        unsigned char *at=image+offset;
        if (type=='d') *(double *)at=strtod(value+1,NULL);
        else if (type=='b') *at=(unsigned char)strtoul(value+1,NULL,0);
        else if (type=='i') *(int32_t *)at=(int32_t)strtol(value+1,NULL,0);
        else *(float *)at=strtof(value+1,NULL);
        p=strchr(value,',');
        if (p) ++p;
    }
}
static DWORD WINAPI watch_thread(void *argument) {
    const char *list=argument;
    for (;;) {
        Sleep(5000);
        apply_pokes();
        char line[1024]; size_t used=0;
        for (const char *p=list; *p && used<sizeof(line)-64; ) {
            char *end; uint64_t offset=strtoull(p,&end,16);
            char type=*end==':' ? end[1] : 'f';
            const unsigned char *at=image+offset;
            if (type=='d') used+=snprintf(line+used,sizeof(line)-used," %llx=%g",(unsigned long long)offset,*(const double *)at);
            else if (type=='b') used+=snprintf(line+used,sizeof(line)-used," %llx=%u",(unsigned long long)offset,*at);
            else if (type=='i') used+=snprintf(line+used,sizeof(line)-used," %llx=%d",(unsigned long long)offset,*(const int32_t *)at);
            else if (type=='q') used+=snprintf(line+used,sizeof(line)-used," %llx=%lld",(unsigned long long)offset,*(const long long *)at);
            else used+=snprintf(line+used,sizeof(line)-used," %llx=%g",(unsigned long long)offset,*(const float *)at);
            p=strchr(end,',');
            if (!p) break;
            ++p;
        }
        printf("Watch:%s\n",line);
        fflush(stdout);
    }
    return 0;
}
#endif
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* The game is built for the PS4's AMD Jaguar CPU: AVX throughout, and BMI1 (andn, bextr, blsi, blsr,
 * tzcnt), MOVBE, LZCNT and POPCNT in thousands of places, its libc's string functions among them.
 * Without them it stopped at the first one with an illegal instruction (0xc000001d in libc.prx
 * while module 0 initialized), except LZCNT, which older Intel CPUs run as BSR (other results).
 * Exit code 24 with a message instead; BB_SKIP_CPU_CHECK=1 starts anyway. */
static void check_cpu(void) {
    if (getenv("BB_SKIP_CPU_CHECK")) return;
    unsigned a=0, b=0, c=0, d=0, basic=0, leaf7=0, extended=0;
    if (__get_cpuid(1,&a,&b,&c,&d)) basic=c;
    if (__get_cpuid_max(0,NULL)>=7) { __cpuid_count(7,0,a,b,c,d); leaf7=b; }    if (__get_cpuid(0x80000001,&a,&b,&c,&d)) extended=c;
    int avx=0;
    if (((basic>>27)&1) && ((basic>>28)&1)) { /* OSXSAVE and AVX: the OS also has to save YMM state */
        unsigned low, high;
        __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        avx=(low&6)==6;
    }
    const struct { int present; const char *name; } features[]={
        {avx,"AVX"}, {(leaf7>>3)&1,"BMI1"}, {(basic>>22)&1,"MOVBE"}, {(extended>>5)&1,"LZCNT"},
        {(basic>>23)&1,"POPCNT"}, {(basic>>20)&1,"SSE4.2"},
    };
    char missing[64]="";
    int no_movbe=0;
    for (size_t i=0;i<sizeof(features)/sizeof(*features);++i) {
        if (features[i].present) continue;
#ifdef _WIN32
        /* MOVBE alone (AMD FX): the fault handler does it (emulate_movbe). */
        if (!strcmp(features[i].name,"MOVBE")) { no_movbe=1; continue; }
#endif
        if (*missing) strcat(missing,", ");
        strcat(missing,features[i].name);
    }
    if (!*missing) {
        if (no_movbe) printf("CPU: no MOVBE, the game's MOVBE instructions are emulated (slower)\n");
        return;
    }
    unsigned words[12]={0};
    char brand[49]="";
    if (__get_cpuid_max(0x80000000,NULL)>=0x80000004)
        for (unsigned i=0;i<3;++i) __get_cpuid(0x80000002+i,&words[i*4],&words[i*4+1],&words[i*4+2],&words[i*4+3]);
    memcpy(brand,words,48);
    for (size_t n=strlen(brand); n && brand[n-1]==' '; --n) brand[n-1]=0;
    const char *name=brand;
    while (*name==' ') ++name;
    if (!*name) name="unknown processor";
    fprintf(stderr,"STOP: unsupported CPU %s: missing %s (needs Intel Core 4th generation or newer, or AMD Ryzen)\n",name,missing);
#ifdef _WIN32
    char text[512];
    snprintf(text,sizeof(text),"This processor cannot run the game:\n%s\n\nMissing instructions: %s\n\n"
             "The game uses instructions of the PS4's processor. They need an Intel Core of the 4th "
             "generation (Haswell, 2013) or newer, or an AMD Ryzen.",name,missing);
    MessageBoxA(NULL,text,"Uncharted",MB_OK|MB_ICONERROR|MB_SETFOREGROUND);
#endif
    exit(24);
}
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    if (gpu_enabled) bbgpu_dump_guest_writes(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* AMD FX (Piledriver) has everything the PS4's code needs except MOVBE (byte-swapped loads and
 * stores, a few hundred places in each game). There it raises an illegal instruction: the load
 * or store is done here and the guest goes on after it (issue #251). Register operand forms do
 * not exist; segment and address-size prefixes are left to the fault report. */
static volatile LONG64 movbe_emulated;
/* Decodes a MOVBE with a memory operand at p (regs: Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi,
 * R8-R15, as in CONTEXT): its length, or 0 when it is not one. */
static int movbe_decode(const unsigned char *p, const uint64_t *regs, uint64_t rip, uint64_t *address,
                        int *reg, int *width, int *store) {
    int n=0, size16=0, rex=0;
    while (n<3 && p[n]==0x66) { size16=1; ++n; }
    if ((p[n]&0xf0)==0x40) rex=p[n++];
    if (p[n]!=0x0f || p[n+1]!=0x38 || (p[n+2]!=0xf0 && p[n+2]!=0xf1)) return 0;
    *store=p[n+2]==0xf1;
    n+=3;
    const unsigned char modrm=p[n++];
    const int mod=modrm>>6, rm=modrm&7;
    *reg=((modrm>>3)&7)|((rex&4)<<1);
    if (mod==3) return 0;
    uint64_t at=0;
    int rip_relative=0;
    if (rm==4) {
        const unsigned char sib=p[n++];
        const int index=((sib>>3)&7)|((rex&2)<<2), base=(sib&7)|((rex&1)<<3);
        if (index!=4) at+=regs[index]<<(sib>>6);
        if ((sib&7)==5 && mod==0) { int32_t d; memcpy(&d,p+n,4); n+=4; at+=(uint64_t)(int64_t)d; }
        else at+=regs[base];
    } else if (rm==5 && mod==0) {
        int32_t d; memcpy(&d,p+n,4); n+=4; at=(uint64_t)(int64_t)d; rip_relative=1;
    } else {
        at=regs[rm|((rex&1)<<3)];
    }
    if (mod==1) { at+=(uint64_t)(int64_t)(int8_t)p[n]; n+=1; }
    else if (mod==2) { int32_t d; memcpy(&d,p+n,4); n+=4; at+=(uint64_t)(int64_t)d; }
    if (rip_relative) at+=rip+(uint64_t)n;
    *address=at;
    *width=(rex&8) ? 8 : size16 ? 2 : 4;
    return n;
}
static int emulate_movbe(CONTEXT *c) {
    DWORD64 *regs=&c->Rax;
    uint64_t address;
    int reg, width, store;
    const int n=movbe_decode((const unsigned char *)(uintptr_t)c->Rip,(const uint64_t *)regs,c->Rip,
                             &address,&reg,&width,&store);
    if (!n) return 0;
    void *memory=(void *)(uintptr_t)address;
    if (store) {
        const uint64_t v=regs[reg];
        if (width==8) { uint64_t s=__builtin_bswap64(v); memcpy(memory,&s,8); }
        else if (width==4) { uint32_t s=__builtin_bswap32((uint32_t)v); memcpy(memory,&s,4); }
        else { uint16_t s=__builtin_bswap16((uint16_t)v); memcpy(memory,&s,2); }
    } else if (width==8) {
        uint64_t v; memcpy(&v,memory,8); regs[reg]=__builtin_bswap64(v);
    } else if (width==4) {
        uint32_t v; memcpy(&v,memory,4); regs[reg]=__builtin_bswap32(v);
    } else {
        uint16_t v; memcpy(&v,memory,2); regs[reg]=(regs[reg]&~(DWORD64)0xffff)|__builtin_bswap16(v);
    }
    c->Rip+=(DWORD64)n;
    InterlockedIncrement64(&movbe_emulated);
    return 1;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && bbgpu_handle_fault(info,(void *)record->ExceptionInformation[1])) return EXCEPTION_CONTINUE_EXECUTION;
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in bb_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)bb_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ILLEGAL_INSTRUCTION && image &&
        (uintptr_t)info->ContextRecord->Rip-(uintptr_t)image<0x20000000 &&
        emulate_movbe(info->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* BB_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* BB_GUEST_HOOKS="offset:bytes,...,START-END": diagnostic call counters on guest functions.
 * The first `bytes` (>= 5, whole position-independent instructions) at each image offset move
 * to a thunk near the image, which counts the call and keeps its first two arguments; START-END
 * hooks every 16-byte aligned function in that range that begins with a frame-pointer prologue.
 * The counts that changed are printed every 2 s. */
#include <pthread.h>
typedef struct { volatile uint64_t calls, rdi, rsi, rdx, rcx, r8, r15; uint64_t offset, printed; } GuestHook;
static GuestHook *guest_hooks;
static size_t guest_hook_count;
static uint64_t guest_image_size;
/* A pointer argument into the image (or guest memory the tracker knows) shown as text. */
static void print_hook_string(const char *name, uint64_t value) {
    const unsigned char *p=(const unsigned char *)(uintptr_t)value;
    if (value<(uint64_t)(uintptr_t)image || value>=(uint64_t)(uintptr_t)image+guest_image_size-64) return;
    char text[65]; size_t n=0;
    while (n<64 && p[n]>=32 && p[n]<127) { text[n]=(char)p[n]; ++n; }
    text[n]=0;
    if (n>=3) printf("    %s -> \"%s\"\n",name,text);
}
static void *report_guest_hooks(void *unused) {
    (void)unused;
    for (;;) {
#ifdef _WIN32
        Sleep(2000);
#else
        sleep(2);
#endif
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        printf("Hooks at %.1f s\n",(double)now.tv_sec+now.tv_nsec/1e9);
        for (size_t i=0;i<guest_hook_count;++i) {
            GuestHook *h=&guest_hooks[i];
            uint64_t calls=h->calls;
            if (calls==h->printed) continue;
            printf("Hook %#" PRIx64 ": %" PRIu64 " calls (+%" PRIu64 ", last rdi %#" PRIx64 " rsi %#" PRIx64 " rdx %#" PRIx64
                   " rcx %#" PRIx64 " r8 %#" PRIx64 " r15 %#" PRIx64 ")\n",h->offset,calls,calls-h->printed,(uint64_t)h->rdi,
                   (uint64_t)h->rsi,(uint64_t)h->rdx,(uint64_t)h->rcx,(uint64_t)h->r8,(uint64_t)h->r15);
            h->printed=calls;
            print_hook_string("rdi",h->rdi); print_hook_string("rsi",h->rsi);
            print_hook_string("rdx",h->rdx); print_hook_string("rcx",h->rcx);
        }
        fflush(stdout);
    }
    return NULL;
}
/* BB_HOOK_DUMP=<hook offset>:<bytes>:<file>: every 20 ms the memory at the hook's last rdi
 * (an object) is appended to <file> as [double time][bytes] records (finding fields that follow
 * a value, e.g. a camera angle). */
static void *dump_hook_object(void *unused) {
    (void)unused;
    const char *spec=getenv("BB_HOOK_DUMP");
    char *end;
    const uint64_t offset=strtoull(spec,&end,0);
    const size_t bytes=*end==':' ? (size_t)strtoull(end+1,&end,0) : 0;
    FILE *f=*end==':' ? fopen(end+1,"wb") : NULL;
    GuestHook *h=NULL;
    for (size_t i=0;i<guest_hook_count;++i) if (guest_hooks[i].offset==offset) h=&guest_hooks[i];
    if (!f || !h || !bytes) { printf("BB_HOOK_DUMP: bad specification\n"); return NULL; }
    unsigned char *copy=malloc(bytes);
    const uint64_t before=getenv("BB_HOOK_DUMP_BEFORE") ? strtoull(getenv("BB_HOOK_DUMP_BEFORE"),NULL,0) : 0;
    for (;;) {
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
        const uint64_t object=h->rdi ? h->rdi-before : 0;
        if (!object) continue;
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)(uintptr_t)object,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize<object+bytes) continue;
#endif
        memcpy(copy,(const void *)(uintptr_t)object,bytes);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        const double t=(double)now.tv_sec+now.tv_nsec/1e9;
        fwrite(&t,sizeof(t),1,f);
        fwrite(copy,1,bytes,f);
        fflush(f);
    }
    return NULL;
}
/* Bytes of whole instructions (>= 5) a frame-pointer prologue at `f` can move, 0 if unknown. */
static size_t prologue_bytes(const unsigned char *f) {
    if (memcmp(f,"\x55\x48\x89\xe5",4)) return 0;
    if (f[4]==0x41 && f[5]>=0x50 && f[5]<=0x57) return 6;
    if (f[4]>=0x50 && f[4]<=0x57) return 5;
    if (f[4]==0x48 && f[5]==0x83 && f[6]==0xec) return 8;
    if (f[4]==0x48 && f[5]==0x81 && f[6]==0xec) return 11;
    return 0;
}
static void install_guest_hooks(Segment *segments, uint64_t ns) {
    const char *spec=getenv("BB_GUEST_HOOKS");
    if (!spec || !*spec) return;
    enum { MAX_HOOKS=16384, THUNK=80 };
    guest_hooks=calloc(MAX_HOOKS,sizeof(*guest_hooks));
    uint64_t end_of_image=0;
    for (uint64_t s=0;s<ns;++s) if (segments[s].address+segments[s].size>end_of_image) end_of_image=segments[s].address+segments[s].size;
    guest_image_size=end_of_image;
    unsigned char *thunks=NULL;
#ifdef _WIN32
    /* rel32 jumps: the thunks within 2 GiB of the image. */
    uint64_t base=((uint64_t)(uintptr_t)image+end_of_image+0xffff)&~(uint64_t)0xffff;
    for (uint64_t step=0;!thunks && step<(1900ull<<20);step+=1u<<16) {
        thunks=VirtualAlloc((void *)(uintptr_t)(base+step),(size_t)MAX_HOOKS*THUNK,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (!thunks && (uint64_t)(uintptr_t)image>step+(2u<<20))
            thunks=VirtualAlloc((void *)(uintptr_t)(((uint64_t)(uintptr_t)image-step-(2u<<20))&~(uint64_t)0xffff),
                                (size_t)MAX_HOOKS*THUNK,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    }
#endif
    if (!thunks) fail("BB_GUEST_HOOKS: no thunk memory near the image");
    for (const char *p=spec;*p;) {
        char *end;
        uint64_t first=strtoull(p,&end,0), last=first, n=0;
        if (*end==':') n=strtoull(end+1,&end,0);
        else if (*end=='-') last=strtoull(end+1,&end,0);
        else fail("bad BB_GUEST_HOOKS entry");
        if (*end && *end!=',') fail("bad BB_GUEST_HOOKS entry");
        p=*end==',' ? end+1 : end;
        for (uint64_t offset=first;offset<=last;offset+=first==last ? 1 : 16) {
            if (!mapped(segments,ns,offset,16)) { if (first==last) fail("bad BB_GUEST_HOOKS entry"); continue; }
            unsigned char *f=image+offset;
            size_t moved=first==last ? (size_t)n : prologue_bytes(f);
            if (first!=last && !moved) continue;
            if (moved<5 || moved>16 || guest_hook_count==MAX_HOOKS) fail("bad BB_GUEST_HOOKS entry");
            GuestHook *h=&guest_hooks[guest_hook_count];
            unsigned char *t=thunks+guest_hook_count*THUNK;
            ++guest_hook_count;
            h->offset=offset;
            uintptr_t slot=(uintptr_t)h;
            size_t k=0;
            t[k++]=0x50;                                                /* push rax */
            t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&slot,8); k+=8;        /* movabs rax, hook */
            t[k++]=0xf0; t[k++]=0x48; t[k++]=0xff; t[k++]=0x00;         /* lock inc qword [rax] */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x78; t[k++]=0x08;         /* mov [rax+8], rdi */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x70; t[k++]=0x10;         /* mov [rax+16], rsi */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x50; t[k++]=0x18;         /* mov [rax+24], rdx */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x48; t[k++]=0x20;         /* mov [rax+32], rcx */
            t[k++]=0x4c; t[k++]=0x89; t[k++]=0x40; t[k++]=0x28;         /* mov [rax+40], r8 */
            t[k++]=0x4c; t[k++]=0x89; t[k++]=0x78; t[k++]=0x30;         /* mov [rax+48], r15 */
            t[k++]=0x58;                                                /* pop rax */
            memcpy(t+k,f,moved); k+=moved;                              /* the moved instructions */
            int32_t back=(int32_t)((int64_t)(uintptr_t)(f+moved)-(int64_t)(uintptr_t)(t+k+5));
            t[k++]=0xe9; memcpy(t+k,&back,4); k+=4;                     /* jmp offset+moved */
            int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(f+5));
            f[0]=0xe9; memcpy(f+1,&to,4); memset(f+5,0x90,moved-5);
        }
    }
    printf("Hooks: %zu guest functions counted\n",guest_hook_count);
    pthread_t thread;
    pthread_create(&thread,NULL,report_guest_hooks,NULL);
    if (getenv("BB_HOOK_DUMP")) pthread_create(&thread,NULL,dump_hook_object,NULL);
}
/* Game callbacks (runtime_guest_callbacks: mouse camera): a guest function whose code matches
 * the expected bytes calls a host function first, with rdi, rsi, the image base and r14 (Microsoft
 * x64 convention on every host). The thunk keeps the guest's registers and flags, so it also
 * works inside a function. */
static unsigned char *near_image_memory(size_t size) {
    unsigned char *memory=NULL;
#ifdef _WIN32
    /* rel32 jumps: within 2 GiB of the image. */
    uint64_t end_of_image=round_page(guest_image_size ? guest_image_size : 1);
    uint64_t base=((uint64_t)(uintptr_t)image+end_of_image+(32u<<20)+0xffff)&~(uint64_t)0xffff;
    for (uint64_t step=0;!memory && step<(1900ull<<20);step+=1u<<16) {
        memory=VirtualAlloc((void *)(uintptr_t)(base+step),size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (!memory && (uint64_t)(uintptr_t)image>step+(2u<<20))
            memory=VirtualAlloc((void *)(uintptr_t)(((uint64_t)(uintptr_t)image-step-(2u<<20))&~(uint64_t)0xffff),
                                size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    }
#else
    (void)size;
#endif
    return memory;
}
static void install_game_callbacks(Segment *segments, uint64_t ns, uint64_t image_size) {
    size_t count=0;
    const RuntimeGuestCallback *callbacks=runtime_guest_callbacks(&count);
    unsigned char *thunks=NULL;
    size_t used=0;
    guest_image_size=image_size;
    for (size_t i=0;i<count;++i) {
        const RuntimeGuestCallback *c=&callbacks[i];
        if (!mapped(segments,ns,c->offset,c->signature_size) ||
            memcmp(image+c->offset,c->signature,c->signature_size)) continue;
        if (!c->callback) {
            memcpy(image+c->offset,c->patch,c->moved);
            printf("Callbacks: %s at %#" PRIx64 " (patch)\n",c->name,c->offset);
            continue;
        }
        if (!thunks && !(thunks=near_image_memory(4096))) { printf("Callbacks: no thunk memory\n"); return; }
        if (used+256>4096) break;
        unsigned char *t=thunks+used, *f=image+c->offset;
        size_t k=0;
        /* pushfq; push rax rcx rdx r8-r11 rbx; mov rbx, rsp; and rsp, -16; sub rsp, 0xb0 (works at
         * a function's start and inside it). The callback's 5th argument ([rsp+0x20]) is rbx: the
         * saved registers rbx r11 r10 r9 r8 rdx rcx rax rflags, then (at a function's start) the
         * return address. */
        static const unsigned char save[]={0x9c,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x53,
                                           0x48,0x89,0xe3,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec,0xb0,0x00,0x00,0x00,
                                           0x48,0x89,0x5c,0x24,0x20};
        memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
        for (int x=0;x<8;++x) {                                     /* movdqu [rsp+0x30+16x], xmmx */
            const uint32_t disp=0x30u+16u*(uint32_t)x;
            t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x7f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
            memcpy(t+k,&disp,4); k+=4;
        }
        t[k++]=0x48; t[k++]=0x89; t[k++]=0xf9;                      /* mov rcx, rdi */
        t[k++]=0x48; t[k++]=0x89; t[k++]=0xf2;                      /* mov rdx, rsi */
        const uintptr_t base=(uintptr_t)image, host=(uintptr_t)c->callback;
        t[k++]=0x49; t[k++]=0xb8; memcpy(t+k,&base,8); k+=8;        /* movabs r8, image */
        t[k++]=0x4d; t[k++]=0x89; t[k++]=0xf1;                      /* mov r9, r14 */
        t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8;        /* movabs rax, callback */
        t[k++]=0xff; t[k++]=0xd0;                                   /* call rax */
        for (int x=0;x<8;++x) {                                     /* movdqu xmmx, [rsp+0x30+16x] */
            const uint32_t disp=0x30u+16u*(uint32_t)x;
            t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x6f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
            memcpy(t+k,&disp,4); k+=4;
        }
        /* mov rsp, rbx; pop rbx r11-r8 rdx rcx rax; popfq */
        static const unsigned char restore[]={0x48,0x89,0xdc,0x5b,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,
                                              0x5a,0x59,0x58,0x9d};
        memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
        memcpy(t+k,f,c->moved); k+=c->moved;                       /* the moved instructions */
        int32_t back=(int32_t)((int64_t)(uintptr_t)(f+c->moved)-(int64_t)(uintptr_t)(t+k+5));
        t[k++]=0xe9; memcpy(t+k,&back,4); k+=4;
        int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(f+5));
        f[0]=0xe9; memcpy(f+1,&to,4); memset(f+5,0x90,c->moved-5);
        used+=(k+15)&~(size_t)15;
        printf("Callbacks: %s at %#" PRIx64 "\n",c->name,c->offset);
    }
}
/* Naughty Dog's FrameBegin (ndlib/render/fg-draw-mgr.cpp) asserts that the frame it recycles,
 * three back, has its GPU end-of-pipe tick (set by the game's EOP interrupt handler). It never
 * waits: on PS4 the GPU always keeps up. Here the GPU side can fall behind (shaders building on a
 * first run, an upscaler starting) and the game stopped right after U1's first cutscene. The
 * assertion's branch is found through its message, in any version of the three games, and the
 * failed check now waits (pause, check again) instead. */
static void patch_eop_tick_wait(uint64_t image_size) {
    static const char message[]="GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick";
    unsigned char *text=NULL;
    for (uint64_t i=0;i+sizeof(message)<=image_size;++i)
        if (image[i]=='G' && !memcmp(image+i,message,sizeof(message))) { text=image+i; break; }
    if (!text) return;
    int patched=0;
    for (uint64_t at=0;at+7<=image_size;++at) {
        /* lea rdi, [rip+rel32] -> the message */
        if (image[at]!=0x48 || image[at+1]!=0x8d || image[at+2]!=0x3d) continue;
        int32_t rel; memcpy(&rel,image+at+3,4);
        if (image+at+7+rel!=text) continue;
        /* Up to 64 bytes before: cmp qword [base(+index*scale)+disp8/32], 0 (REX.W 83 /7, mod
         * 01/10, with or without SIB: 1.00 has [rax+0xa08], US 1.02 [rax+rcx+0xa08]); jne past
         * the message, with room for the 4 patched bytes before it */
        for (uint64_t back=10;back<=64 && back<=at;++back) {
            unsigned char *c=image+at-back;
            if ((c[0]&0xf8)!=0x48 || c[1]!=0x83 || ((c[2]>>3)&7)!=7) continue;
            const int mod=c[2]>>6, sib=(c[2]&7)==4;
            if (mod!=1 && mod!=2) continue;
            const int length=3+sib+(mod==2 ? 4 : 1);
            if (c[length]!=0x00 || c[length+1]!=0x75) continue;
            unsigned char *jne=c+length+1, *ok=jne+2+(int8_t)jne[1];
            if (ok<=image+at || image+at-(jne+2)<4) continue;
            /* after the jne: pause; jmp back to the cmp */
            jne[2]=0xf3; jne[3]=0x90; jne[4]=0xeb; jne[5]=(unsigned char)(int8_t)(c-(jne+6));
            printf("Frame pacing: the EOP tick check at %#" PRIx64 " waits for the GPU\n",
                   (uint64_t)(c-image));
            ++patched;
            break;
        }
    }
    if (!patched) printf("Frame pacing: EOP tick check not found\n");
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets BB_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("BB_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: BB_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    TerminateProcess(GetCurrentProcess(),0);
#else
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    check_cpu();
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, list_missing = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *guest_argv_file=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--list-missing")) list_missing = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--guest-argv") && i+1<argc) guest_argv_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--list-missing] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--guest-argv file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        char serial[16]="", title[128]="", version[16]="";
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"APP_VER",version,sizeof(version),NULL);
        bbnet_configure(app0,serial,title,version);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("BB_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"BB_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    if (list_missing) {
        unsigned char *kinds=calloc(import_count ? import_count : 1,1);
        if (!kinds) fail("allocation failed");
        for (uint64_t i=0;i<nr;++i) if (relocs[i].kind) kinds[relocs[i].value]=(unsigned char)relocs[i].kind;
        uint64_t missing=0;
        for (uint64_t i=0;i<import_count;++i) {
            if (!kinds[i] || runtime_resolve(names[i],kinds[i]==2) || (native_libc && bindings[i])) continue;
            const char *symbol=runtime_symbol(names[i]);
            printf("missing %s %s%s\n",names[i],symbol ? symbol : "?",kinds[i]==2 ? " (data)" : "");
            ++missing;
        }
        printf("%" PRIu64 " of %" PRIu64 " imports missing\n",missing,import_count);
        return missing ? 20 : 0;
    }
    image = runtime_image_map(round_page(size), PROT_READ | PROT_WRITE);
    if (!image) image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    install_guest_hooks(segments, ns);
    {
        uint64_t image_end=0;
        for (uint64_t s=0;s<ns;++s) if (segments[s].address+segments[s].size>image_end) image_end=segments[s].address+segments[s].size;
        install_game_callbacks(segments, ns, image_end);
        patch_eop_tick_wait(image_end);
    }
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    printf("Guest image at %p\n", (void *)image);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        runtime_set_guest_fpu();
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
#ifdef _WIN32
    if (getenv("BB_WATCH")) CloseHandle(CreateThread(NULL,0,watch_thread,getenv("BB_WATCH"),0,NULL));
#endif
    /* argv: the executable and, after sceSystemServiceLoadExec, the arguments it was given
     * (--guest-argv: one per line, the guest path first). */
    struct { uint64_t argc; const char *argv[34]; } params = {1, {"/app0/eboot.bin", NULL}};
    if (guest_argv_file) {
        FILE *list=fopen(guest_argv_file,"rb");
        static char lines[33][1024];
        uint64_t n=0;
        while (list && n<33 && fgets(lines[n],sizeof(lines[n]),list)) {
            lines[n][strcspn(lines[n],"\r\n")]=0;
            params.argv[n]=lines[n]; ++n;
        }
        if (list) fclose(list);
        if (n) { params.argc=n; params.argv[n]=NULL; }
        printf("Guest argv: %s (+%" PRIu64 " arguments)\n",params.argv[0],params.argc-1);
    }
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
