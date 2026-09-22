#ifndef PICO_RENDER_H
#define PICO_RENDER_H
#include <stddef.h>
#include <stdint.h>

/* Offline rasterised leaf assets, accessed directly from ROM. No allocation. */
#ifndef PICO_RENDER_MAX_WIDTH
#define PICO_RENDER_MAX_WIDTH 550
#endif
#ifndef PICO_RGBA_MAX_SOURCE_WIDTH
#define PICO_RGBA_MAX_SOURCE_WIDTH 2048
#endif
#if PICO_RGBA_MAX_SOURCE_WIDTH < 1 || PICO_RGBA_MAX_SOURCE_WIDTH > 65535
#error PICO_RGBA_MAX_SOURCE_WIDTH must be between 1 and 65535
#endif
#ifndef PICO_ASSET_CACHE_ENTRIES
#define PICO_ASSET_CACHE_ENTRIES 64
#endif
#if PICO_ASSET_CACHE_ENTRIES < 1 || PICO_ASSET_CACHE_ENTRIES > 65535
#error PICO_ASSET_CACHE_ENTRIES must be between 1 and 65535
#endif
typedef struct PicoAssetCache {
    uint8_t *pixels;
    const uint8_t *source;
    size_t capacity, used;
    unsigned count;
    struct {
        const uint8_t *record;
        size_t offset;
    } entries[PICO_ASSET_CACHE_ENTRIES];
} PicoAssetCache;
typedef struct PicoAssets {
    const uint8_t *data;
    size_t size;
    uint32_t records_offset, palette_offset;
    uint16_t count, scale_num, scale_den, version;
    PicoAssetCache *cache;
} PicoAssets;

/* Local SWF pixels to original 550x350 stage pixels, signed Q16.16.
 * x'=a*x+c*y+tx; y'=b*x+d*y+ty. The renderer applies viewport scaling.
 * CXFORM multiply is Q8.8, add is integer; initialize multiply to 256. */
typedef struct PicoDraw {
    int32_t a, b, c, d, tx, ty;
    int16_t multiply[4], add[4];
    uint16_t symbol, ratio;
    uint8_t kind; /* 0=leaf; masks unsupported (source game has none). */
} PicoDraw;

typedef struct PicoRect {
    unsigned x, y, width, height;
} PicoRect;

typedef void (*PicoScanline)(void *user, unsigned y, const uint16_t *rgb565, unsigned width);
typedef void (*PicoScanline32)(void *user, unsigned y, const uint32_t *rgb888, unsigned width);

/* Version 1 stores indexed RGB565; versions 2 and 3 store 8-bit RGBA.
 * Version 3 decodes one source row in 4*PICO_RGBA_MAX_SOURCE_WIDTH bytes of
 * temporary stack storage. This applies to validation, rendering and hits;
 * versions 1 and 2 do not reserve this storage.
 * Returns 1 for a valid bounded asset pack, 0 otherwise. */
int pico_assets_init(PicoAssets *assets, const void *data, size_t size);
/* Optional decoded pixels for rotated/sheared v3 leaves. The pixel arena needs
 * no alignment beyond a byte. All storage belongs
 * to the caller and must outlive the binding. A NULL cache detaches it.
 * Initialising another pack detaches the previous cache. Row/scanline helpers
 * use the normal decoder when either bound is reached, without eviction.
 * Framebuffer helpers can reuse the arena between completed leaves. */
int pico_assets_bind_cache(PicoAssets *assets, PicoAssetCache *cache, void *memory,
                           size_t capacity);
/* Clear before a frame to give its visible leaves the available budget.
 * Complete-frame render helpers do this automatically. Row/blit users call
 * it once themselves; retained immutable pixels remain valid if omitted. */
void pico_assets_reset_cache(const PicoAssets *assets);
/* Counts morph variants too. Lookup selects the nearest authored or sampled ratio. */
unsigned pico_assets_count(const PicoAssets *assets);
/* Draws back-to-front. Work RAM is one RGB565 scanline (2*MAX_WIDTH bytes).
 * width <= PICO_RENDER_MAX_WIDTH. Source artwork is 550x350. */
int pico_raster_render(const PicoAssets *assets, const PicoDraw *draws, unsigned count,
                       unsigned width, unsigned height, uint16_t background, PicoScanline scanline,
                       void *user);
/* Draw a single row into caller-owned storage; useful for custom display drivers. */
int pico_raster_row(const PicoAssets *assets, const PicoDraw *draws, unsigned count, unsigned width,
                    unsigned height, unsigned y, uint16_t background, uint16_t *row);
/* Composite one draw into an existing row without clearing. A host may walk
 * the runtime display list once per row and avoid retaining any draw list. */
int pico_raster_blit_row(const PicoAssets *assets, const PicoDraw *draw, unsigned width,
                         unsigned height, unsigned y, uint16_t *row);
/* Full-colour output is packed 0x00RRGGBB. All pack versions are accepted.
 * Versions 2 and 3 preserve source colour and soft alpha when compositing. The
 * render helper uses 4*MAX_WIDTH bytes; row helpers use caller-owned storage. */
int pico_raster_render32(const PicoAssets *assets, const PicoDraw *draws, unsigned count,
                         unsigned width, unsigned height, uint32_t background,
                         PicoScanline32 scanline, void *user);
int pico_raster_row32(const PicoAssets *assets, const PicoDraw *draws, unsigned count,
                      unsigned width, unsigned height, unsigned y, uint32_t background,
                      uint32_t *row);
int pico_raster_blit_row32(const PicoAssets *assets, const PicoDraw *draw, unsigned width,
                           unsigned height, unsigned y, uint32_t *row);
/* Optional full framebuffer path: composite each leaf across its rows before
 * advancing to the next. This reuses draw setup and decoded rows, with no heap
 * or retained setup list. The caller owns at least (height-1)*stride+width
 * pixels; stride is in pixels and must be >= width. Padding is untouched.
 * The optional leaf cache is reset at frame start and may be reused between
 * leaves when full. The row APIs remain available without a framebuffer. */
int pico_raster_framebuffer(const PicoAssets *assets, const PicoDraw *draws, unsigned count,
                            unsigned width, unsigned height, uint16_t background, uint16_t *pixels,
                            size_t stride);
int pico_raster_framebuffer32(const PicoAssets *assets, const PicoDraw *draws, unsigned count,
                              unsigned width, unsigned height, uint32_t background,
                              uint32_t *pixels, size_t stride);
/* Conservative output bounds, clipped to the viewport. Returns 1 if visible,
 * 0 for an empty draw, or -1 for invalid arguments/unsupported draw kinds. */
int pico_raster_draw_bounds(const PicoAssets *assets, const PicoDraw *draw, unsigned width,
                            unsigned height, PicoRect *rect);
/* Redraw one region of an existing framebuffer. Pixels outside it are untouched.
 * The region must fit the viewport; all draws are still composited in order.
 * Use old and new bounds when accumulating damage from a changed draw. */
int pico_raster_region(const PicoAssets *assets, const PicoDraw *draws, unsigned count,
                       unsigned width, unsigned height, uint16_t background, const PicoRect *region,
                       uint16_t *pixels, size_t stride);
int pico_raster_region32(const PicoAssets *assets, const PicoDraw *draws, unsigned count,
                         unsigned width, unsigned height, uint32_t background,
                         const PicoRect *region, uint32_t *pixels, size_t stride);
int pico_asset_hit(const PicoAssets *assets, unsigned symbol, unsigned ratio, int32_t local_x_q16,
                   int32_t local_y_q16);
int pico_draw_inverse(const PicoDraw *draw, int32_t stage_x_q16, int32_t stage_y_q16,
                      int32_t *local_x_q16, int32_t *local_y_q16);
/* MSB-first 1bpp output; white=1. Caller supplies ceil(width/8) bytes.
 * Ordered 4x4 Bayer luminance dither is anchored to screen coordinates. */
void pico_rgb565_to_mono(const uint16_t *row, unsigned width, unsigned y, uint8_t *bits);
#endif
