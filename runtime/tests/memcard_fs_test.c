/* Exercises the native BIOS memory-card file system the way libmcrd does (create, async
 * write, close, firstfile/nextfile, reopen, read) and validates the raw card image format. */
#include "../psx.h"
#include <stdio.h>

/* stubs for runtime pieces not needed here */
const ModuleDesc g_modules[1];
const int g_num_modules = 0;
Config g_cfg;
int g_log_level = LOG_WARN;
volatile int g_quit;
uint16_t g_pad_buttons[2];
bool g_pad_connected[2];
void log_msg(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void fatal(const char *fmt, ...) { (void)fmt; printf("FATAL\n"); exit(1); }
void platform_frame(void) {}
void platform_audio_push(const int16_t *s, int n) { (void)s; (void)n; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put_str(uint32_t a, const char *s) { do { MEM_W8(a++, (uint8_t)*s); } while (*s++); }
static uint32_t B(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    g_cpu.r[4] = a0; g_cpu.r[5] = a1; g_cpu.r[6] = a2;
    bios_call(&g_cpu, 0xB0, fn);
    return g_cpu.r[2];
}

int main(void) {
    remove("test_card.mcd");
    memcard_load(0, "test_card.mcd");
    bios_init();
    uint8_t *card = memcard_data(0);
    CHECK(card && card[0] == 'M' && card[1] == 'C');

    const uint32_t NAME = 0x80100000, BUF = 0x80110000, DIRENT = 0x80120000;
    put_str(NAME, "bu00:BASLUS-01115-FARM0");
    /* create a 2-block file */
    int32_t fd = (int32_t)B(0x32, NAME, 0x200 | (2u << 16), 0);
    CHECK(fd >= 2);
    CHECK((int32_t)B(0x36, (uint32_t)fd, 0, 0) == fd);
    /* create again must fail (exists) */
    CHECK((int32_t)B(0x32, NAME, 0x200 | (1u << 16), 0) == -1);

    /* write 0x2000+0x80 bytes asynchronously across the block boundary */
    for (uint32_t i = 0; i < 0x2080; i++) MEM_W8(BUF + i, (uint8_t)(i * 7 + 3));
    fd = (int32_t)B(0x32, NAME, 0x8002, 0);
    CHECK(fd >= 2);
    CHECK(B(0x35, (uint32_t)fd, BUF, 0x2080) == 0);   /* async: accepted */
    B(0x36, (uint32_t)fd, 0, 0);

    /* directory frames: first 0x51 with size 0x4000, then 0x53, valid checksums, linked */
    int first = -1, second = -1;
    for (int b = 1; b < 16; b++) {
        uint8_t *f = card + b * 128;
        uint8_t x = 0;
        for (int i = 0; i < 127; i++) x ^= f[i];
        CHECK(x == f[127]);
        if (f[0] == 0x51) first = b;
        if (f[0] == 0x53) second = b;
    }
    CHECK(first == 1 && second == 2);
    uint32_t sz; memcpy(&sz, card + first * 128 + 4, 4);
    CHECK(sz == 0x4000);
    CHECK(card[first * 128 + 8] == second - 1 && card[first * 128 + 9] == 0);
    CHECK(!strcmp((char *)card + first * 128 + 10, "BASLUS-01115-FARM0"));
    /* data landed in block 1 and block 2 */
    CHECK(card[0x2000 + 5] == (uint8_t)(5 * 7 + 3));
    CHECK(card[0x4000 + 0x10] == (uint8_t)((0x2010 * 7 + 3) & 0xFF));

    /* firstfile / nextfile */
    put_str(NAME + 0x100, "bu00:BASLUS-01115*");
    CHECK(B(0x42, NAME + 0x100, DIRENT, 0) == DIRENT);
    CHECK(MEM_R32(DIRENT + 0x18) == 0x4000);
    char nm[24]; for (int i = 0; i < 20; i++) nm[i] = (char)MEM_R8(DIRENT + (uint32_t)i); nm[20] = 0;
    CHECK(!strcmp(nm, "BASLUS-01115-FARM0"));
    CHECK(B(0x43, DIRENT, 0, 0) == 0);

    /* read back with seek */
    fd = (int32_t)B(0x32, NAME, 0x8001, 0);
    CHECK(B(0x33, (uint32_t)fd, 0x2000, 0) == 0x2000);
    CHECK(B(0x34, (uint32_t)fd, BUF + 0x10000, 0x80) == 0);   /* async: accepted */
    CHECK(MEM_R8(BUF + 0x10000 + 0x10) == (uint8_t)((0x2010 * 7 + 3) & 0xFF));
    B(0x36, (uint32_t)fd, 0, 0);

    /* synchronous read returns the byte count; misaligned reads are rejected */
    fd = (int32_t)B(0x32, NAME, 0x0001, 0);
    CHECK(B(0x34, (uint32_t)fd, BUF + 0x10000, 0x80) == 0x80);
    CHECK((int32_t)B(0x34, (uint32_t)fd, BUF, 0x40) == -1);
    B(0x36, (uint32_t)fd, 0, 0);

    /* rename + persistence */
    put_str(NAME + 0x200, "bu00:BASLUS-01115-FARM1");
    CHECK(B(0x44, NAME, NAME + 0x200, 0) == 1);
    memcard_flush();
    FILE *f = fopen("test_card.mcd", "rb");
    uint8_t img[0x20000];
    CHECK(f && fread(img, 1, sizeof img, f) == sizeof img);
    if (f) fclose(f);
    CHECK(!strcmp((char *)img + 128 + 10, "BASLUS-01115-FARM1"));
    remove("test_card.mcd");
    printf(fails ? "%d FAILURES\n" : "memcard fs: all checks passed\n", fails);
    return fails != 0;
}
