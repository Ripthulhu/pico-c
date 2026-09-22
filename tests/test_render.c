#include "pico_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define TW PICO_RENDER_MAX_WIDTH
#define XUNIT (550 * 65536 / TW)

static void w16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void w32(unsigned char *p, unsigned v) {
    w16(p, v);
    w16(p + 2, v >> 16);
}
static unsigned char pack[588];
static unsigned char pack32[88];
static unsigned char pack3[PICO_RGBA_MAX_SOURCE_WIDTH * 4 + 512];
static void fixture(void) {
    unsigned char *r = pack + 32;
    memset(pack, 0, sizeof(pack));
    memcpy(pack, "PCTA", 4);
    w16(pack + 4, 1);
    w16(pack + 6, 1);
    w16(pack + 8, 1);
    w16(pack + 10, 1);
    w32(pack + 12, 32);
    w32(pack + 16, 56);
    w16(r, 7);
    w16(r + 2, 0);
    w16(r + 4, 65535);
    w16(r + 6, 65535);
    w16(r + 8, 2);
    w16(r + 10, 2);
    w32(r + 12, 568);
    w32(r + 16, 580);
    w32(r + 20, 588);
    w16(pack + 56 + 2, 0xf800);
    w16(pack + 56 + 4, 0x07e0);
    w16(pack + 56 + 6, 0x001f);
    w32(pack + 568, 580);
    w32(pack + 572, 584);
    w32(pack + 576, 588);
    pack[580] = 1;
    pack[581] = 1;
    pack[582] = 1;
    pack[583] = 2;
    pack[584] = 1;
    pack[585] = 3;
    pack[586] = 1;
    pack[587] = 0;
}
static void fixture32(void) {
    unsigned char *r = pack32 + 32;
    static const unsigned char pixels[20] = {
        1, 19, 83, 177, 255, 1, 200, 40, 100, 128,
        1, 30, 210, 80, 127, 1, 255, 100, 40, 0
    };
    memset(pack32, 0, sizeof(pack32));
    memcpy(pack32, "PCTA", 4);
    w16(pack32 + 4, 2);
    w16(pack32 + 6, 1);
    w16(pack32 + 8, 1);
    w16(pack32 + 10, 1);
    w32(pack32 + 12, 32);
    w16(r, 7);
    w16(r + 4, 65535);
    w16(r + 6, 65535);
    w16(r + 8, 2);
    w16(r + 10, 2);
    w32(r + 12, 56);
    w32(r + 16, 68);
    w32(r + 20, 88);
    w32(pack32 + 56, 68);
    w32(pack32 + 60, 78);
    w32(pack32 + 64, 88);
    memcpy(pack32 + 68, pixels, sizeof(pixels));
}
static PicoDraw draw(void) {
    PicoDraw d;
    unsigned i;
    memset(&d, 0, sizeof(d));
    d.a = XUNIT;
    d.d = 65536;
    d.tx = 10 * XUNIT;
    d.ty = 10 * 65536;
    d.symbol = 7;
    for (i = 0; i < 4; i++)
        d.multiply[i] = 256;
    return d;
}
static unsigned calls;
static void capture(void *u, unsigned y, const uint16_t *row, unsigned width) {
    (void)u;
    (void)row;
    assert(width == 96);
    assert(y == calls);
    calls++;
}
static void capture32(void *u, unsigned y, const uint32_t *row, unsigned width) {
    unsigned x;
    (void)u;
    assert(width == 96);
    assert(y == calls);
    for (x = 0; x < width; x++)
        assert(!(row[x] & 0xff000000u));
    calls++;
}
static void test_rgba(void) {
    PicoAssets a;
    PicoDraw d = draw();
    uint32_t guarded[TW + 2], *row = guarded + 1;
    uint16_t row16[TW];
    unsigned n;
    fixture32();
    assert(pico_assets_init(&a, pack32, sizeof(pack32)));
    assert(a.version == 2 && a.palette_offset == 0);
    guarded[0] = 0xdeadbeefu;
    guarded[TW + 1] = 0xdecaf00du;
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x001450a0, row));
    assert(row[8] == 0x001450a0 && row[11] == 0x001450a0);
    assert(row[9] == 0x001353b1);
    assert(row[10] == 0x006e3c82);
    assert(pico_raster_blit_row32(&a, &d, TW, 350, 9, row));
    assert(row[10] == 0x009b3273);
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 10, 0x001450a0, row));
    assert(row[9] == 0x00199178 && row[10] == 0x001450a0);
    row[8] = 0x00817263;
    assert(pico_raster_blit_row32(&a, &d, TW, 350, 9, row));
    assert(row[8] == 0x00817263 && row[9] == 0x001353b1);
    assert(pico_asset_hit(&a, 7, 0, -32768, -32768));
    assert(pico_asset_hit(&a, 7, 0, 32768, -32768));
    assert(!pico_asset_hit(&a, 7, 0, -32768, 32768));
    assert(!pico_asset_hit(&a, 7, 0, 32768, 32768));
    d.multiply[0] = 128;
    d.add[0] = 7;
    d.add[1] = 20;
    d.multiply[2] = 0;
    d.add[2] = 99;
    d.multiply[3] = 128;
    d.add[3] = 16;
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x001450a0, row));
    assert(row[10] == 0x002f4a8d);
    d = draw();
    d.multiply[3] = 0;
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x001450a0, row));
    assert(row[9] == 0x001450a0 && row[10] == 0x001450a0);
    d.multiply[3] = -256;
    d.add[3] = 255;
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x001450a0, row));
    assert(row[9] == 0x001450a0 && row[10] == 0x006e3c82);
    d = draw();
    assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0, row16));
    assert(row16[9] == (uint16_t)((19 >> 3) << 11 | (83 >> 2) << 5 | (177 >> 3)));
    assert(guarded[0] == 0xdeadbeefu && guarded[TW + 1] == 0xdecaf00du);
    assert(!pico_raster_row32(&a, &d, 1, TW + 1, 350, 9, 0, row));
    assert(!pico_raster_row32(&a, &d, 1, TW, 0, 0, 0, row));
    assert(!pico_raster_row32(&a, &d, 1, TW, 350, 350, 0, row));
    assert(!pico_raster_row32(&a, 0, 1, TW, 350, 9, 0, row));
    assert(!pico_raster_row32(&a, &d, 1, TW, 350, 9, 0, 0));
    assert(pico_raster_row32(&a, 0, 0, TW, 350, 9, 0xff123456u, row));
    assert(row[0] == 0x00123456 && row[TW - 1] == 0x00123456);
    calls = 0;
    assert(pico_raster_render32(&a, &d, 1, 96, 64, 0, capture32, 0));
    assert(calls == 64);
    assert(!pico_raster_render32(&a, &d, 1, 96, 64, 0, 0, 0));
    for (n = 0; n < sizeof(pack32); n++)
        assert(!pico_assets_init(&a, pack32, n));
    pack32[68] = 0;
    assert(!pico_assets_init(&a, pack32, sizeof(pack32)));
    fixture32();
    pack32[68] = 3;
    assert(!pico_assets_init(&a, pack32, sizeof(pack32)));
    fixture32();
    w32(pack32 + 60, 77);
    assert(!pico_assets_init(&a, pack32, sizeof(pack32)));
    fixture32();
    w32(pack32 + 16, 32);
    assert(!pico_assets_init(&a, pack32, sizeof(pack32)));
    fixture32();
    w16(pack32 + 4, 4);
    assert(!pico_assets_init(&a, pack32, sizeof(pack32)));
    assert(!a.data && !a.version);
    fixture();
    assert(pico_assets_init(&a, pack, sizeof(pack)));
    assert(a.version == 1);
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0xffffff, row));
    assert(row[9] == 0xff0000 && row[10] == 0x00ff00);
}
static unsigned fixture3_bytes(unsigned width, const unsigned char *bytes, unsigned size) {
    unsigned char *r = pack3 + 32;
    memset(pack3, 0, sizeof(pack3));
    memcpy(pack3, "PCTA", 4);
    w16(pack3 + 4, 3);
    w16(pack3 + 6, 1);
    w16(pack3 + 8, 1);
    w16(pack3 + 10, 1);
    w32(pack3 + 12, 32);
    w16(r, 7);
    w16(r + 4, 65535);
    w16(r + 6, 65535);
    w16(r + 8, width);
    w16(r + 10, 1);
    w32(r + 12, 56);
    w32(r + 16, 64);
    w32(r + 20, 64 + size);
    w32(pack3 + 56, 64);
    w32(pack3 + 60, 64 + size);
    if (size)
        memcpy(pack3 + 64, bytes, size);
    return 64 + size;
}
static unsigned fixture3(unsigned mode) {
    static const unsigned char rgba[8] = {19, 83, 177, 255, 200, 40, 100, 128};
    unsigned char bytes[16], transformed[8];
    unsigned i, channel, at = 0;
    bytes[at++] = (unsigned char)mode;
    if (mode == 1) {
        for (i = 0; i < 2; i++) {
            bytes[at++] = 1;
            memcpy(bytes + at, rgba + i * 4, 4);
            at += 4;
        }
    } else {
        for (i = 0; i < 2; i++)
            for (channel = 0; channel < 4; channel++) {
                unsigned index = mode == 3 || mode == 5 ? channel * 2 + i : i * 4 + channel;
                transformed[index] = (unsigned char)(rgba[i * 4 + channel] -
                                      (mode >= 4 && i ? rgba[(i - 1) * 4 + channel] : 0));
            }
        if (mode >= 2)
            bytes[at++] = 0x80;
        memcpy(bytes + at, transformed, 8);
        at += 8;
    }
    return fixture3_bytes(2, bytes, at);
}
static void test_lossless_rows(void) {
    static const unsigned char overlap[] = {2, 0x1f, 0xa5, 1, 0, 7, 0x50,
                                            0xa5, 0xa5, 0xa5, 0xa5, 0xa5};
    static const unsigned char invalid[][12] = {
        {6},                              /* Unknown mode. */
        {0, 1, 2, 3},                    /* Incomplete raw pixel. */
        {1, 0, 1, 2, 3, 4},              /* Empty run. */
        {1, 3, 1, 2, 3, 4},              /* Run exceeds row width. */
        {1, 2, 1, 2, 3},                 /* Truncated RGBA run. */
        {2, 0xf0},                       /* Missing literal length. */
        {2, 0xf0, 255, 255},             /* Length exceeds row size. */
        {2, 0x80, 1, 2, 3},              /* Truncated literals. */
        {2, 0x10, 1, 0, 0},              /* Zero back-reference. */
        {2, 0x10, 1, 2, 0},              /* Back-reference before row. */
        {2, 0x10, 1, 1},                 /* Truncated back-reference. */
        {2, 0x1f, 1, 1, 0},              /* Match exceeds row size. */
        {2, 0x13, 1, 1, 0},              /* Missing final literal sequence. */
        {2, 0x70, 1, 2, 3, 4, 5, 6, 7}, /* Decoded row too short. */
        {2, 0x90, 1, 2, 3, 4, 5, 6, 7, 8, 9}
    };
    static const unsigned lengths[] = {1, 4, 6, 6, 5, 2, 4, 5, 5, 5, 4, 5, 5, 9, 11};
    PicoAssets a;
    PicoDraw d = draw();
    uint32_t guarded[TW + 2], *row = guarded + 1;
    uint16_t row16[TW];
    unsigned char bytes[PICO_RGBA_MAX_SOURCE_WIDTH * 4 + 128];
    unsigned mode, size, i, at;
    uint32_t random = 0x719ada73u;
    for (mode = 0; mode <= 5; mode++) {
        size = fixture3(mode);
        assert(pico_assets_init(&a, pack3, size));
        assert(a.version == 3);
        guarded[0] = 0xdeadbeefu;
        guarded[TW + 1] = 0xdecaf00du;
        assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x001450a0, row));
        assert(row[9] == 0x001353b1 && row[10] == 0x006e3c82);
        assert(pico_raster_blit_row32(&a, &d, TW, 350, 9, row));
        assert(row[10] == 0x009b3273);
        assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0, row16));
        assert(row16[9] == (uint16_t)((19 >> 3) << 11 | (83 >> 2) << 5 | (177 >> 3)));
        assert(pico_asset_hit(&a, 7, 0, -32768, -32768));
        assert(pico_asset_hit(&a, 7, 0, 32768, -32768));
        assert(!pico_asset_hit(&a, 7, 0, 32768, 32768));
        d.a = -XUNIT;
        assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x001450a0, row));
        assert(row[9] == 0x006e3c82 && row[10] == 0x001353b1);
        d = draw();
        assert(guarded[0] == 0xdeadbeefu && guarded[TW + 1] == 0xdecaf00du);
        for (i = 0; i < size; i++)
            assert(!pico_assets_init(&a, pack3, i));
    }
    size = fixture3_bytes(8, overlap, sizeof(overlap));
    assert(pico_assets_init(&a, pack3, size));
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0, row));
    assert(row[9] == 0x006b6b6b && row[16] == 0x006b6b6b);
    memcpy(bytes, overlap, sizeof(overlap));
    bytes[5] = 255;
    size = fixture3_bytes(8, bytes, sizeof(overlap));
    assert(!pico_assets_init(&a, pack3, size));
    size = fixture3_bytes(8, overlap, 5);
    assert(!pico_assets_init(&a, pack3, size));
    bytes[0] = 2;
    bytes[1] = 0xf0;
    bytes[2] = 255;
    bytes[3] = 50;
    for (i = 0; i < 320; i++)
        bytes[4 + i] = (unsigned char)i;
    size = fixture3_bytes(80, bytes, 324);
    assert(pico_assets_init(&a, pack3, size));
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        size = fixture3_bytes(2, invalid[i], lengths[i]);
        assert(!pico_assets_init(&a, pack3, size));
    }
    size = fixture3_bytes(2, bytes, 0);
    assert(!pico_assets_init(&a, pack3, size));
    bytes[0] = 0;
    for (i = 0; i < PICO_RGBA_MAX_SOURCE_WIDTH * 4; i++)
        bytes[1 + i] = i % 4 == 3 ? 255 : 73;
    size = fixture3_bytes(PICO_RGBA_MAX_SOURCE_WIDTH, bytes, 1 + PICO_RGBA_MAX_SOURCE_WIDTH * 4);
    assert(pico_assets_init(&a, pack3, size));
    assert(pico_asset_hit(&a, 7, 0, PICO_RGBA_MAX_SOURCE_WIDTH * 65536 - 98304, -32768));
#if PICO_RGBA_MAX_SOURCE_WIDTH < 65535
    w16(pack3 + 40, PICO_RGBA_MAX_SOURCE_WIDTH + 1);
    assert(!pico_assets_init(&a, pack3, size));
#endif
    for (i = 0; i < 10000; i++) {
        unsigned width, length;
        random = random * 1664525u + 1013904223u;
        width = 1 + (random & 31);
        length = (random >> 8) & 127;
        for (at = 0; at < length; at++) {
            random = random * 1664525u + 1013904223u;
            bytes[at] = (unsigned char)(random >> 24);
        }
        if (length)
            bytes[0] %= 6;
        size = fixture3_bytes(width, bytes, length);
        if (pico_assets_init(&a, pack3, size))
            assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0, row));
    }
}
static void test_cache(void) {
    PicoAssets a;
    PicoAssetCache cache;
    PicoDraw d = draw();
    unsigned char arena[18];
    uint32_t expected[TW], actual[TW];
    uint16_t expected16[TW], actual16[TW];
    unsigned mode, size, i;
    d.b = 16384;
    for (mode = 0; mode <= 5; mode++) {
        size = fixture3(mode);
        assert(pico_assets_init(&a, pack3, size));
        assert(!a.cache);
        assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, expected));
        assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0x1494, expected16));
        memset(arena, 0x5a, sizeof(arena));
        assert(pico_assets_bind_cache(&a, &cache, arena + 1, 8));
        assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
        assert(!memcmp(actual, expected, sizeof(actual)));
        assert(cache.used == 8 && cache.count == 1 && cache.capacity == 8);
        assert(arena[0] == 0x5a && arena[9] == 0x5a);
        assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0x1494, actual16));
        assert(!memcmp(actual16, expected16, sizeof(actual16)));
        assert(pico_asset_hit(&a, 7, 0, -32768, -32768));
        assert(!pico_asset_hit(&a, 7, 0, -32768, 32768));
        for (i = 0; i < TW; i++) {
            actual[i] = 0x1450a0;
            actual16[i] = 0x1494;
        }
        assert(pico_raster_blit_row32(&a, &d, TW, 350, 9, actual));
        assert(pico_raster_blit_row(&a, &d, TW, 350, 9, actual16));
        assert(!memcmp(actual, expected, sizeof(actual)));
        assert(!memcmp(actual16, expected16, sizeof(actual16)));
        pico_assets_reset_cache(&a);
        assert(!cache.used && !cache.count);
        assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
        assert(!memcmp(actual, expected, sizeof(actual)));
        assert(cache.used == 8 && cache.count == 1);
        assert(pico_assets_bind_cache(&a, &cache, arena + 1, 7));
        assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
        assert(!memcmp(actual, expected, sizeof(actual)));
        assert(!cache.used && !cache.count);
        assert(pico_assets_bind_cache(&a, &cache, arena + 1, 16));
        cache.count = PICO_ASSET_CACHE_ENTRIES;
        memset(cache.entries, 0, sizeof(cache.entries));
        assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
        assert(!memcmp(actual, expected, sizeof(actual)) && !cache.used);
        pico_assets_reset_cache(&a);
        d.b = 0;
        assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
        assert(!cache.used && !cache.count);
        d.b = 16384;
        assert(pico_assets_bind_cache(&a, 0, 0, 0));
        assert(!a.cache);
        assert(!pico_assets_bind_cache(&a, &cache, 0, 8));
        assert(!pico_assets_bind_cache(&a, &cache, arena, 0));
    }
    size = fixture3(0);
    memmove(pack3 + 88, pack3 + 64, size - 64);
    size += 24;
    memcpy(pack3 + 56, pack3 + 32, 24);
    w16(pack3 + 6, 2);
    w16(pack3 + 56, 8);
    for (i = 0; i < 2; i++) {
        w32(pack3 + 32 + i * 24 + 12, 80);
        w32(pack3 + 32 + i * 24 + 16, 88);
        w32(pack3 + 32 + i * 24 + 20, size);
    }
    w32(pack3 + 80, 88);
    w32(pack3 + 84, size);
    assert(pico_assets_init(&a, pack3, size));
    assert(pico_assets_bind_cache(&a, &cache, arena + 1, 8));
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, expected));
    assert(cache.used == 8 && cache.count == 1);
    d.symbol = 8;
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
    assert(!memcmp(actual, expected, sizeof(actual)));
    assert(cache.used == 8 && cache.count == 1 && cache.entries[0].record == pack3 + 32);
    pico_assets_reset_cache(&a);
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
    assert(!memcmp(actual, expected, sizeof(actual)) && cache.entries[0].record == pack3 + 56);
    d.symbol = 7;
    assert(pico_assets_bind_cache(&a, &cache, arena + 1, 16));
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
    assert(cache.count == 1);
    fixture32();
    assert(pico_assets_init(&a, pack32, sizeof(pack32)));
    assert(!a.cache && a.version == 2);
    assert(pico_assets_bind_cache(&a, &cache, arena + 1, 16));
    assert(!cache.count && !cache.used && cache.source == pack32);
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
    assert(!cache.count && !cache.used);
    size = fixture3(0);
    pack3[66] = 12;
    assert(pico_assets_init(&a, pack3, size));
    assert(!a.cache);
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, expected));
    assert(pico_assets_bind_cache(&a, &cache, arena + 1, 16));
    assert(pico_raster_row32(&a, &d, 1, TW, 350, 9, 0x1450a0, actual));
    assert(!memcmp(actual, expected, sizeof(actual)));
    calls = 0;
    assert(pico_raster_render32(&a, 0, 0, 96, 64, 0, capture32, 0));
    assert(calls == 64 && !cache.used && !cache.count);
    assert(!pico_assets_init(&a, pack3, 1));
    assert(!a.cache);
}
int main(void) {
    PicoAssets a;
    PicoDraw d = draw();
    uint16_t row[TW];
    int32_t x, y;
    uint8_t mono[2];
    unsigned n;
    fixture();
    assert(pico_assets_init(&a, pack, sizeof(pack)));
    assert(pico_assets_count(&a) == 1);
    assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0xffff, row));
    assert(row[8] == 0xffff);
    assert(row[9] == 0xf800);
    assert(row[10] == 0x07e0);
    assert(row[11] == 0xffff);
    assert(pico_raster_row(&a, &d, 1, TW, 350, 10, 0xffff, row));
    assert(row[9] == 0x001f);
    assert(row[10] == 0xffff);
    row[8] = 0x1234;
    assert(pico_raster_blit_row(&a, &d, TW, 350, 9, row));
    assert(row[8] == 0x1234);
    assert(row[9] == 0xf800);
    assert(pico_asset_hit(&a, 7, 0, -32768, -32768));
    assert(!pico_asset_hit(&a, 7, 0, 32768, 32768));
    assert(!pico_asset_hit(&a, 999, 0, 0, 0));
    d.a = -XUNIT;
    assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0, row));
    assert(row[9] == 0x07e0);
    assert(row[10] == 0xf800);
    d.a = d.d = 0;
    d.b = 65536;
    d.c = -XUNIT;
    assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0, row));
    assert(row[9] == 0x001f);
    assert(row[10] == 0xf800);
    assert(pico_draw_inverse(&d, 9 * XUNIT + XUNIT / 2, 9 * 65536 + 32768, &x, &y));
    assert(x == -32768 && y >= 32760 && y <= 32776);
    d = draw();
    d.multiply[3] = 0;
    assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0xffff, row));
    assert(row[9] == 0xffff);
    d = draw();
    d.multiply[3] = 128;
    assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0, row));
    assert((row[9] >> 11) == 15);
    d = draw();
    d.multiply[0] = 0;
    d.add[2] = 255;
    assert(pico_raster_row(&a, &d, 1, TW, 350, 9, 0, row));
    assert(row[9] == 0x001f);
    d = draw();
    calls = 0;
    assert(pico_raster_render(&a, &d, 1, 96, 64, 0, capture, 0));
    assert(calls == 64);
    for (n = 0; n < 9; n++)
        row[n] = n < 4 ? 0 : 0xffff;
    pico_rgb565_to_mono(row, 9, 0, mono);
    assert(mono[0] == 15 && mono[1] == 128);
    for (n = 0; n < sizeof(pack); n++)
        assert(!pico_assets_init(&a, pack, n));
    pack[580] = 0;
    assert(!pico_assets_init(&a, pack, sizeof(pack)));
    fixture();
    pack[580] = 3;
    assert(!pico_assets_init(&a, pack, sizeof(pack)));
    fixture();
    w32(pack + 572, 583);
    assert(!pico_assets_init(&a, pack, sizeof(pack)));
    fixture();
    assert(pico_assets_init(&a, pack, sizeof(pack)));
    d = draw();
    d.a = d.d = 0;
    assert(!pico_draw_inverse(&d, 0, 0, &x, &y));
    test_rgba();
    test_lossless_rows();
    test_cache();
    puts("renderer tests: origins, reflection, rotation, RGB565/RGBA, soft alpha, hit, scanlines, 1bpp, "
         "lossless row modes, bounded cache, malformed/truncated packs passed");
    return 0;
}
