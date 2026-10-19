/* runtime_display_patch on a synthetic image: the display configuration init function (as in the games),
 * the memory budget table and the validation constant. Build: see tests/test_display.py. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)
static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put_entry(unsigned char *p, uint32_t id, uint32_t type, uint64_t mb) {
    put32(p, id); put32(p + 4, type);
    uint64_t size = mb << 20, alignment = 0x100000;
    memcpy(p + 8, &size, 8); memcpy(p + 16, &alignment, 8);
}

int main(void) {
    enum { SIZE = 0x4000, CODE = 0x100, TABLE = 0x1000, TOTAL = 0x800 };
    unsigned char *image = calloc(1, SIZE);
    /* mov dword [rax+disp8], imm32 */
    const struct { unsigned char disp; uint32_t imm; } stores[] = {
        {0x08, 1920}, {0x0c, 1080}, {0x14, 0}, {0x18, 1920}, {0x1c, 1080}, {0x20, 0x3fe38e39}, {0x24, 1920}, {0x28, 1080},
        {0x2c, 0x3fe38e39}, {0x10, 0x3f800000}, {0x40, 0xf0}, {0x44, 0x10e}, {0x30, 0xa00}, {0x34, 0x4c0}, {0x38, 1920}, {0x3c, 1080},
    };
    unsigned char *p = image + CODE;
    for (size_t i = 0; i < sizeof(stores) / sizeof(*stores); ++i, p += 7) {
        p[0] = 0xc7; p[1] = 0x40; p[2] = stores[i].disp; put32(p + 3, stores[i].imm);
    }
    /* movabs rdx, 0x120000000 */
    static const unsigned char total[10] = {0x48, 0xba, 0, 0, 0, 0x20, 1, 0, 0, 0};
    memcpy(image + TOTAL, total, sizeof(total));
    put_entry(image + TABLE, 3, 2, 2988);
    put_entry(image + TABLE + 32, 132, 6, 140);
    put_entry(image + TABLE + 64, 133, 6, 497);
    put_entry(image + TABLE + 96, 14, 6, 177);

    setenv("BB_GAME_RES", "2561x1441", 1);       /* odd numbers are made even */
    setenv("BB_GAME_RT_MB", "256", 1);
    setenv("BB_GAME_RT2_MB", "64", 1);
    setenv("BB_GAME_FREE_MB", "100", 1);
    runtime_display_patch(image, SIZE);

    CHECK(get32(image + CODE + 3) == 2560);       /* [+8] width */
    CHECK(get32(image + CODE + 7 + 3) == 1440);   /* [+0xc] height */
    CHECK(get32(image + CODE + 3 * 7 + 3) == 2560 && get32(image + CODE + 4 * 7 + 3) == 1440);
    CHECK(get32(image + CODE + 6 * 7 + 3) == 2560 && get32(image + CODE + 7 * 7 + 3) == 1440);
    CHECK(get32(image + CODE + 14 * 7 + 3) == 2560 && get32(image + CODE + 15 * 7 + 3) == 1440);
    float aspect; uint32_t bits = get32(image + CODE + 5 * 7 + 3); memcpy(&aspect, &bits, 4);
    CHECK(aspect > 1.77f && aspect < 1.78f);
    CHECK(get32(image + CODE + 10 * 7 + 3) == 320 && get32(image + CODE + 11 * 7 + 3) == 360);   /* 2560/8, 1440/4 */
    CHECK(get32(image + CODE + 12 * 7 + 3) == 0xa00);                                         /* untouched */
    CHECK((get64(image + TABLE + 8) >> 20) == 2988 + 220);          /* pool: 256 + 64 - 100 */
    CHECK((get64(image + TABLE + 32 + 8) >> 20) == 140 + 64);
    CHECK((get64(image + TABLE + 64 + 8) >> 20) == 497 + 256);
    CHECK((get64(image + TABLE + 96 + 8) >> 20) == 77);
    CHECK((get64(image + TOTAL + 2) >> 20) == 4608 + 220);

    /* The default size changes nothing. */
    unsigned char *same = calloc(1, SIZE);
    memcpy(same, image, SIZE);
    setenv("BB_GAME_RES", "1920x1080", 1);
    runtime_display_patch(same, SIZE);
    CHECK(!memcmp(same, image, SIZE));
    puts("display patch ok");
    return 0;
}
