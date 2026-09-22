#include "pico_render.h"
#include <limits.h>
#include <string.h>

#ifndef PICO_NOINLINE
#if defined(_MSC_VER)
#define PICO_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define PICO_NOINLINE __attribute__((noinline))
#else
#define PICO_NOINLINE
#endif
#endif

/* These helpers ran inline in the original scanline loop. Keep that cost
 * when optional framebuffer callers would otherwise make -Os outline them. */
#if defined(_MSC_VER)
#define PICO_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define PICO_INLINE inline __attribute__((always_inline))
#else
#define PICO_INLINE inline
#endif

/* All multibyte pack reads are explicit little endian: no alignment or host
 * endian assumptions. Assets must remain mapped/readable after initialization. */
static uint16_t u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static int16_t s16(const uint8_t *p) {
    uint16_t v = u16(p);
    return (int16_t)(v < 32768 ? v : (int32_t)v - 65536);
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int span(size_t size, uint32_t off, uint32_t n) {
    return off <= size && n <= size - off;
}
static const uint8_t *record(const PicoAssets *a, unsigned i) {
    return a->data + a->records_offset + 24 * i;
}

static int lz4_length(const uint8_t *p, uint32_t n, uint32_t *at, unsigned base, unsigned maximum,
                      int extended, unsigned *length) {
    unsigned value = base;
    if (value > maximum)
        return 0;
    if (extended) {
        unsigned extra;
        do {
            if (*at == n)
                return 0;
            extra = p[(*at)++];
            if (extra > maximum - value)
                return 0;
            value += extra;
        } while (extra == 255);
    }
    *length = value;
    return 1;
}

/* A row is an independent LZ4 block with an exact decoded size. Forward
 * copying is required for matches whose length exceeds their offset. */
static int lz4_row(const uint8_t *p, uint32_t n, uint8_t *out, unsigned size) {
    uint32_t at = 0;
    unsigned used = 0, last_match = 0;
    int matched = 0;
    while (at < n) {
        unsigned token = p[at++], literals, length, offset, i;
        if (!lz4_length(p, n, &at, token >> 4, size - used, (token >> 4) == 15, &literals) ||
            literals > n - at)
            return 0;
        memcpy(out + used, p + at, literals);
        used += literals;
        at += literals;
        if (at == n)
            return used == size && (!matched || (literals >= 5 && size - last_match >= 12));
        if (n - at < 2)
            return 0;
        offset = u16(p + at);
        at += 2;
        if (!offset || offset > used ||
            !lz4_length(p, n, &at, (token & 15) + 4, size - used, (token & 15) == 15, &length))
            return 0;
        last_match = used;
        matched = 1;
        if (offset == 1) {
            memset(out + used, out[used - 1], length);
            used += length;
        } else if (offset >= length) {
            memcpy(out + used, out + used - offset, length);
            used += length;
        } else
            for (i = 0; i < length; i++) {
                out[used] = out[used - offset];
                used++;
            }
    }
    return 0;
}

/* Return 1 for interleaved bytes, 2 for channel planes, or 0 on invalid data.
 * Sub filtering is undone in place without a second row buffer. */
static int decode_row(const uint8_t *p, uint32_t n, unsigned width, uint8_t *out) {
    unsigned mode, i, size = width * 4;
    if (!n || !width || width > PICO_RGBA_MAX_SOURCE_WIDTH)
        return 0;
    mode = *p++;
    n--;
    if (mode == 0) {
        if (n != size)
            return 0;
        memcpy(out, p, size);
    } else if (mode == 1) {
        unsigned used = 0;
        if (n % 5)
            return 0;
        while (n) {
            unsigned count = p[0], x, channel;
            if (!count || count > width - used)
                return 0;
            for (x = 0; x < count; x++) {
                for (channel = 0; channel < 4; channel++)
                    out[used * 4 + channel] = p[1 + channel];
                used++;
            }
            p += 5;
            n -= 5;
        }
        if (used != width)
            return 0;
    } else if (mode <= 5) {
        if (!lz4_row(p, n, out, size))
            return 0;
        if (mode == 4)
            for (i = 4; i < size; i++)
                out[i] = (uint8_t)(out[i] + out[i - 4]);
        else if (mode == 5) {
            unsigned channel;
            for (channel = 0; channel < 4; channel++)
                for (i = 1; i < width; i++)
                    out[channel * width + i] =
                        (uint8_t)(out[channel * width + i] + out[channel * width + i - 1]);
        }
    } else
        return 0;
    return mode == 3 || mode == 5 ? 2 : 1;
}

/* Keep this scratch allocation out of callers handling indexed packs. Ports
 * using another compiler can supply its no-inline annotation above. */
static PICO_NOINLINE int validate_row3(const uint8_t *p, uint32_t n, unsigned width) {
    uint8_t scratch[PICO_RGBA_MAX_SOURCE_WIDTH * 4];
    return decode_row(p, n, width, scratch) != 0;
}

int pico_assets_init(PicoAssets *a, const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    unsigned i, run_size;
    uint32_t prev = 0;
    if (!a)
        return 0;
    a->data = 0;
    a->size = 0;
    a->count = 0;
    a->version = 0;
    a->cache = 0;
    if (!p || size < 32 || p[0] != 'P' || p[1] != 'C' || p[2] != 'T' || p[3] != 'A' ||
        (u16(p + 4) < 1 || u16(p + 4) > 3))
        return 0;
    a->version = u16(p + 4);
    run_size = a->version == 2 ? 5 : 2;
    a->count = u16(p + 6);
    a->scale_num = u16(p + 8);
    a->scale_den = u16(p + 10);
    a->records_offset = u32(p + 12);
    a->palette_offset = u32(p + 16);
    if (!a->count || !a->scale_num || !a->scale_den ||
        !span(size, a->records_offset, 24u * a->count) ||
        (a->version == 1 ? !span(size, a->palette_offset, 512) : a->palette_offset != 0))
        goto bad;
    a->data = p;
    a->size = size;
    for (i = 0; i < a->count; i++) {
        const uint8_t *r = record(a, i);
        unsigned w = u16(r + 8), h = u16(r + 10), y;
        uint32_t rows = u32(r + 12), start = u32(r + 16), end = u32(r + 20);
        uint32_t key = ((uint32_t)u16(r) << 16) | u16(r + 2);
        if ((i && key <= prev) || !w || !h || !span(size, rows, 4u * (h + 1)) || start > end ||
            !span(size, start, end - start) || (a->version == 3 && w > PICO_RGBA_MAX_SOURCE_WIDTH))
            goto bad;
        prev = key;
        if (u32(p + rows) != start || u32(p + rows + 4 * h) != end)
            goto bad;
        for (y = 0; y < h; y++) {
            uint32_t off = u32(p + rows + 4 * y), lim = u32(p + rows + 4 * (y + 1));
            unsigned total = 0;
            if (off < start || lim > end || off > lim)
                goto bad;
            if (a->version == 3) {
                if (!validate_row3(p + off, lim - off, w))
                    goto bad;
                continue;
            }
            if ((lim - off) % run_size)
                goto bad;
            while (off < lim) {
                unsigned n = p[off];
                if (!n || total + n > w)
                    goto bad;
                total += n;
                off += run_size;
            }
            if (total != w)
                goto bad;
        }
    }
    return 1;
bad:
    a->data = 0;
    a->size = 0;
    a->count = 0;
    a->version = 0;
    return 0;
}
unsigned pico_assets_count(const PicoAssets *a) {
    return a && a->data ? a->count : 0;
}
int pico_assets_bind_cache(PicoAssets *a, PicoAssetCache *cache, void *memory, size_t capacity) {
    if (!a || !a->data || (cache && (!memory || !capacity)))
        return 0;
    if (cache) {
        cache->pixels = (uint8_t *)memory;
        cache->source = a->data;
        cache->capacity = capacity;
        cache->used = 0;
        cache->count = 0;
    }
    a->cache = cache;
    return 1;
}
void pico_assets_reset_cache(const PicoAssets *a) {
    if (a && a->cache && a->cache->source == a->data) {
        a->cache->used = 0;
        a->cache->count = 0;
    }
}
static const uint8_t *cache_lookup(const PicoAssets *a, const uint8_t *r) {
    PicoAssetCache *cache = a->cache;
    unsigned i;
    if (!cache || cache->source != a->data)
        return 0;
    for (i = 0; i < cache->count; i++)
        if (cache->entries[i].record == r)
            return cache->pixels + cache->entries[i].offset;
    return 0;
}
static const uint8_t *cache_leaf(const PicoAssets *a, const uint8_t *r, uint8_t *scratch) {
    PicoAssetCache *cache = a->cache;
    const uint8_t *existing = cache_lookup(a, r);
    unsigned width = u16(r + 8), height = u16(r + 10), y, x;
    uint32_t rows = u32(r + 12);
    size_t bytes;
    uint8_t *pixels;
    if (existing)
        return existing;
    if (!cache || cache->source != a->data || cache->count == PICO_ASSET_CACHE_ENTRIES ||
        height > (cache->capacity - cache->used) / 4 / width)
        return 0;
    bytes = (size_t)width * height * 4;
    pixels = cache->pixels + cache->used;
    for (y = 0; y < height; y++) {
        uint32_t start = u32(a->data + rows + 4 * y);
        int mode =
            decode_row(a->data + start, u32(a->data + rows + 4 * (y + 1)) - start, width, scratch);
        uint8_t *out = pixels + (size_t)y * width * 4;
        if (!mode)
            return 0;
        if (mode == 1)
            memcpy(out, scratch, width * 4);
        else
            for (x = 0; x < width; x++) {
                out[x * 4] = scratch[x];
                out[x * 4 + 1] = scratch[width + x];
                out[x * 4 + 2] = scratch[width * 2 + x];
                out[x * 4 + 3] = scratch[width * 3 + x];
            }
    }
    cache->entries[cache->count].record = r;
    cache->entries[cache->count].offset = cache->used;
    cache->count++;
    cache->used += bytes;
    return pixels;
}

static const uint8_t *find_asset(const PicoAssets *a, unsigned symbol, unsigned ratio) {
    unsigned lo = 0, hi = a->count, best, delta;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (u16(record(a, mid)) < symbol)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == a->count || u16(record(a, lo)) != symbol)
        return 0;
    best = lo;
    delta = UINT_MAX;
    while (lo < a->count) {
        const uint8_t *r = record(a, lo);
        unsigned q, d;
        if (u16(r) != symbol)
            break;
        q = u16(r + 2);
        d = q > ratio ? q - ratio : ratio - q;
        if (d < delta) {
            best = lo;
            delta = d;
        }
        ++lo;
    }
    return record(a, best);
}

typedef struct RowCursor {
    int y;
    unsigned begin, end;
    uint32_t pos, limit;
    uint32_t value;
    const uint8_t *cached;
} RowCursor;
static uint32_t expand565(uint16_t p) {
    return (uint32_t)((p >> 11) * 255 / 31) << 16 | (uint32_t)(((p >> 5) & 63) * 255 / 63) << 8 |
           (p & 31) * 255 / 31;
}
static uint16_t reduce565(uint32_t p) {
    return (uint16_t)(((p >> 19) & 31) << 11 | ((p >> 10) & 63) << 5 | ((p >> 3) & 31));
}
static uint32_t sample(const PicoAssets *a, const uint8_t *r, int x, int y, RowCursor *c,
                       uint8_t *scratch) {
    const uint8_t *p = a->data;
    if (x < 0 || y < 0 || x >= u16(r + 8) || y >= u16(r + 10))
        return 0;
    if (a->version == 3) {
        unsigned width = u16(r + 8), step, pos;
        if (c->cached) {
            const uint8_t *q = c->cached + ((size_t)y * width + (unsigned)x) * 4;
            return (uint32_t)q[3] << 24 | (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
        }
        if (c->y != y) {
            uint32_t rows = u32(r + 12), start = u32(p + rows + 4 * y);
            c->value = (uint32_t)decode_row(p + start, u32(p + rows + 4 * (y + 1)) - start, width,
                                            scratch);
            c->y = y;
        }
        if (!c->value)
            return 0;
        step = c->value == 2 ? width : 1;
        pos = c->value == 2 ? (unsigned)x : (unsigned)x * 4;
        return (uint32_t)scratch[pos + step * 3] << 24 | (uint32_t)scratch[pos] << 16 |
               (uint32_t)scratch[pos + step] << 8 | scratch[pos + step * 2];
    }
    if (c->y != y || (unsigned)x < c->begin) {
        uint32_t rows = u32(r + 12);
        c->pos = u32(p + rows + 4 * y);
        c->limit = u32(p + rows + 4 * (y + 1));
        c->y = y;
        c->begin = 0;
        c->end = 0;
    }
    while ((unsigned)x >= c->end && c->pos < c->limit) {
        c->begin = c->end;
        c->end += p[c->pos];
        if (a->version == 2) {
            c->value = (uint32_t)p[c->pos + 4] << 24 | (uint32_t)p[c->pos + 1] << 16 |
                       (uint32_t)p[c->pos + 2] << 8 | p[c->pos + 3];
            c->pos += 5;
        } else {
            unsigned index = p[c->pos + 1];
            c->value = index ? 0xff000000u | expand565(u16(p + a->palette_offset + index * 2)) : 0;
            c->pos += 2;
        }
    }
    return c->value;
}
static int floorq(int64_t v) {
    return (int)(v >= 0 ? v / 65536 : -((-v + 65535) / 65536));
}
static PICO_NOINLINE uint32_t hit_sample3(const PicoAssets *a, const uint8_t *r, int x, int y) {
    uint8_t scratch[PICO_RGBA_MAX_SOURCE_WIDTH * 4];
    RowCursor cur = {-1, 0, 0, 0, 0, 0, 0};
    return sample(a, r, x, y, &cur, scratch);
}
int pico_asset_hit(const PicoAssets *a, unsigned symbol, unsigned ratio, int32_t x, int32_t y) {
    const uint8_t *r;
    int px, py;
    RowCursor cur = {-1, 0, 0, 0, 0, 0, 0};
    if (!a || !a->data || (r = find_asset(a, symbol, ratio)) == 0)
        return 0;
    px = floorq((int64_t)x * a->scale_num / a->scale_den) - s16(r + 4);
    py = floorq((int64_t)y * a->scale_num / a->scale_den) - s16(r + 6);
    if (a->version == 3)
        cur.cached = cache_lookup(a, r);
    return ((a->version == 3 && !cur.cached ? hit_sample3(a, r, px, py)
                                            : sample(a, r, px, py, &cur, 0)) >>
            24) >= 128;
}
static int clamp8(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}
static PICO_INLINE uint32_t color(uint32_t src, uint32_t dst, const PicoDraw *d) {
    int r = (src >> 16) & 255, g = (src >> 8) & 255, b = src & 255;
    int alpha = clamp8((int)(src >> 24) * d->multiply[3] / 256 + d->add[3]);
    if (d->multiply[0] != 256 || d->add[0])
        r = clamp8(r * d->multiply[0] / 256 + d->add[0]);
    if (d->multiply[1] != 256 || d->add[1])
        g = clamp8(g * d->multiply[1] / 256 + d->add[1]);
    if (d->multiply[2] != 256 || d->add[2])
        b = clamp8(b * d->multiply[2] / 256 + d->add[2]);
    if (alpha < 255) {
        int dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
        r = (r * alpha + dr * (255 - alpha) + 127) / 255;
        g = (g * alpha + dg * (255 - alpha) + 127) / 255;
        b = (b * alpha + db * (255 - alpha) + 127) / 255;
    }
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* Conservative transformed bitmap bounds. Fixed-point math remains integer;
 * no libm, floating-point, heap, graphics library or SWF VM is linked. */
static PICO_INLINE void bounds(const PicoAssets *a, const uint8_t *r, const PicoDraw *d,
                               int64_t *minx, int64_t *maxx, int64_t *miny, int64_t *maxy) {
    int64_t xs[2], ys[2];
    unsigned x, y;
    xs[0] = (int64_t)s16(r + 4) * 65536 * a->scale_den / a->scale_num;
    xs[1] = (int64_t)(s16(r + 4) + u16(r + 8)) * 65536 * a->scale_den / a->scale_num;
    ys[0] = (int64_t)s16(r + 6) * 65536 * a->scale_den / a->scale_num;
    ys[1] = (int64_t)(s16(r + 6) + u16(r + 10)) * 65536 * a->scale_den / a->scale_num;
    *minx = *miny = INT64_MAX;
    *maxx = *maxy = INT64_MIN;
    for (y = 0; y < 2; y++)
        for (x = 0; x < 2; x++) {
            int64_t px = ((int64_t)d->a * xs[x] + (int64_t)d->c * ys[y]) / 65536 + d->tx;
            int64_t py = ((int64_t)d->b * xs[x] + (int64_t)d->d * ys[y]) / 65536 + d->ty;
            if (px < *minx)
                *minx = px;
            if (px > *maxx)
                *maxx = px;
            if (py < *miny)
                *miny = py;
            if (py > *maxy)
                *maxy = py;
        }
}

typedef struct RasterDraw {
    const uint8_t *record;
    int64_t ia, ib, ic, id, miny, maxy, sx, stepx, stepy;
    int first, last, plain;
} RasterDraw;

static int prepare_draw(const PicoAssets *a, const PicoDraw *d, unsigned width, RasterDraw *p) {
    int64_t det, minx, maxx;
    if (d->kind)
        return -1;
    p->record = find_asset(a, d->symbol, d->ratio);
    if (!p->record || (d->multiply[3] >= 0 && clamp8(255 * d->multiply[3] / 256 + d->add[3]) == 0))
        return 0;
    bounds(a, p->record, d, &minx, &maxx, &p->miny, &p->maxy);
    p->first = floorq(minx * width / 550) - 1;
    p->last = floorq(maxx * width / 550) + 1;
    if (p->first < 0)
        p->first = 0;
    if (p->last > (int)width)
        p->last = (int)width;
    det = (int64_t)d->a * d->d - (int64_t)d->b * d->c;
    if (p->first >= p->last || !det)
        return 0;
    p->ia = (int64_t)d->d * 4294967296LL / det;
    p->ib = -(int64_t)d->b * 4294967296LL / det;
    p->ic = -(int64_t)d->c * 4294967296LL / det;
    p->id = (int64_t)d->a * 4294967296LL / det;
    p->sx = ((int64_t)p->first * 65536 + 32768) * 550 / width;
    p->stepx = p->ia * 550 * a->scale_num / ((int64_t)width * a->scale_den);
    p->stepy = p->ib * 550 * a->scale_num / ((int64_t)width * a->scale_den);
    p->plain = d->multiply[0] == 256 && d->multiply[1] == 256 && d->multiply[2] == 256 &&
               d->multiply[3] == 256 && !d->add[0] && !d->add[1] && !d->add[2] && !d->add[3];
    return 1;
}

static void draw_row(const PicoAssets *a, const PicoDraw *d, const RasterDraw *p, int64_t sy,
                     int first, int last, void *row, int rgb32, RowCursor *cur, uint8_t *scratch) {
    const uint8_t *r = p->record;
    int64_t lx, ly;
    unsigned x;
    lx = ((p->ia * (p->sx - d->tx) + p->ic * (sy - d->ty)) / 65536) * a->scale_num / a->scale_den -
         (int64_t)s16(r + 4) * 65536;
    ly = ((p->ib * (p->sx - d->tx) + p->id * (sy - d->ty)) / 65536) * a->scale_num / a->scale_den -
         (int64_t)s16(r + 6) * 65536;
    /* Keep the original sampling origin and quantised steps when clipping. */
    lx += (first - p->first) * p->stepx;
    ly += (first - p->first) * p->stepy;
    for (x = (unsigned)first; x < (unsigned)last; x++) {
        uint32_t rgba = sample(a, r, floorq(lx), floorq(ly), cur, scratch);
        if (rgba >> 24) {
            uint32_t rgb;
            if (p->plain && (rgba >> 24) == 255)
                rgb = rgba & 0xffffffu;
            else
                rgb =
                    color(rgba, rgb32 ? ((uint32_t *)row)[x] : expand565(((uint16_t *)row)[x]), d);
            if (rgb32)
                ((uint32_t *)row)[x] = rgb;
            else
                ((uint16_t *)row)[x] = reduce565(rgb);
        }
        lx += p->stepx;
        ly += p->stepy;
    }
}

/* Keep the scanline hot loop independent of framebuffer setup storage. Passing
 * a prepared draw per leaf/row raised measured streaming cost in busy scenes.
 * Golden pixel tests keep both paths on the same sampling rules. */
static int raster_row_work(const PicoAssets *a, const PicoDraw *draws, unsigned count,
                           unsigned width, unsigned height, unsigned y, uint32_t bg, void *row,
                           int clear, int rgb32, uint8_t *scratch) {
    unsigned n, x;
    int64_t sy;
    if (!a || !a->data || !row || !width || width > PICO_RENDER_MAX_WIDTH || !height ||
        y >= height || (!draws && count))
        return 0;
    if (clear) {
        for (x = 0; x < width; x++)
            if (rgb32)
                ((uint32_t *)row)[x] = bg & 0xffffffu;
            else
                ((uint16_t *)row)[x] = (uint16_t)bg;
    }
    sy = ((int64_t)y * 65536 + 32768) * 350 / height;
    for (n = 0; n < count; n++) {
        const PicoDraw *d = draws + n;
        const uint8_t *r;
        int64_t det, ia, ib, ic, id, minx, maxx, miny, maxy, sx, lx, ly, stepx, stepy;
        int first, last, plain;
        RowCursor cur = {-1, 0, 0, 0, 0, 0, 0};
        if (d->kind)
            return 0;
        r = find_asset(a, d->symbol, d->ratio);
        if (!r)
            continue;
        if (d->multiply[3] >= 0 && clamp8(255 * d->multiply[3] / 256 + d->add[3]) == 0)
            continue;
        if (!d->b) {
            /* Most leaves miss this scanline. When y is independent of x,
             * reject them before finding all four transformed corners. */
            int64_t y0 = (int64_t)s16(r + 6) * 65536 * a->scale_den / a->scale_num;
            int64_t y1 = (int64_t)(s16(r + 6) + u16(r + 10)) * 65536 * a->scale_den /
                         a->scale_num;
            y0 = (int64_t)d->d * y0 / 65536 + d->ty;
            y1 = (int64_t)d->d * y1 / 65536 + d->ty;
            if (sy < (y0 < y1 ? y0 : y1) || sy >= (y0 > y1 ? y0 : y1))
                continue;
        }
        bounds(a, r, d, &minx, &maxx, &miny, &maxy);
        if (sy < miny || sy >= maxy)
            continue;
        first = floorq(minx * width / 550) - 1;
        last = floorq(maxx * width / 550) + 1;
        if (first < 0)
            first = 0;
        if (last > (int)width)
            last = (int)width;
        if (first >= last)
            continue;
        det = (int64_t)d->a * d->d - (int64_t)d->b * d->c;
        if (!det)
            continue;
        ia = (int64_t)d->d * 4294967296LL / det;
        ib = -(int64_t)d->b * 4294967296LL / det;
        ic = -(int64_t)d->c * 4294967296LL / det;
        id = (int64_t)d->a * 4294967296LL / det;
        sx = ((int64_t)first * 65536 + 32768) * 550 / width;
        lx = ((ia * (sx - d->tx) + ic * (sy - d->ty)) / 65536) * a->scale_num / a->scale_den -
             (int64_t)s16(r + 4) * 65536;
        ly = ((ib * (sx - d->tx) + id * (sy - d->ty)) / 65536) * a->scale_num / a->scale_den -
             (int64_t)s16(r + 6) * 65536;
        stepx = ia * 550 * a->scale_num / ((int64_t)width * a->scale_den);
        stepy = ib * 550 * a->scale_num / ((int64_t)width * a->scale_den);
        plain = d->multiply[0] == 256 && d->multiply[1] == 256 && d->multiply[2] == 256 &&
                d->multiply[3] == 256 && !d->add[0] && !d->add[1] && !d->add[2] && !d->add[3];
        if (a->version == 3 && d->b)
            cur.cached = cache_leaf(a, r, scratch);
        for (x = (unsigned)first; x < (unsigned)last; x++) {
            uint32_t rgba = sample(a, r, floorq(lx), floorq(ly), &cur, scratch);
            if (rgba >> 24) {
                uint32_t rgb;
                if (plain && (rgba >> 24) == 255)
                    rgb = rgba & 0xffffffu;
                else
                    rgb = color(rgba,
                                rgb32 ? ((uint32_t *)row)[x] : expand565(((uint16_t *)row)[x]), d);
                if (rgb32)
                    ((uint32_t *)row)[x] = rgb;
                else
                    ((uint16_t *)row)[x] = reduce565(rgb);
            }
            lx += stepx;
            ly += stepy;
        }
    }
    return 1;
}
static PICO_NOINLINE int raster_row3(const PicoAssets *a, const PicoDraw *draws, unsigned count,
                                     unsigned width, unsigned height, unsigned y, uint32_t bg,
                                     void *row, int clear, int rgb32) {
    uint8_t scratch[PICO_RGBA_MAX_SOURCE_WIDTH * 4];
    return raster_row_work(a, draws, count, width, height, y, bg, row, clear, rgb32, scratch);
}
static int raster_row(const PicoAssets *a, const PicoDraw *draws, unsigned count, unsigned width,
                      unsigned height, unsigned y, uint32_t bg, void *row, int clear, int rgb32) {
    if (a && a->data && a->version == 3)
        return raster_row3(a, draws, count, width, height, y, bg, row, clear, rgb32);
    return raster_row_work(a, draws, count, width, height, y, bg, row, clear, rgb32, 0);
}
int pico_raster_row(const PicoAssets *a, const PicoDraw *draws, unsigned count, unsigned width,
                    unsigned height, unsigned y, uint16_t bg, uint16_t *row) {
    return raster_row(a, draws, count, width, height, y, bg, row, 1, 0);
}
int pico_raster_blit_row(const PicoAssets *a, const PicoDraw *draw, unsigned width, unsigned height,
                         unsigned y, uint16_t *row) {
    return raster_row(a, draw, 1, width, height, y, 0, row, 0, 0);
}
int pico_raster_row32(const PicoAssets *a, const PicoDraw *draws, unsigned count, unsigned width,
                      unsigned height, unsigned y, uint32_t bg, uint32_t *row) {
    return raster_row(a, draws, count, width, height, y, bg, row, 1, 1);
}
int pico_raster_blit_row32(const PicoAssets *a, const PicoDraw *draw, unsigned width,
                           unsigned height, unsigned y, uint32_t *row) {
    return raster_row(a, draw, 1, width, height, y, 0, row, 0, 1);
}
static int framebuffer_work(const PicoAssets *a, const PicoDraw *draws, unsigned count,
                            unsigned width, unsigned height, uint32_t bg, const PicoRect *clip,
                            void *pixels, size_t stride, int rgb32, uint8_t *scratch) {
    unsigned n, y, x;
    PicoRect all = {0, 0, width, height};
    size_t bytes = rgb32 ? sizeof(uint32_t) : sizeof(uint16_t);
    if (!a || !a->data || !pixels || !width || width > PICO_RENDER_MAX_WIDTH || !height ||
        (!draws && count) || stride < width || stride > SIZE_MAX / bytes ||
        height - 1 > (SIZE_MAX / bytes - width) / stride)
        return 0;
    if (!clip)
        clip = &all;
    if (clip->x > width || clip->width > width - clip->x || clip->y > height ||
        clip->height > height - clip->y)
        return 0;
    if (!clip->width || !clip->height)
        return 1;
    pico_assets_reset_cache(a);
    for (y = clip->y; y < clip->y + clip->height; y++) {
        void *row = (uint8_t *)pixels + (size_t)y * stride * bytes;
        for (x = clip->x; x < clip->x + clip->width; x++)
            if (rgb32)
                ((uint32_t *)row)[x] = bg & 0xffffffu;
            else
                ((uint16_t *)row)[x] = (uint16_t)bg;
    }
    for (n = 0; n < count; n++) {
        const PicoDraw *d = draws + n;
        RasterDraw p;
        RowCursor cur = {-1, 0, 0, 0, 0, 0, 0};
        int ready = prepare_draw(a, d, width, &p), cached = 0;
        int first, last;
        if (ready < 0)
            return 0;
        if (!ready)
            continue;
        first = p.first < (int)clip->x ? (int)clip->x : p.first;
        last = p.last > (int)(clip->x + clip->width) ? (int)(clip->x + clip->width) : p.last;
        if (first >= last)
            continue;
        for (y = clip->y; y < clip->y + clip->height; y++) {
            int64_t sy = ((int64_t)y * 65536 + 32768) * 350 / height;
            void *row;
            if (sy < p.miny || sy >= p.maxy)
                continue;
            if (!cached && a->version == 3 && d->b) {
                cur.cached = cache_leaf(a, p.record, scratch);
                if (!cur.cached && a->cache && a->cache->source == a->data &&
                    u16(p.record + 10) <= a->cache->capacity / 4 / u16(p.record + 8)) {
                    /* Earlier leaves are already composited. No live cursor
                     * refers to their cached pixels, so this arena can be reused. */
                    pico_assets_reset_cache(a);
                    cur.cached = cache_leaf(a, p.record, scratch);
                }
                cached = 1;
            }
            row = (uint8_t *)pixels + (size_t)y * stride * bytes;
            draw_row(a, d, &p, sy, first, last, row, rgb32, &cur, scratch);
        }
    }
    return 1;
}

static PICO_NOINLINE int framebuffer3(const PicoAssets *a, const PicoDraw *draws, unsigned count,
                                      unsigned width, unsigned height, uint32_t bg,
                                      const PicoRect *clip, void *pixels, size_t stride,
                                      int rgb32) {
    uint8_t scratch[PICO_RGBA_MAX_SOURCE_WIDTH * 4];
    return framebuffer_work(a, draws, count, width, height, bg, clip, pixels, stride, rgb32,
                            scratch);
}

static int framebuffer(const PicoAssets *a, const PicoDraw *draws, unsigned count, unsigned width,
                       unsigned height, uint32_t bg, const PicoRect *clip, void *pixels,
                       size_t stride, int rgb32) {
    if (a && a->data && a->version == 3)
        return framebuffer3(a, draws, count, width, height, bg, clip, pixels, stride, rgb32);
    return framebuffer_work(a, draws, count, width, height, bg, clip, pixels, stride, rgb32, 0);
}

int pico_raster_framebuffer(const PicoAssets *a, const PicoDraw *draws, unsigned count,
                            unsigned width, unsigned height, uint16_t bg, uint16_t *pixels,
                            size_t stride) {
    return framebuffer(a, draws, count, width, height, bg, NULL, pixels, stride, 0);
}

int pico_raster_framebuffer32(const PicoAssets *a, const PicoDraw *draws, unsigned count,
                              unsigned width, unsigned height, uint32_t bg, uint32_t *pixels,
                              size_t stride) {
    return framebuffer(a, draws, count, width, height, bg, NULL, pixels, stride, 1);
}

int pico_raster_region(const PicoAssets *a, const PicoDraw *draws, unsigned count, unsigned width,
                       unsigned height, uint16_t bg, const PicoRect *region, uint16_t *pixels,
                       size_t stride) {
    if (!region)
        return 0;
    return framebuffer(a, draws, count, width, height, bg, region, pixels, stride, 0);
}

int pico_raster_region32(const PicoAssets *a, const PicoDraw *draws, unsigned count, unsigned width,
                         unsigned height, uint32_t bg, const PicoRect *region, uint32_t *pixels,
                         size_t stride) {
    if (!region)
        return 0;
    return framebuffer(a, draws, count, width, height, bg, region, pixels, stride, 1);
}

int pico_raster_draw_bounds(const PicoAssets *a, const PicoDraw *d, unsigned width, unsigned height,
                            PicoRect *rect) {
    const uint8_t *r;
    int64_t minx, maxx, miny, maxy, firstx, lastx, firsty, lasty;
    if (!a || !a->data || !d || !rect || !width || width > PICO_RENDER_MAX_WIDTH || !height ||
        d->kind)
        return -1;
    memset(rect, 0, sizeof(*rect));
    r = find_asset(a, d->symbol, d->ratio);
    if (!r || (int64_t)d->a * d->d == (int64_t)d->b * d->c ||
        (d->multiply[3] >= 0 && clamp8(255 * d->multiply[3] / 256 + d->add[3]) == 0))
        return 0;
    bounds(a, r, d, &minx, &maxx, &miny, &maxy);
    if (maxx < 0 || minx > 550 * 65536LL || maxy < 0 || miny > 350 * 65536LL)
        return 0;
    firstx = minx <= 0 ? 0 : minx * width / 550 / 65536 - 1;
    lastx = maxx >= 550 * 65536LL ? width : maxx * width / 550 / 65536 + 1;
    firsty = miny <= 0 ? 0 : miny * height / 350 / 65536 - 1;
    lasty = maxy >= 350 * 65536LL ? height : maxy * height / 350 / 65536 + 1;
    if (firstx < 0)
        firstx = 0;
    if (firsty < 0)
        firsty = 0;
    if (lastx > width)
        lastx = width;
    if (lasty > height)
        lasty = height;
    if (firstx >= lastx || firsty >= lasty)
        return 0;
    rect->x = (unsigned)firstx;
    rect->y = (unsigned)firsty;
    rect->width = (unsigned)(lastx - firstx);
    rect->height = (unsigned)(lasty - firsty);
    return 1;
}

int pico_draw_inverse(const PicoDraw *d, int32_t sx, int32_t sy, int32_t *x, int32_t *y) {
    int64_t det, ia, ib, ic, id, lx, ly;
    if (!d || !x || !y)
        return 0;
    det = (int64_t)d->a * d->d - (int64_t)d->b * d->c;
    if (!det)
        return 0;
    ia = (int64_t)d->d * 4294967296LL / det;
    ib = -(int64_t)d->b * 4294967296LL / det;
    ic = -(int64_t)d->c * 4294967296LL / det;
    id = (int64_t)d->a * 4294967296LL / det;
    lx = (ia * ((int64_t)sx - d->tx) + ic * ((int64_t)sy - d->ty)) / 65536;
    ly = (ib * ((int64_t)sx - d->tx) + id * ((int64_t)sy - d->ty)) / 65536;
    if (lx < INT32_MIN || lx > INT32_MAX || ly < INT32_MIN || ly > INT32_MAX)
        return 0;
    *x = (int32_t)lx;
    *y = (int32_t)ly;
    return 1;
}
int pico_raster_render(const PicoAssets *a, const PicoDraw *draws, unsigned count, unsigned width,
                       unsigned height, uint16_t bg, PicoScanline cb, void *user) {
    uint16_t row[PICO_RENDER_MAX_WIDTH];
    unsigned y;
    if (!cb || !width || width > PICO_RENDER_MAX_WIDTH || !height)
        return 0;
    pico_assets_reset_cache(a);
    for (y = 0; y < height; y++) {
        if (!pico_raster_row(a, draws, count, width, height, y, bg, row))
            return 0;
        cb(user, y, row, width);
    }
    return 1;
}
int pico_raster_render32(const PicoAssets *a, const PicoDraw *draws, unsigned count, unsigned width,
                         unsigned height, uint32_t bg, PicoScanline32 cb, void *user) {
    uint32_t row[PICO_RENDER_MAX_WIDTH];
    unsigned y;
    if (!cb || !width || width > PICO_RENDER_MAX_WIDTH || !height)
        return 0;
    pico_assets_reset_cache(a);
    for (y = 0; y < height; y++) {
        if (!pico_raster_row32(a, draws, count, width, height, y, bg, row))
            return 0;
        cb(user, y, row, width);
    }
    return 1;
}
void pico_rgb565_to_mono(const uint16_t *row, unsigned width, unsigned y, uint8_t *bits) {
    static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    unsigned x;
    for (x = 0; x < (width + 7) / 8; x++)
        bits[x] = 0;
    for (x = 0; x < width; x++) {
        unsigned p = row[x], r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63,
                 b = (p & 31) * 255 / 31;
        unsigned l = (r * 77 + g * 150 + b * 29) >> 8;
        if (l > (unsigned)bayer[(y & 3) * 4 + (x & 3)] * 16 + 7)
            bits[x / 8] |= (uint8_t)(128 >> (x & 7));
    }
}
