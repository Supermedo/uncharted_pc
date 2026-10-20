/* red_zone.c on Windows. Build and run in the MSYS2 CLANG64 shell, from the repository:
 *   clang -std=c11 -O2 -Wall -Wextra -Werror -Isrc tests/test_red_zone.c src/red_zone.c -lZydis -o out/red-zone-test.exe
 *   out/red-zone-test.exe [a decrypted game executable: out/eboot/eboot.elf, out/big2/eboot.elf, out/big3/eboot.elf]
 * 1. A leaf that keeps 16 qwords below rsp takes a write fault (a read-only page that the handler
 *    makes writable, as the GPU page tracking does) with the upper YMM halves in use: how many
 *    qwords Windows left, with the store as it is and with rsp lowered by 128 around it.
 * 2. With a game's executable: its constant table builder (U1 US 1.02 0x63df10) fills a table
 *    whose end is on a read-only page, at every stack alignment, before and after
 *    red_zone_patch, against a run without a fault. */
#include "red_zone.h"
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *guarded;              /* the page a store faults on */
static volatile long faults;
static LONG CALLBACK handler(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r=e->ExceptionRecord;
    if (r->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && r->NumberParameters>=2 &&
        r->ExceptionInformation[1]-(ULONG_PTR)guarded<4096) {
        ++faults;
        DWORD old;
        VirtualProtect(guarded,4096,PAGE_READWRITE,&old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void guard(void) { DWORD old; VirtualProtect(guarded,4096,PAGE_READONLY,&old); }

/* MS ABI: rcx = the store's target, edx = 1 to lower rsp around it. Returns the qwords of
 * [rsp-128, rsp) that still hold the pattern after the store. */
extern int leaf(void *target, int lower);
__asm__(".globl leaf\nleaf:\n"
        "  movabs $0x1122334455667788, %rax\n mov $1, %r9\n"
        "1: mov %rsp, %r10\n mov %r9, %r11\n shl $3, %r11\n sub %r11, %r10\n mov %rax, (%r10)\n"
        "  inc %r9\n cmp $17, %r9\n jne 1b\n"
        "  vpcmpeqd %xmm0, %xmm0, %xmm0\n vinsertf128 $1, %xmm0, %ymm0, %ymm0\n vmovaps %ymm0, %ymm1\n"
        "  test %edx, %edx\n jz 2f\n"
        "  lea -0x80(%rsp), %rsp\n movl $1, (%rcx)\n lea 0x80(%rsp), %rsp\n jmp 3f\n"
        "2: movl $1, (%rcx)\n"
        "3: xor %eax, %eax\n mov $1, %r9\n"
        "4: mov %rsp, %r10\n mov %r9, %r11\n shl $3, %r11\n sub %r11, %r10\n"
        "  movabs $0x1122334455667788, %r11\n cmp %r11, (%r10)\n jne 5f\n inc %eax\n"
        "5: inc %r9\n cmp $17, %r9\n jne 4b\n"
        "  vzeroupper\n ret\n");
/* Runs a System V function (rdi, rsi, edx) with the upper YMM halves in use, as game code does,
 * with rsp `stack_shift` bytes below a 64-byte boundary at the call. A fault's frame overwrites
 * the stack from the 64-byte boundary at or below rsp down, so that decides what it reaches.
 * MS ABI: rcx = function, rdx, r8, r9d = its arguments. */
uint64_t stack_shift;
extern void call_with_avx(void *function, void *a, const void *b, uint32_t c);
__asm__(".globl call_with_avx\ncall_with_avx:\n"
        "  push %rdi\n push %rsi\n push %rbx\n mov %rsp, %rbx\n and $-64, %rsp\n sub stack_shift(%rip), %rsp\n"
        "  mov %rcx, %rax\n mov %rdx, %rdi\n mov %r8, %rsi\n mov %r9d, %edx\n"
        "  vpcmpeqd %xmm1, %xmm1, %xmm1\n vinsertf128 $1, %xmm1, %ymm1, %ymm1\n vmovaps %ymm1, %ymm2\n"
        "  call *%rax\n vzeroupper\n"
        "  mov %rbx, %rsp\n pop %rbx\n pop %rsi\n pop %rdi\n ret\n");

static int failures;
static void check(int ok, const char *what) {
    printf("%s %s\n",ok ? "ok:  " : "FAIL:",what);
    if (!ok) ++failures;
}

static void test_windows_frame(void) {
    int kept[2]={16,16};
    for (int lower=0;lower<2;++lower)
        for (int i=0;i<200;++i) {
            guard();
            const int k=leaf(guarded,lower);
            if (k<kept[lower]) kept[lower]=k;
        }
    printf("Windows exception frame with the AVX state in use: %d of the 16 qwords below rsp kept when the "
           "store faults as it is, %d with rsp lowered by 128\n",kept[0],kept[1]);
    check(kept[1]==16,"a fault under a lowered rsp keeps the 128 bytes below the old rsp");
}

/* The game's executable at its load addresses, in executable memory. */
static unsigned char *game;
static RedZoneSegment segments[16];
static size_t segment_count;
static uint64_t load_elf(const char *path) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    fseek(f,0,SEEK_END);
    const long length=ftell(f);
    fseek(f,0,SEEK_SET);
    unsigned char *file=length>0x40 ? malloc((size_t)length) : NULL;
    const int read=file && fread(file,1,(size_t)length,f)==(size_t)length;
    fclose(f);
    if (!read) { free(file); return 0; }
    uint64_t phoff, size=0;
    uint16_t phnum;
    memcpy(&phoff,file+0x20,8); memcpy(&phnum,file+0x38,2);
    for (int pass=0;pass<2;++pass) {
        if (pass && !(game=VirtualAlloc(NULL,size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE))) break;
        for (unsigned i=0;i<phnum && phoff+56*(uint64_t)(i+1)<=(uint64_t)length;++i) {
            const unsigned char *ph=file+phoff+56*(uint64_t)i;
            uint32_t type, flags;
            uint64_t offset, vaddr, filesz, memsz;
            memcpy(&type,ph,4); memcpy(&flags,ph+4,4); memcpy(&offset,ph+8,8); memcpy(&vaddr,ph+16,8);
            memcpy(&filesz,ph+32,8); memcpy(&memsz,ph+40,8);
            if (type!=1 || offset+filesz>(uint64_t)length || filesz>memsz) continue;
            if (!pass) { if (vaddr+memsz>size) size=vaddr+memsz; continue; }
            memcpy(game+vaddr,file+offset,filesz);
            if (segment_count<16) segments[segment_count++]=(RedZoneSegment){vaddr,memsz,flags};
        }
    }
    free(file);
    return game ? size : 0;
}

/* The builder's first bytes: the same in U1 (US 1.02 0x63df10), U2 (0xd645f0) and U3 (0x12d6cf0). */
enum { SLOTS=40, TABLE=0x28+0x30*SLOTS };
static const unsigned char builder_start[]={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x49,0xb9,
                                            0x00,0x24,0x00,0x0d,0x10,0x00,0x00,0x00};
static unsigned char *builder;
/* Slot usage entries {type, slot, register, flags}: every class of entry the builder makes
 * (types 0-4 and 6-8 with registers below and above 16, and 10, 13, 24). */
static void make_slots(unsigned char *slots) {
    static const unsigned char types[]={0,1,2,4,6,7,8,10,13,24};
    uint32_t seed=12345;
    for (int i=0;i<SLOTS;++i) {
        seed=seed*1103515245u+12345u;
        slots[4*i]=types[(seed>>16)%sizeof(types)];
        slots[4*i+1]=(unsigned char)(seed>>8);
        slots[4*i+2]=(unsigned char)((seed>>4)%32);
        slots[4*i+3]=(unsigned char)(seed>>24);
    }
}
/* Tables built with entry `first` and later on the read-only page, at each of the 8 stack
 * alignments, that differ from `reference`. */
enum { RUNS=8*(SLOTS-1) };
static int wrong_tables(const unsigned char *slots, const unsigned char *reference, long *taken) {
    int wrong=0;
    *taken=0;
    for (int run=0;run<RUNS;++run) {
        const int first=1+run/8;
        stack_shift=8*(uint64_t)(run%8);
        unsigned char *table=guarded-0x28-0x30*first;
        DWORD old;
        VirtualProtect(guarded,4096,PAGE_READWRITE,&old);
        memset(table,0xab,TABLE);
        guard();
        const long before=faults;
        call_with_avx(builder,table,slots,SLOTS);
        *taken+=faults-before;
        if (memcmp(table,reference,TABLE)) ++wrong;
    }
    return wrong;
}
static void test_game(const char *path) {
    const uint64_t size=load_elf(path);
    for (size_t s=0;s<segment_count && !builder;++s)
        for (uint64_t at=segments[s].address; (segments[s].flags&1) && at+sizeof(builder_start)<=segments[s].address+segments[s].size; ++at)
            if (game[at]==0x55 && !memcmp(game+at,builder_start,sizeof(builder_start))) { builder=game+at; break; }
    if (!size || !builder) {
        printf("skip: %s is not an executable of the collection (no constant table builder)\n",path);
        return;
    }
    printf("%s: the constant table builder is at %#" PRIx64 "\n",path,(uint64_t)(builder-game));
    unsigned char slots[4*SLOTS], reference[TABLE], *table=malloc(TABLE);
    make_slots(slots);
    memset(table,0xab,TABLE);
    call_with_avx(builder,table,slots,SLOTS);
    memcpy(reference,table,TABLE);
    free(table);
    long taken;
    const int before=wrong_tables(slots,reference,&taken);
    printf("builder as it is: %d of %d tables wrong after a fault (%ld faults)\n",before,RUNS,taken);
    red_zone_patch(game,size,segments,segment_count);
    const int after=wrong_tables(slots,reference,&taken);
    printf("builder after red_zone_patch: %d of %d tables wrong after a fault (%ld faults)\n",after,RUNS,taken);
    check(taken>=RUNS,"every patched run took its fault");
    check(after==0,"the patched builder fills the table as without a fault");
}

int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    AddVectoredExceptionHandler(1,handler);
    guarded=(unsigned char *)VirtualAlloc(NULL,2*4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)+4096;
    test_windows_frame();
    if (argc>1) test_game(argv[1]);
    printf(failures ? "%d FAILED\n" : "all passed\n",failures);
    return failures!=0;
}
