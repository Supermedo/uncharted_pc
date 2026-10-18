/* Guest instructions the host CPU lacks (cpu_patch.c). */
#ifndef CPU_PATCH_H
#define CPU_PATCH_H
#include <stddef.h>
#include <stdint.h>

/* Rewrites the instructions this CPU lacks in the executable ranges ({offset, size} in the
 * image) before guest code runs; the image must still be writable. BB_CPU_PATCH=0 turns it
 * off, BB_CPU_PATCH=all rewrites every supported instruction (tests on a CPU that has them). */
void cpu_patch_image(unsigned char *image, uint64_t image_size, const uint64_t (*ranges)[2], size_t count);
/* Exception handlers, 1 when handled (*rip moved to a thunk): a breakpoint the rewrite put at
 * `address`, or an illegal instruction at *rip in the image that the scan did not reach. */
int cpu_patch_breakpoint(uint64_t *rip, uint64_t address);
int cpu_patch_illegal(uint64_t *rip);

/* An instruction run by C code (thunks for BEXTR and 16-bit MOVBE); public for the thunks. */
typedef struct { int base, index, scale, rip; int64_t disp; uint64_t absolute; } CpuPatchMem;
typedef struct { int kind, width, reg[3]; CpuPatchMem mem; } CpuPatchOp;
#endif
