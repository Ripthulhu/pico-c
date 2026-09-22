#include "pico_render.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Pixel hashes were captured with the renderer at 329726ad379ab270b487e7441e776f1733d2fe9e.
 * The fixtures include samples close to affine rounding boundaries. Comparing
 * only two current render paths would miss a regression shared by both. */
#define MAX_WIDTH 127
#define MAX_HEIGHT 83
#define FRAMES 24
#define BACKGROUND32 0x003a719bu
#define BACKGROUND16 0x3b93u

static uint8_t pack[8192];
static uint8_t arena[2048];
static uint16_t pixels16[MAX_WIDTH * MAX_HEIGHT];
static uint32_t pixels32[MAX_WIDTH * MAX_HEIGHT];
static unsigned output_width, output_height, output_rows;

typedef struct Golden {
    uint64_t rgb565, rgb888;
} Golden;

#ifndef PICO_WRITE_GOLDENS
/* Indexed colours differ; every RGBA row encoding represents the same pixels. */
static const Golden goldens[2][3][2] = {
    {{{UINT64_C(0x3c5a181f3c1a82a9), UINT64_C(0x08619756d1b2ff8e)},
      {UINT64_C(0x9ba70b7dfc2d892c), UINT64_C(0x6f4ef8882cc85688)}},
     {{UINT64_C(0xe936c9a23ce9fd4e), UINT64_C(0x4e24c7a3d5a92b23)},
      {UINT64_C(0xf504116b6649af00), UINT64_C(0x0a999216b806c1aa)}},
     {{UINT64_C(0x1172430383986d17), UINT64_C(0x8824eb3a1bcb32e7)},
      {UINT64_C(0xbfb3778f44392ed1), UINT64_C(0x6e933ae3e7b41430)}}},
    {{{UINT64_C(0xc548162a95c15d5a), UINT64_C(0xa0e4612441e42c01)},
      {UINT64_C(0x8916fe2d0df90d2a), UINT64_C(0x80702df4353b50dc)}},
     {{UINT64_C(0x84c829dc621609a4), UINT64_C(0x4a6ab1beb10db405)},
      {UINT64_C(0x3ffcaa64f8b2c1bb), UINT64_C(0xcb3e4244ba7e9615)}},
     {{UINT64_C(0xa403831146cf59f8), UINT64_C(0xeb8db4ceb56f8bb4)},
      {UINT64_C(0x2986c24515843911), UINT64_C(0xd661385116b61c28)}}}};
#endif

static void w16(uint8_t *p, unsigned value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void w32(uint8_t *p, unsigned value) {
    w16(p, value);
    w16(p + 2, value >> 16);
}

static uint16_t palette(unsigned index) {
    return (uint16_t)(((index * 7u & 31u) << 11) | ((index * 13u & 63u) << 5) |
                      (index * 19u & 31u));
}

static void pixel(unsigned leaf, unsigned x, unsigned y, uint8_t rgba[4]) {
    static const uint8_t alpha[] = {0, 1, 63, 127, 128, 192, 254, 255};
    rgba[0] = (uint8_t)(19 + x * 83 + y * 37 + leaf * 53);
    rgba[1] = (uint8_t)(203 + x * 41 + y * 109 + leaf * 29);
    rgba[2] = (uint8_t)(71 + x * 131 + y * 17 + leaf * 7);
    rgba[3] = alpha[(x * 5 + y * 3 + leaf) % sizeof(alpha)];
}

static unsigned fixture(unsigned format, unsigned numerator, unsigned denominator) {
    static const unsigned widths[] = {13, 3, 11};
    static const unsigned heights[] = {7, 31, 9};
    unsigned version = format < 2 ? format + 1 : 3;
    unsigned mode = format < 2 ? 0 : format - 2;
    unsigned leaf, at = 32 + 3 * 24;
    memset(pack, 0, sizeof(pack));
    memcpy(pack, "PCTA", 4);
    w16(pack + 4, version);
    w16(pack + 6, 3);
    w16(pack + 8, numerator);
    w16(pack + 10, denominator);
    w32(pack + 12, 32);
    if (version == 1) {
        unsigned i;
        w32(pack + 16, at);
        for (i = 0; i < 256; i++)
            w16(pack + at + i * 2, palette(i));
        at += 512;
    }
    for (leaf = 0; leaf < 3; leaf++) {
        uint8_t *r = pack + 32 + leaf * 24;
        unsigned width = widths[leaf], height = heights[leaf], rows = at, y;
        w16(r, 7 + leaf);
        w16(r + 4, (unsigned)-6);
        w16(r + 6, (unsigned)-3);
        w16(r + 8, width);
        w16(r + 10, height);
        w32(r + 12, rows);
        at += 4 * (height + 1);
        w32(r + 16, at);
        for (y = 0; y < height; y++) {
            uint8_t raw[13 * 4], filtered[13 * 4];
            unsigned x, channel;
            w32(pack + rows + y * 4, at);
            for (x = 0; x < width; x++)
                pixel(leaf, x, y, raw + x * 4);
            if (version == 3)
                pack[at++] = (uint8_t)mode;
            if (version < 3 || mode == 1) {
                for (x = 0; x < width; x++) {
                    pack[at++] = 1;
                    if (version == 1)
                        pack[at++] = raw[x * 4 + 3] ? (uint8_t)(1 + (x * 7 + y * 5) % 255) : 0;
                    else {
                        memcpy(pack + at, raw + x * 4, 4);
                        at += 4;
                    }
                }
            } else {
                for (x = 0; x < width; x++)
                    for (channel = 0; channel < 4; channel++) {
                        unsigned index =
                            mode == 3 || mode == 5 ? channel * width + x : x * 4 + channel;
                        filtered[index] =
                            (uint8_t)(raw[x * 4 + channel] -
                                      (mode >= 4 && x ? raw[(x - 1) * 4 + channel] : 0));
                    }
                if (mode >= 2) {
                    unsigned length = width * 4;
                    pack[at++] = (uint8_t)((length < 15 ? length : 15) << 4);
                    if (length >= 15)
                        pack[at++] = (uint8_t)(length - 15);
                }
                memcpy(pack + at, filtered, width * 4);
                at += width * 4;
            }
        }
        w32(pack + rows + height * 4, at);
        w32(r + 20, at);
    }
    assert(at <= sizeof(pack));
    return at;
}

static void identity(PicoDraw *d, unsigned symbol, unsigned numerator, unsigned denominator) {
    unsigned i;
    memset(d, 0, sizeof(*d));
    d->symbol = (uint16_t)symbol;
    d->a = d->d = (int32_t)(65536u * numerator / denominator);
    for (i = 0; i < 4; i++)
        d->multiply[i] = 256;
}

static void scene(PicoDraw draws[3], unsigned frame, unsigned numerator, unsigned denominator) {
    static const int32_t matrices[][4] = {
        {65536, 0, 0, 65536},           {-65536, 0, 0, 65536},         {65536, 0, 0, -65536},
        {-65536, 0, 0, -65536},         {0, 65536, -65536, 0},         {0, -65536, 65536, 0},
        {46341, 46341, -46341, 46341},  {56756, -32768, 32768, 56756}, {65536, 32768, 16384, 65536},
        {65536, -16384, -49152, 65536}, {98303, 7, -3, 32769},         {257, 65536, -65536, -129}};
    static const int32_t positions[][2] = {
        {275 * 65536 + 1, 175 * 65536 - 1},         {12 * 65536 + 32767, 13 * 65536 + 32769},
        {540 * 65536 - 32769, 339 * 65536 - 32767}, {-14 * 65536 + 3, 164 * 65536 + 11},
        {299 * 65536 - 5, -8 * 65536 + 7},          {568 * 65536 - 1, 357 * 65536 + 1}};
    const int32_t *m = matrices[frame % 12], *position = positions[frame % 6];
    int64_t factor = (int64_t)(9 + frame % 4) * numerator;
    identity(draws, 9, numerator, denominator);
    draws[0].a *= 30;
    draws[0].d *= 21;
    draws[0].tx = 275 * 65536;
    draws[0].ty = 175 * 65536;
    identity(draws + 1, 7 + frame % 3, numerator, denominator);
    draws[1].a = (int32_t)(m[0] * factor / denominator);
    draws[1].b = (int32_t)(m[1] * factor / denominator);
    draws[1].c = (int32_t)(m[2] * factor / denominator);
    draws[1].d = (int32_t)(m[3] * factor / denominator);
    draws[1].tx = position[0];
    draws[1].ty = position[1];
    if (frame >= 12) {
        draws[1].multiply[0] = -128;
        draws[1].multiply[1] = 385;
        draws[1].multiply[2] = 127;
        draws[1].multiply[3] = frame % 2 ? -192 : 177;
        draws[1].add[0] = 201;
        draws[1].add[1] = -37;
        draws[1].add[2] = 73;
        draws[1].add[3] = frame % 2 ? 255 : -11;
    }
    draws[2] = draws[1];
    draws[2].symbol = 7 + (frame + 1) % 3;
    draws[2].tx += 19 * 65536 + 101;
    draws[2].ty += 11 * 65536 - 53;
    draws[2].multiply[3] = 113;
    draws[2].add[3] = 9;
}

static uint64_t hash_pixel(uint64_t hash, uint32_t pixel, unsigned bytes) {
    unsigned i;
    for (i = 0; i < bytes; i++)
        hash = (hash ^ (uint8_t)(pixel >> (i * 8))) * UINT64_C(1099511628211);
    return hash;
}

static void capture16(void *user, unsigned y, const uint16_t *row, unsigned width) {
    (void)user;
    assert(width == output_width && y == output_rows && y < output_height);
    assert(!memcmp(row, pixels16 + y * width, width * sizeof(*row)));
    output_rows++;
}

static void capture32(void *user, unsigned y, const uint32_t *row, unsigned width) {
    (void)user;
    assert(width == output_width && y == output_rows && y < output_height);
    assert(!memcmp(row, pixels32 + y * width, width * sizeof(*row)));
    output_rows++;
}

#if !defined(PICO_WRITE_GOLDENS) && !defined(PICO_REFERENCE_RENDERER)
static void check_framebuffers(PicoAssets *assets, const PicoDraw *draws, unsigned count,
                               unsigned width, unsigned height) {
    static uint16_t actual16[(MAX_WIDTH + 3) * MAX_HEIGHT + 2];
    static uint32_t actual32[(MAX_WIDTH + 3) * MAX_HEIGHT + 2];
    unsigned padding;
    for (padding = 0; padding <= 3; padding += 3) {
        size_t stride = width + padding, end = (height - 1) * stride + width;
        size_t i;
        unsigned x, y;
        for (i = 0; i < sizeof(actual16) / sizeof(actual16[0]); i++) {
            actual16[i] = 0xa53c;
            actual32[i] = 0x7da53c91;
        }
        assert(pico_raster_framebuffer(assets, draws, count, width, height, BACKGROUND16,
                                       actual16 + 1, stride));
        assert(pico_raster_framebuffer32(assets, draws, count, width, height, BACKGROUND32,
                                         actual32 + 1, stride));
        assert(actual16[0] == 0xa53c && actual32[0] == 0x7da53c91);
        for (y = 0; y < height; y++) {
            assert(!memcmp(actual16 + 1 + y * stride, pixels16 + y * width,
                           width * sizeof(actual16[0])));
            assert(!memcmp(actual32 + 1 + y * stride, pixels32 + y * width,
                           width * sizeof(actual32[0])));
            if (y + 1 < height)
                for (x = width; x < stride; x++) {
                    assert(actual16[1 + y * stride + x] == 0xa53c);
                    assert(actual32[1 + y * stride + x] == 0x7da53c91);
                }
        }
        for (i = end + 1; i < sizeof(actual16) / sizeof(actual16[0]); i++) {
            assert(actual16[i] == 0xa53c);
            assert(actual32[i] == 0x7da53c91);
        }
    }
}

static void check_regions(PicoAssets *assets, const PicoDraw *draws, unsigned count, unsigned width,
                          unsigned height) {
    static uint16_t actual16[(MAX_WIDTH + 3) * MAX_HEIGHT + 2];
    static uint32_t actual32[(MAX_WIDTH + 3) * MAX_HEIGHT + 2];
    PicoRect regions[] = {{width / 3, height / 4, width / 2, height / 2},
                          {width - 1, height - 1, 1, 1},
                          {0, 0, 1, height}};
    size_t stride = width + 3;
    unsigned which;
    for (which = 0; which < sizeof(regions) / sizeof(regions[0]); which++) {
        const PicoRect *r = regions + which;
        size_t i;
        for (i = 0; i < sizeof(actual16) / sizeof(actual16[0]); i++) {
            actual16[i] = 0xa53c;
            actual32[i] = 0x7da53c91;
        }
        assert(pico_raster_region(assets, draws, count, width, height, BACKGROUND16, r,
                                  actual16 + 1, stride));
        assert(pico_raster_region32(assets, draws, count, width, height, BACKGROUND32, r,
                                    actual32 + 1, stride));
        for (i = 0; i < sizeof(actual16) / sizeof(actual16[0]); i++) {
            size_t y = i ? (i - 1) / stride : height;
            size_t x = i ? (i - 1) % stride : width;
            int covered = x >= r->x && x - r->x < r->width && y >= r->y && y - r->y < r->height;
            assert(actual16[i] == (covered ? pixels16[y * width + x] : 0xa53c));
            assert(actual32[i] == (covered ? pixels32[y * width + x] : 0x7da53c91));
        }
    }
}

static void check_draw_bounds(PicoAssets *assets, const PicoDraw *draws, unsigned count,
                              unsigned width, unsigned height) {
    unsigned n;
    for (n = 0; n < count; n++) {
        PicoRect rect;
        unsigned x, y;
        int visible = pico_raster_draw_bounds(assets, draws + n, width, height, &rect);
        assert(visible >= 0);
        if (visible)
            assert(rect.x < width && rect.y < height && rect.width <= width - rect.x &&
                   rect.height <= height - rect.y);
        pico_assets_reset_cache(assets);
        for (y = 0; y < height; y++) {
            uint32_t row[MAX_WIDTH];
            assert(pico_raster_row32(assets, draws + n, 1, width, height, y, BACKGROUND32, row));
            for (x = 0; x < width; x++)
                if (row[x] != BACKGROUND32)
                    assert(visible && x >= rect.x && x - rect.x < rect.width && y >= rect.y &&
                           y - rect.y < rect.height);
        }
    }
}

static void test_vertical_cull(void) {
    static const int32_t vertical[] = {98303, -98303, 0, 257, -257};
    unsigned format, which, y;
    for (format = 0; format < 8; format++) {
        PicoAssets assets;
        unsigned bytes = fixture(format, 3, 2);
        assert(pico_assets_init(&assets, pack, bytes));
        for (which = 0; which < sizeof(vertical) / sizeof(vertical[0]); which++) {
            PicoDraw draws[3];
            unsigned i;
            for (i = 0; i < 3; i++) {
                identity(draws + i, 7 + i, 3, 2);
                draws[i].a *= 9;
                draws[i].c = (i % 2 ? -1 : 1) * 458753;
                draws[i].d = vertical[which] * 11;
                draws[i].tx = (int32_t)i * 160 * 65536 - 32767;
                draws[i].ty = (int32_t)i * 97 * 65536 - 32769;
            }
            /* Fractional bank scales and horizontal shear must keep the row
             * rejection identical to the full four-corner bounds path. */
            for (y = 0; y < MAX_HEIGHT; y++) {
                assert(pico_raster_row(&assets, draws, 3, MAX_WIDTH, MAX_HEIGHT, y,
                                       BACKGROUND16, pixels16 + y * MAX_WIDTH));
                assert(pico_raster_row32(&assets, draws, 3, MAX_WIDTH, MAX_HEIGHT, y,
                                         BACKGROUND32, pixels32 + y * MAX_WIDTH));
            }
            check_framebuffers(&assets, draws, 3, MAX_WIDTH, MAX_HEIGHT);
        }
    }
}

static void test_framebuffer_arguments(void) {
    PicoAssets assets, invalid_assets;
    PicoDraw draws[4];
    uint16_t out16[16];
    uint32_t out32[16];
    PicoRect region = {0, 0, 1, 1}, rect;
    unsigned bytes = fixture(2, 1, 1), i;
    assert(pico_assets_init(&assets, pack, bytes));
    memset(&invalid_assets, 0, sizeof(invalid_assets));
    identity(draws, 7, 1, 1);
    assert(!pico_raster_framebuffer(NULL, draws, 1, 1, 1, 0, out16, 1));
    assert(!pico_raster_framebuffer32(NULL, draws, 1, 1, 1, 0, out32, 1));
    assert(!pico_raster_framebuffer(&invalid_assets, draws, 1, 1, 1, 0, out16, 1));
    assert(!pico_raster_framebuffer32(&invalid_assets, draws, 1, 1, 1, 0, out32, 1));
    assert(!pico_raster_framebuffer(&assets, NULL, 1, 1, 1, 0, out16, 1));
    assert(!pico_raster_framebuffer32(&assets, NULL, 1, 1, 1, 0, out32, 1));
    assert(!pico_raster_framebuffer(&assets, draws, 1, 1, 1, 0, NULL, 1));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 1, 1, 0, NULL, 1));
    assert(!pico_raster_framebuffer(&assets, draws, 1, 0, 1, 0, out16, 1));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 0, 1, 0, out32, 1));
    assert(!pico_raster_framebuffer(&assets, draws, 1, 1, 0, 0, out16, 1));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 1, 0, 0, out32, 1));
    assert(!pico_raster_framebuffer(&assets, draws, 1, PICO_RENDER_MAX_WIDTH + 1, 1, 0, out16,
                                    PICO_RENDER_MAX_WIDTH + 1));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, PICO_RENDER_MAX_WIDTH + 1, 1, 0, out32,
                                      PICO_RENDER_MAX_WIDTH + 1));
    assert(!pico_raster_framebuffer(&assets, draws, 1, 2, 1, 0, out16, 1));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 2, 1, 0, out32, 1));
    assert(!pico_raster_framebuffer(&assets, draws, 1, 1, 1, 0, out16, 0));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 1, 1, 0, out32, 0));
    assert(!pico_raster_framebuffer(&assets, draws, 1, 1, 1, 0, out16, SIZE_MAX / 2 + 1));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 1, 1, 0, out32, SIZE_MAX / 4 + 1));
    assert(!pico_raster_framebuffer(&assets, draws, 1, 2, 3, 0, out16, SIZE_MAX / 2));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 2, 3, 0, out32, SIZE_MAX / 4));
    assert(!pico_raster_framebuffer(&assets, draws, 1, 2, UINT_MAX, 0, out16, SIZE_MAX / 2));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 2, UINT_MAX, 0, out32, SIZE_MAX / 4));
    assert(pico_raster_draw_bounds(NULL, draws, 1, 1, &rect) == -1);
    assert(pico_raster_draw_bounds(&invalid_assets, draws, 1, 1, &rect) == -1);
    assert(pico_raster_draw_bounds(&assets, NULL, 1, 1, &rect) == -1);
    assert(pico_raster_draw_bounds(&assets, draws, 1, 1, NULL) == -1);
    assert(pico_raster_draw_bounds(&assets, draws, 0, 1, &rect) == -1);
    assert(pico_raster_draw_bounds(&assets, draws, 1, 0, &rect) == -1);
    assert(pico_raster_draw_bounds(&assets, draws, PICO_RENDER_MAX_WIDTH + 1, 1, &rect) == -1);
    assert(!pico_raster_region(&assets, draws, 1, 1, 1, 0, NULL, out16, 1));
    assert(!pico_raster_region32(&assets, draws, 1, 1, 1, 0, NULL, out32, 1));
    assert(!pico_raster_region(&assets, draws, 1, 1, 1, 0, &region, NULL, 1));
    assert(!pico_raster_region32(&assets, draws, 1, 1, 1, 0, &region, NULL, 1));
    region.x = UINT_MAX;
    assert(!pico_raster_region(&assets, draws, 1, 1, 1, 0, &region, out16, 1));
    assert(!pico_raster_region32(&assets, draws, 1, 1, 1, 0, &region, out32, 1));
    region.x = 0;
    region.y = UINT_MAX;
    assert(!pico_raster_region(&assets, draws, 1, 1, 1, 0, &region, out16, 1));
    assert(!pico_raster_region32(&assets, draws, 1, 1, 1, 0, &region, out32, 1));
    region.y = 0;
    region.width = UINT_MAX;
    assert(!pico_raster_region(&assets, draws, 1, 1, 1, 0, &region, out16, 1));
    assert(!pico_raster_region32(&assets, draws, 1, 1, 1, 0, &region, out32, 1));
    region.width = 1;
    region.height = UINT_MAX;
    assert(!pico_raster_region(&assets, draws, 1, 1, 1, 0, &region, out16, 1));
    assert(!pico_raster_region32(&assets, draws, 1, 1, 1, 0, &region, out32, 1));
    for (i = 0; i < 16; i++) {
        out16[i] = 0x7281;
        out32[i] = 0x4c7281ae;
    }
    region.x = 3;
    region.y = 2;
    region.width = region.height = 0;
    assert(pico_raster_region(&assets, NULL, 0, 3, 2, 0, &region, out16 + 1, 5));
    assert(pico_raster_region32(&assets, NULL, 0, 3, 2, 0, &region, out32 + 1, 5));
    for (i = 0; i < 16; i++) {
        assert(out16[i] == 0x7281);
        assert(out32[i] == 0x4c7281ae);
    }
    assert(pico_raster_framebuffer(&assets, NULL, 0, 3, 2, BACKGROUND16, out16 + 1, 5));
    assert(pico_raster_framebuffer32(&assets, NULL, 0, 3, 2, 0xfd000000u | BACKGROUND32, out32 + 1,
                                     5));
    for (i = 0; i < 16; i++) {
        int covered = (i >= 1 && i <= 3) || (i >= 6 && i <= 8);
        assert(out16[i] == (covered ? BACKGROUND16 : 0x7281));
        assert(out32[i] == (covered ? BACKGROUND32 : 0x4c7281ae));
    }
    for (i = 0; i < MAX_WIDTH * MAX_HEIGHT; i++) {
        pixels16[i] = BACKGROUND16;
        pixels32[i] = BACKGROUND32;
    }
    draws[0].symbol = 999;
    identity(draws + 1, 7, 1, 1);
    draws[1].a = draws[1].d = 0;
    identity(draws + 2, 7, 1, 1);
    draws[2].multiply[3] = 0;
    identity(draws + 3, 7, 1, 1);
    draws[3].tx = draws[3].ty = 20000 * 65536;
    for (i = 0; i < 4; i++) {
        assert(!pico_raster_draw_bounds(&assets, draws + i, MAX_WIDTH, MAX_HEIGHT, &rect));
        assert(!rect.x && !rect.y && !rect.width && !rect.height);
    }
    check_framebuffers(&assets, draws, 4, MAX_WIDTH, MAX_HEIGHT);
    draws[0].kind = 1;
    assert(pico_raster_draw_bounds(&assets, draws, 1, 1, &rect) == -1);
    assert(!pico_raster_framebuffer(&assets, draws, 1, 1, 1, 0, out16, 1));
    assert(!pico_raster_framebuffer32(&assets, draws, 1, 1, 1, 0, out32, 1));
}
#endif

static Golden render_cases(PicoAssets *assets, unsigned numerator, unsigned denominator,
                           unsigned width, unsigned height) {
    Golden result = {UINT64_C(14695981039346656037), UINT64_C(14695981039346656037)};
    unsigned frame;
    output_width = width;
    output_height = height;
    for (frame = 0; frame < FRAMES; frame++) {
        PicoDraw draws[3];
        unsigned y, x, i;
        scene(draws, frame, numerator, denominator);
        pico_assets_reset_cache(assets);
        for (y = 0; y < height; y++) {
            uint16_t guarded16[MAX_WIDTH + 2];
            uint32_t guarded32[MAX_WIDTH + 2];
            guarded16[0] = guarded16[width + 1] = 0x9ab7;
            guarded32[0] = guarded32[width + 1] = 0x7182abcd;
            assert(pico_raster_row(assets, draws, 3, width, height, y, BACKGROUND16,
                                   pixels16 + y * width));
            assert(pico_raster_row32(assets, draws, 3, width, height, y, BACKGROUND32,
                                     pixels32 + y * width));
            for (x = 0; x < width; x++) {
                result.rgb565 = hash_pixel(result.rgb565, pixels16[y * width + x], 2);
                result.rgb888 = hash_pixel(result.rgb888, pixels32[y * width + x], 4);
                guarded16[x + 1] = BACKGROUND16;
                guarded32[x + 1] = BACKGROUND32;
            }
            for (i = 0; i < 3; i++) {
                assert(pico_raster_blit_row(assets, draws + i, width, height, y, guarded16 + 1));
                assert(pico_raster_blit_row32(assets, draws + i, width, height, y, guarded32 + 1));
            }
            assert(!memcmp(guarded16 + 1, pixels16 + y * width, width * sizeof(uint16_t)));
            assert(!memcmp(guarded32 + 1, pixels32 + y * width, width * sizeof(uint32_t)));
            assert(guarded16[0] == 0x9ab7 && guarded16[width + 1] == 0x9ab7);
            assert(guarded32[0] == 0x7182abcd && guarded32[width + 1] == 0x7182abcd);
        }
        output_rows = 0;
        assert(pico_raster_render(assets, draws, 3, width, height, BACKGROUND16, capture16, NULL));
        assert(output_rows == height);
        output_rows = 0;
        assert(
            pico_raster_render32(assets, draws, 3, width, height, BACKGROUND32, capture32, NULL));
        assert(output_rows == height);
#if !defined(PICO_WRITE_GOLDENS) && !defined(PICO_REFERENCE_RENDERER)
        check_framebuffers(assets, draws, 3, width, height);
        check_regions(assets, draws, 3, width, height);
        check_draw_bounds(assets, draws, 3, width, height);
#endif
    }
    return result;
}

int main(void) {
    static const unsigned scales[][2] = {{1, 4}, {1, 1}, {2, 1}};
    static const unsigned sizes[][2] = {{97, 61}, {127, 83}};
    static const size_t capacities[] = {0, 1, 364, sizeof(arena)};
    unsigned format, scale, size, binding;
    assert(PICO_RENDER_MAX_WIDTH >= MAX_WIDTH);
#if !defined(PICO_WRITE_GOLDENS) && !defined(PICO_REFERENCE_RENDERER)
    test_framebuffer_arguments();
    test_vertical_cull();
#endif
    for (format = 0; format < 8; format++) {
#ifdef PICO_WRITE_GOLDENS
        printf("    {\n");
#endif
        for (scale = 0; scale < 3; scale++) {
#ifdef PICO_WRITE_GOLDENS
            printf("        {");
#endif
            for (size = 0; size < 2; size++) {
                PicoAssets assets;
                PicoAssetCache cache;
                unsigned bytes = fixture(format, scales[scale][0], scales[scale][1]);
                assert(pico_assets_init(&assets, pack, bytes));
                for (binding = 0; binding < sizeof(capacities) / sizeof(capacities[0]); binding++) {
                    Golden actual;
                    assert(pico_assets_bind_cache(&assets, binding ? &cache : NULL,
                                                  binding ? arena : NULL, capacities[binding]));
                    actual = render_cases(&assets, scales[scale][0], scales[scale][1],
                                          sizes[size][0], sizes[size][1]);
#ifdef PICO_WRITE_GOLDENS
                    if (!binding)
                        printf("%s{UINT64_C(0x%016" PRIx64 "), UINT64_C(0x%016" PRIx64 ")}",
                               size ? ", " : "", actual.rgb565, actual.rgb888);
#else
                    if (actual.rgb565 != goldens[format != 0][scale][size].rgb565 ||
                        actual.rgb888 != goldens[format != 0][scale][size].rgb888) {
                        fprintf(stderr,
                                "pixel mismatch: format=%u scale=%u/%u size=%ux%u cache=%zu "
                                "RGB565=%016" PRIx64 " RGB888=%016" PRIx64 "\n",
                                format, scales[scale][0], scales[scale][1], sizes[size][0],
                                sizes[size][1], capacities[binding], actual.rgb565, actual.rgb888);
                        return 1;
                    }
#endif
                }
            }
#ifdef PICO_WRITE_GOLDENS
            printf("}%s\n", scale == 2 ? "" : ",");
#endif
        }
#ifdef PICO_WRITE_GOLDENS
        printf("    }%s\n", format == 7 ? "" : ",");
#endif
    }
#ifndef PICO_WRITE_GOLDENS
    puts("renderer pixel equivalence: frozen RGB565/RGB888 hashes, all pack formats, fractional "
         "affine transforms, clipped thin shapes, colour transforms and bounded caches passed");
#endif
    return 0;
}
