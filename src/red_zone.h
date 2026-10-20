/* Guest leaf functions' red zone on Windows (red_zone.c). */
#ifndef RED_ZONE_H
#define RED_ZONE_H
#include <stddef.h>
#include <stdint.h>

/* A loaded segment: offset in the image, size, ELF flags (1 execute, 2 write, 4 read). */
typedef struct { uint64_t address, size, flags; } RedZoneSegment;

/* Before guest code runs (the image still writable, after every other code patch): the memory
 * accesses of functions that keep data below rsp run from thunks with rsp 128 bytes lower.
 * BB_RED_ZONE=0 turns it off, BB_RED_ZONE=list prints each function and access. */
void red_zone_patch(unsigned char *image, uint64_t image_size, const RedZoneSegment *segments, size_t count);
/* 1 when `address` is in a thunk: *site is the guest offset of the instructions it runs. */
int red_zone_site(uint64_t address, uint64_t *site);
/* The exception handler resolved a fault at `rip` (a guest page tracked by the GPU code). */
void red_zone_fault_handled(uint64_t rip);
#endif
