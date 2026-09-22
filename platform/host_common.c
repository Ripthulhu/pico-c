#define _POSIX_C_SOURCE 200809L
#include "host_common.h"
#include "game_data.h"
#include "pico_ui.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

double host_now(void) {
#ifdef _WIN32
    return (double)clock() / CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static int map_assets(HostApp *app, const char *path) {
#ifdef _WIN32
    FILE *f = fopen(path, "rb");
    long length;
    void *bytes;
    if (!f || fseek(f, 0, SEEK_END)) {
        if (f)
            fclose(f);
        return 0;
    }
    length = ftell(f);
    if (length <= 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return 0;
    }
    bytes = malloc((size_t)length);
    if (!bytes) {
        fclose(f);
        return 0;
    }
    if (fread(bytes, 1, (size_t)length, f) != (size_t)length) {
        free(bytes);
        fclose(f);
        return 0;
    }
    fclose(f);
    app->asset_bytes = bytes;
    app->asset_size = (size_t)length;
#else
    struct stat st;
    int fd = open(path, O_RDONLY);
    void *bytes;
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) || st.st_size <= 0) {
        close(fd);
        return 0;
    }
    bytes = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (bytes == MAP_FAILED)
        return 0;
    app->asset_bytes = bytes;
    app->asset_size = (size_t)st.st_size;
#endif
    app->owns_asset_bytes = 1;
    return 1;
}

static int hide_corner_logo(const HostApp *app) {
    unsigned caption_height = app->assist && app->height >= 16 ? 8 * host_text_scale(app) : 0;
    return app->width < 550 || app->height - caption_height < 350;
}

static int corner_logo_art(uint16_t symbol, const PicoMatrix *m) {
    if (symbol == 285 || symbol == 286)
        return 1;
    /* Root depth 717 is the logo backing. Other placements of shape 203
     * belong to the health display, map and dialogue panels. */
    return symbol == 203 && m->a == 40991 && !m->b && !m->c && m->d == 13232 && m->tx == 3407872 &&
           m->ty == 530842;
}

#ifndef PICO_STREAM_RENDER
static void damage_draw(HostApp *app, const PicoDraw *draw) {
    PicoRect rect;
    unsigned right, bottom;
    int visible;
    if (!app->retained_valid)
        return;
    visible = pico_raster_draw_bounds(&app->assets, draw, app->view_width, app->view_height, &rect);
    if (visible < 0) {
        app->retained_valid = 0;
        return;
    }
    if (!visible)
        return;
    if (!app->damage.width || !app->damage.height) {
        app->damage = rect;
        return;
    }
    right = app->damage.x + app->damage.width;
    bottom = app->damage.y + app->damage.height;
    if (right < rect.x + rect.width)
        right = rect.x + rect.width;
    if (bottom < rect.y + rect.height)
        bottom = rect.y + rect.height;
    if (app->damage.x > rect.x)
        app->damage.x = rect.x;
    if (app->damage.y > rect.y)
        app->damage.y = rect.y;
    app->damage.width = right - app->damage.x;
    app->damage.height = bottom - app->damage.y;
}

static void damage_changed_draw(HostApp *app, const PicoDraw *draw) {
    unsigned index = app->draw_count, match, n;
    int added = 0;
    if (index >= app->retained_draw_count) {
        app->retained_draw_count = index + 1;
        added = 1;
    } else if (app->draws[index].symbol != draw->symbol) {
        /* Keep the new prefix and the unmatched old suffix in the same list.
         * A blinking map leaf must not make its unchanged neighbours dirty. */
        for (match = index + 1; match < app->retained_draw_count; match++)
            if (app->draws[match].symbol == draw->symbol)
                break;
        if (match < app->retained_draw_count) {
            for (n = index; n < match; n++)
                damage_draw(app, &app->draws[n]);
            memmove(&app->draws[index], &app->draws[match],
                    (app->retained_draw_count - match) * sizeof(*draw));
            app->retained_draw_count -= match - index;
        } else if (app->retained_draw_count < PICO_HOST_MAX_DRAWS) {
            memmove(&app->draws[index + 1], &app->draws[index],
                    (app->retained_draw_count - index) * sizeof(*draw));
            app->retained_draw_count++;
            added = 1;
        } else {
            /* Both lists fit individually, but their temporary overlap may
             * not. Finish collection normally and draw a complete picture. */
            app->retained_valid = 0;
        }
    }
    if (added) {
        damage_draw(app, draw);
    } else if (app->retained_valid && memcmp(draw, &app->draws[index], sizeof(*draw))) {
        damage_draw(app, &app->draws[index]);
        damage_draw(app, draw);
    }
}
#endif

static void draw_callback(void *user, uint16_t symbol, const PicoMatrix *m, const PicoColor *c,
                          uint16_t ratio, uint8_t kind) {
    HostApp *app = (HostApp *)user;
    PicoDraw temporary;
    PicoDraw *draw = &temporary;
    const char *text;
    if (corner_logo_art(symbol, m) && hide_corner_logo(app))
        return;
    if (!app->reading && c->mul[3]
#ifdef PICO_STREAM_RENDER
        /* All rows visit the same scene. Discover reader text only once. */
        && (!app->scanline || app->scanline_y == 0)
#endif
    ) {
        text = pico_text_for_symbol(symbol);
        if (text && (!app->visible_text || strlen(text) > strlen(app->visible_text)))
            app->visible_text = text;
    }
    if (kind != PICO_DRAW_LEAF) {
        app->mask_events++;
        return;
    }
#ifdef PICO_STREAM_RENDER
    app->draw_count++;
#else
    if (app->draw_count == PICO_HOST_MAX_DRAWS) {
        app->draw_overflows++;
        return;
    }
    /* Include padding in the stable byte comparison below. */
    memset(draw, 0, sizeof(*draw));
#endif
    draw->a = m->a;
    draw->b = m->b;
    draw->c = m->c;
    draw->d = m->d;
    draw->tx = m->tx;
    draw->ty = m->ty;
    memcpy(draw->multiply, c->mul, sizeof(draw->multiply));
    memcpy(draw->add, c->add, sizeof(draw->add));
    draw->symbol = symbol;
    draw->ratio = ratio;
    draw->kind = kind;
#ifdef PICO_STREAM_RENDER
    if (app->scanline) {
        if (app->assets.version >= 2)
            pico_raster_blit_row32(&app->assets, draw, app->view_width, app->view_height,
                                   app->scanline_y, (uint32_t *)app->scanline);
        else
            pico_raster_blit_row(&app->assets, draw, app->view_width, app->view_height,
                                 app->scanline_y, (uint16_t *)app->scanline);
    }
#else
    if (app->retained_valid)
        damage_changed_draw(app, draw);
    memcpy(&app->draws[app->draw_count++], draw, sizeof(*draw));
#endif
}

static int hit_callback(void *user, uint16_t symbol, const PicoMatrix *m, int32_t x, int32_t y) {
    HostApp *app = (HostApp *)user;
    PicoDraw draw;
    int32_t local_x, local_y;
    if (symbol == 287 && hide_corner_logo(app))
        return 0;
    memset(&draw, 0, sizeof(draw));
    draw.a = m->a;
    draw.b = m->b;
    draw.c = m->c;
    draw.d = m->d;
    draw.tx = m->tx;
    draw.ty = m->ty;
    if (!pico_draw_inverse(&draw, x, y, &local_x, &local_y))
        return 0;
    return pico_asset_hit(&app->assets, symbol, 0, local_x, local_y);
}

static void sound_callback(void *user, const PicoSoundEvent *event) {
    HostApp *app = (HostApp *)user;
    app->sound_events++;
    if (app->sound.event)
        app->sound.event(app->sound.user, event);
    else
        app->silent_sound_events++;
}
static void stop_callback(void *user) {
    HostApp *app = (HostApp *)user;
    app->stopped_sounds++;
    if (app->sound.stop)
        app->sound.stop(app->sound.user);
}
static void medal_callback(void *user, const char *name) {
    (void)name;
    ((HostApp *)user)->medals++;
}

static int valid_size(unsigned width, unsigned height) {
    return width && width <= PICO_RENDER_MAX_WIDTH && height && height <= PICO_HOST_MAX_HEIGHT;
}

unsigned host_pixel_bytes(const HostApp *app) {
    return app->assets.version >= 2 ? 4 : 2;
}

uint32_t host_pixel_rgb(const HostApp *app, size_t index) {
    uint16_t pixel;
    if (app->assets.version >= 2)
        return ((const uint32_t *)app->pixels)[index];
    pixel = ((const uint16_t *)app->pixels)[index];
    return ((((pixel >> 11) & 31) * 255 / 31) << 16) | ((((pixel >> 5) & 63) * 255 / 63) << 8) |
           ((pixel & 31) * 255 / 31);
}

unsigned host_text_scale(const HostApp *app) {
    unsigned scale = app->width / 160;
    if (scale > app->height / 96)
        scale = app->height / 96;
    return scale < 1 ? 1 : scale > 8 ? 8 : scale;
}

unsigned host_reader_pages(const HostApp *app) {
    unsigned scale = host_text_scale(app);
    return (unsigned)pico_ui_pages(app->visible_text ? app->visible_text : "NO TEXT IN THIS VIEW",
                                   (int)(app->width / scale), (int)(app->height / scale / 8));
}

int host_resize(HostApp *app, unsigned width, unsigned height) {
    void *pixels;
    unsigned pages;
    if (!valid_size(width, height))
        return 0;
    if (app->pixels && app->width == width && app->height == height)
        return 1;
    pixels = calloc((size_t)width * height, host_pixel_bytes(app));
    if (!pixels)
        return 0;
    /* Keep the old framebuffer usable if allocation fails. */
    free(app->pixels);
    app->pixels = pixels;
    app->width = width;
    app->height = height;
#ifndef PICO_STREAM_RENDER
    app->retained_valid = 0;
#endif
    if (app->assist_auto)
        app->assist = width <= 128 && height >= 16;
    pages = host_reader_pages(app);
    if (app->reader_page >= pages)
        app->reader_page = pages - 1;
    return 1;
}

int host_open(HostApp *app, const char *assets, unsigned width, unsigned height,
              unsigned start_frame, int mono) {
    return host_open_with_sound(app, assets, width, height, start_frame, mono, NULL);
}

static int finish_open(HostApp *app, unsigned width, unsigned height, unsigned start_frame,
                       int mono) {
    PicoHost host;
    if (!valid_size(width, height)) {
        fprintf(stderr, "Unsupported output dimensions (width 1..%u, height 1..%u).\n",
                (unsigned)PICO_RENDER_MAX_WIDTH, (unsigned)PICO_HOST_MAX_HEIGHT);
        host_close(app);
        return 0;
    }
    if (!pico_assets_init(&app->assets, app->asset_bytes, app->asset_size)) {
        fprintf(stderr, "Invalid asset pack.\n");
        host_close(app);
        return 0;
    }
#ifndef PICO_STREAM_RENDER
    if (app->assets.version == 3) {
        size_t capacity =
            app->assets.scale_num <= app->assets.scale_den ? 512u * 1024 : 2u * 1024 * 1024;
        PicoAssetCache *cache = (PicoAssetCache *)malloc(sizeof(*cache) + capacity);
        if (cache) {
            if (pico_assets_bind_cache(&app->assets, cache, cache + 1, capacity))
                app->asset_cache_memory = cache;
            else
                free(cache);
        }
    }
#endif
    app->assist_auto = 1;
    if (!host_resize(app, width, height)) {
        host_close(app);
        return 0;
    }
    app->mono = mono != 0;
    app->selected = PICO_NONE;
    memset(&host, 0, sizeof(host));
    host.user = app;
    host.draw = draw_callback;
    host.hit = hit_callback;
    host.sound = sound_callback;
    host.stop_sounds = stop_callback;
    host.medal = medal_callback;
    if (!pico_init(&app->game, &pico_game_data, &host, (uint16_t)start_frame)) {
        fprintf(stderr, "Game initialization failed (errors=%u).\n", app->game.errors);
        host_close(app);
        return 0;
    }
    return 1;
}

int host_open_with_sound(HostApp *app, const char *assets, unsigned width, unsigned height,
                         unsigned start_frame, int mono, const HostSoundSink *sound) {
    memset(app, 0, sizeof(*app));
    if (sound)
        app->sound = *sound;
    if (!map_assets(app, assets)) {
        fprintf(stderr, "Cannot open asset pack: %s\n", assets);
        return 0;
    }
    return finish_open(app, width, height, start_frame, mono);
}

int host_open_memory(HostApp *app, const void *bytes, size_t size, unsigned width, unsigned height,
                     unsigned start_frame, int mono, const HostSoundSink *sound) {
    memset(app, 0, sizeof(*app));
    app->asset_bytes = bytes;
    app->asset_size = size;
    if (sound)
        app->sound = *sound;
    return finish_open(app, width, height, start_frame, mono);
}

void host_close(HostApp *app) {
    free(app->pixels);
    app->pixels = NULL;
#ifndef PICO_STREAM_RENDER
    app->retained_valid = 0;
    app->retained_pixels = NULL;
    free(app->asset_cache_memory);
    app->asset_cache_memory = NULL;
    app->assets.cache = NULL;
#endif
    if (app->asset_bytes && app->owns_asset_bytes) {
#ifdef _WIN32
        free((void *)app->asset_bytes);
#else
        munmap((void *)app->asset_bytes, app->asset_size);
#endif
    }
    app->asset_bytes = NULL;
    app->owns_asset_bytes = 0;
}

static void row_callback(void *user, unsigned y, const uint16_t *row, unsigned width) {
    HostApp *app = (HostApp *)user;
    uint16_t *out = (uint16_t *)app->pixels + (size_t)(y + app->view_y) * app->width + app->view_x;
    unsigned x;
    if (!app->mono) {
        memcpy(out, row, width * sizeof(uint16_t));
        return;
    }
    for (x = 0; x < width; x++) {
        static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        uint16_t p = row[x];
        unsigned r = ((p >> 11) & 31) * 255 / 31;
        unsigned g = ((p >> 5) & 63) * 255 / 63;
        unsigned b = (p & 31) * 255 / 31;
        unsigned luminance = (r * 77 + g * 150 + b * 29) >> 8;
        out[x] = luminance > (unsigned)bayer[(y & 3) * 4 + (x & 3)] * 16 + 8 ? 65535 : 0;
    }
}

static void row_callback32(void *user, unsigned y, const uint32_t *row, unsigned width) {
    HostApp *app = (HostApp *)user;
    uint32_t *out = (uint32_t *)app->pixels + (size_t)(y + app->view_y) * app->width + app->view_x;
    unsigned x;
    if (!app->mono) {
        memcpy(out, row, width * sizeof(uint32_t));
        return;
    }
    for (x = 0; x < width; x++) {
        static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        uint32_t p = row[x];
        unsigned luminance =
            (((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8;
        out[x] = luminance > (unsigned)bayer[(y & 3) * 4 + (x & 3)] * 16 + 8 ? 0xffffff : 0;
    }
}

static void text_overlay(HostApp *app, unsigned top, unsigned height, const char *text,
                         unsigned page, unsigned rows_per_page) {
    uint16_t row[PICO_RENDER_MAX_WIDTH];
    unsigned scale = host_text_scale(app), width = app->width / scale, y, x;
    for (y = 0; y < height; y++) {
        if (y % scale == 0)
            pico_ui_text_row(row, (int)width, (int)(y / scale), text, (int)page,
                             (int)rows_per_page);
        for (x = 0; x < app->width; x++) {
            uint16_t pixel = x / scale < width ? row[x / scale] : 0;
            size_t index = (size_t)(top + y) * app->width + x;
            if (app->assets.version >= 2)
                ((uint32_t *)app->pixels)[index] = pixel ? 0xffffff : 0;
            else
                ((uint16_t *)app->pixels)[index] = pixel;
        }
    }
}

#ifdef PICO_STREAM_RENDER
/* Separate call frames keep a version-1 render on its original 16-bit row. */
#if defined(__GNUC__)
#define HOST_ROW_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define HOST_ROW_NOINLINE __declspec(noinline)
#else
#define HOST_ROW_NOINLINE
#endif
static HOST_ROW_NOINLINE void render_stream16(HostApp *app) {
    uint16_t row[PICO_RENDER_MAX_WIDTH];
    unsigned y;
    app->scanline = row;
    for (y = 0; y < app->view_height; y++) {
        memset(row, 0, app->view_width * sizeof(uint16_t));
        app->draw_count = 0;
        app->scanline_y = y;
        pico_render(&app->game);
        row_callback(app, y, row, app->view_width);
    }
    app->scanline = NULL;
}

static HOST_ROW_NOINLINE void render_stream32(HostApp *app) {
    uint32_t row[PICO_RENDER_MAX_WIDTH];
    unsigned y;
    app->scanline = row;
    for (y = 0; y < app->view_height; y++) {
        memset(row, 0, app->view_width * sizeof(uint32_t));
        app->draw_count = 0;
        app->scanline_y = y;
        pico_render(&app->game);
        row_callback32(app, y, row, app->view_width);
    }
    app->scanline = NULL;
}
#undef HOST_ROW_NOINLINE
#endif

int host_render(HostApp *app) {
    double begin = host_now();
    int ok;
    unsigned scale = host_text_scale(app);
    unsigned caption_height = app->assist && app->height >= 16 ? 8 * scale : 0;
    unsigned available_height = app->height - caption_height;
#ifndef PICO_STREAM_RENDER
    unsigned previous_width = app->view_width, previous_height = app->view_height;
    unsigned previous_x = app->view_x, previous_y = app->view_y;
    unsigned previous_overflows = app->draw_overflows, i;
#endif
    char caption[128];
    app->draw_count = 0;
    if (!app->reading)
        app->visible_text = NULL;
    app->view_width = app->width;
    app->view_height = app->width * 350 / 550;
    if (app->view_height > available_height) {
        app->view_height = available_height;
        app->view_width = available_height * 550 / 350;
    }
    if (!app->view_width)
        app->view_width = 1;
    if (!app->view_height)
        app->view_height = 1;
    app->view_x = (app->width - app->view_width) / 2;
    app->view_y = (available_height - app->view_height) / 2;
#ifdef PICO_STREAM_RENDER
    memset(app->pixels, 0, (size_t)app->width * app->height * host_pixel_bytes(app));
    if (app->assets.version >= 2)
        render_stream32(app);
    else
        render_stream16(app);
    ok = 1;
#else
    if (app->reading || app->assist || app->mono || app->retained_pixels != app->pixels ||
        previous_width != app->view_width || previous_height != app->view_height ||
        previous_x != app->view_x || previous_y != app->view_y)
        app->retained_valid = 0;
    memset(&app->damage, 0, sizeof(app->damage));
    pico_render(&app->game);
    /* Removed leaves still occupy their old slots after collection. Include
     * them before the list becomes the reference for the next picture. */
    for (i = app->draw_count; i < app->retained_draw_count; i++)
        damage_draw(app, &app->draws[i]);
    if (!app->retained_valid)
        memset(app->pixels, 0, (size_t)app->width * app->height * host_pixel_bytes(app));
    if (app->retained_valid && !app->damage.width)
        ok = 1;
    else if (app->retained_valid && app->assets.version >= 2)
        ok = pico_raster_region32(
            &app->assets, app->draws, app->draw_count, app->view_width, app->view_height, 0,
            &app->damage, (uint32_t *)app->pixels + (size_t)app->view_y * app->width + app->view_x,
            app->width);
    else if (app->retained_valid)
        ok = pico_raster_region(
            &app->assets, app->draws, app->draw_count, app->view_width, app->view_height, 0,
            &app->damage, (uint16_t *)app->pixels + (size_t)app->view_y * app->width + app->view_x,
            app->width);
    else if (!app->mono && app->assets.version >= 2)
        ok = pico_raster_framebuffer32(
            &app->assets, app->draws, app->draw_count, app->view_width, app->view_height, 0,
            (uint32_t *)app->pixels + (size_t)app->view_y * app->width + app->view_x, app->width);
    else if (!app->mono)
        ok = pico_raster_framebuffer(
            &app->assets, app->draws, app->draw_count, app->view_width, app->view_height, 0,
            (uint16_t *)app->pixels + (size_t)app->view_y * app->width + app->view_x, app->width);
    else if (app->assets.version >= 2)
        ok = pico_raster_render32(&app->assets, app->draws, app->draw_count, app->view_width,
                                  app->view_height, 0, row_callback32, app);
    else
        ok = pico_raster_render(&app->assets, app->draws, app->draw_count, app->view_width,
                                app->view_height, 0, row_callback, app);
    app->retained_draw_count = app->draw_count;
    app->retained_pixels = app->pixels;
    app->retained_valid = ok && previous_overflows == app->draw_overflows && !app->reading &&
                          !app->assist && !app->mono;
#endif
    if (app->draw_count > app->peak_draws)
        app->peak_draws = app->draw_count;
    host_select(app, 0);
    if (app->reading) {
        const char *text =
            app->visible_text && app->visible_text[0] ? app->visible_text : "NO TEXT IN THIS VIEW";
        text_overlay(app, 0, app->height, text, app->reader_page, app->height / scale / 8);
    } else if (caption_height) {
        unsigned pages, page;
        host_caption(app, app->selected, caption, sizeof(caption));
        pages = (unsigned)pico_ui_pages(caption, (int)(app->width / scale), 1);
        page = pages ? ((app->game.tick - app->caption_epoch) / 48) % pages : 0;
        text_overlay(app, app->height - caption_height, caption_height, caption, page, 1);
    }
    app->render_seconds += host_now() - begin;
    app->render_count++;
    return ok;
}

void host_tick(HostApp *app) {
    double begin = host_now(), elapsed;
    pico_tick(&app->game);
    elapsed = host_now() - begin;
    app->tick_seconds += elapsed;
    if (elapsed > app->worst_tick_seconds)
        app->worst_tick_seconds = elapsed;
}

uint32_t host_frame_hash(const HostApp *app) {
    size_t n = (size_t)app->width * app->height, i;
    uint32_t h = 2166136261u;
    for (i = 0; i < n; i++) {
        unsigned byte, bytes = host_pixel_bytes(app);
        uint32_t pixel = bytes == 4 ? host_pixel_rgb(app, i) : ((const uint16_t *)app->pixels)[i];
        for (byte = 0; byte < bytes; byte++)
            h = (h ^ (uint8_t)(pixel >> (byte * 8))) * 16777619u;
    }
    return h;
}

int host_ppm(const HostApp *app, const char *path) {
    FILE *f = fopen(path, "wb");
    size_t i, n = (size_t)app->width * app->height;
    int ok;
    if (!f)
        return 0;
    fprintf(f, "P6\n%u %u\n255\n", app->width, app->height);
    for (i = 0; i < n; i++) {
        uint32_t p = host_pixel_rgb(app, i);
        uint8_t rgb[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
        if (fwrite(rgb, 1, 3, f) != 3) {
            fclose(f);
            return 0;
        }
    }
    ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

int host_pbm(const HostApp *app, const char *path) {
    FILE *f = fopen(path, "wb");
    unsigned x, y;
    int ok;
    if (!f)
        return 0;
    fprintf(f, "P4\n%u %u\n", app->width, app->height);
    for (y = 0; y < app->height; y++) {
        uint8_t packed = 0;
        for (x = 0; x < app->width; x++) {
            if (!host_pixel_rgb(app, (size_t)y * app->width + x))
                packed |= (uint8_t)(0x80 >> (x & 7));
            if ((x & 7) == 7 || x + 1 == app->width) {
                fputc(packed, f);
                packed = 0;
            }
        }
    }
    ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

void host_metrics(const HostApp *app, FILE *out) {
    const Pico *p = &app->game;
    size_t draw_bytes = 0;
    size_t cache_capacity = 0, cache_used = 0, cache_metadata = 0;
    unsigned draw_capacity = 0;
#ifndef PICO_STREAM_RENDER
    draw_bytes = sizeof(app->draws);
    draw_capacity = PICO_HOST_MAX_DRAWS;
    if (app->assets.cache) {
        cache_capacity = app->assets.cache->capacity;
        cache_used = app->assets.cache->used;
        cache_metadata = sizeof(PicoAssetCache);
    }
#endif
    fprintf(out,
            "{\n  \"width\": %u, \"height\": %u, \"monochrome\": %s,\n"
            "  \"ticks\": %u, \"root_frame\": %u, \"frames_rendered\": %u,\n"
            "  \"frame_hash_fnv1a\": \"%08x\",\n"
            "  \"runtime_bytes\": %zu, \"draw_list_capacity_bytes\": %zu,\n"
            "  \"host_framebuffer_bytes\": %zu, \"asset_rom_bytes\": %zu,\n"
            "  \"host_pixel_format\": \"%s\", \"asset_pack_version\": %u,\n"
            "  \"packed_mono_frame_bytes\": %zu, \"render_scanline_bytes\": %u,\n"
            "  \"asset_decode_row_bytes\": %u,\n"
            "  \"asset_cache_capacity_bytes\": %zu, \"asset_cache_used_bytes\": %zu,\n"
            "  \"asset_cache_metadata_bytes\": %zu,\n"
            "  \"live_instances\": %u, \"peak_instances\": %u, \"instance_capacity\": %u,\n"
            "  \"peak_queue\": %u, \"peak_draws\": %u, \"draw_capacity\": %u,\n"
            "  \"runtime_errors\": %u, \"draw_overflows\": %u, \"mask_events_unimplemented\": %u,\n"
            "  \"missing_targets\": %u, \"missing_labels\": %u, \"actions_executed\": %u,\n"
            "  \"sound_events\": %u, \"sound_events_silent\": %u, \"medals\": %u,\n"
            "  \"tick_mean_us\": %.3f, \"tick_worst_us\": %.3f, \"render_mean_us\": %.3f\n}\n",
            app->width, app->height, app->mono ? "true" : "false", p->tick, p->instances[0].frame,
            app->render_count, host_frame_hash(app), sizeof(Pico), draw_bytes,
            (size_t)app->width * app->height * host_pixel_bytes(app), app->asset_size,
            app->assets.version >= 2 ? "xrgb8888" : "rgb565", (unsigned)app->assets.version,
            (size_t)((app->width + 7) / 8) * app->height,
            (unsigned)PICO_RENDER_MAX_WIDTH * host_pixel_bytes(app),
            app->assets.version == 3 ? 4u * PICO_RGBA_MAX_SOURCE_WIDTH : 0u, cache_capacity,
            cache_used, cache_metadata, p->live_instances, p->peak_instances,
            (unsigned)PICO_MAX_INSTANCES, p->peak_queue, app->peak_draws, draw_capacity, p->errors,
            app->draw_overflows, app->mask_events, p->missing_targets, p->missing_labels,
            p->actions_executed, app->sound_events, app->silent_sound_events, app->medals,
            p->tick ? app->tick_seconds * 1e6 / p->tick : 0, app->worst_tick_seconds * 1e6,
            app->render_count ? app->render_seconds * 1e6 / app->render_count : 0);
}

void host_instance_path(const HostApp *app, uint16_t instance, char *out, size_t capacity) {
    uint16_t parents[PICO_MAX_NESTING];
    unsigned count = 0;
    size_t used = 0;
    if (!capacity)
        return;
    out[0] = 0;
    while (instance != PICO_NONE && instance < PICO_MAX_INSTANCES && count < PICO_MAX_NESTING) {
        parents[count++] = instance;
        instance = app->game.instances[instance].parent;
    }
    while (count && used + 1 < capacity) {
        uint16_t index = parents[--count];
        const PicoInstance *i = &app->game.instances[index];
        const char *name = NULL;
        int written;
        if (index == 0)
            name = "_root";
        else if (i->placement < app->game.data->placement_count) {
            unsigned name_index = app->game.data->placements[i->placement].name;
            if (name_index < app->game.data->string_count)
                name = app->game.data->strings[name_index];
        }
        if (name && name[0])
            written = snprintf(out + used, capacity - used, "%s%s", used ? "/" : "", name);
        else
            written = snprintf(out + used, capacity - used, "/#%u:%u", i->symbol, index);
        if (written < 0)
            break;
        if ((size_t)written >= capacity - used) {
            used = capacity - 1;
            break;
        }
        used += (size_t)written;
    }
}

static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c < 32)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

void host_state(const HostApp *app, FILE *out) {
    unsigned i, printed = 0;
    fputs("[\n", out);
    for (i = 0; i < PICO_MAX_INSTANCES; i++) {
        char path[512];
        const PicoInstance *instance = &app->game.instances[i];
        if (!instance->alive)
            continue;
        host_instance_path(app, (uint16_t)i, path, sizeof(path));
        if (printed++)
            fputs(",\n", out);
        fprintf(out, "  {\"handle\":%u,\"symbol\":%u,\"frame\":%u,\"playing\":%s,\"path\":", i,
                instance->symbol, instance->frame, instance->playing ? "true" : "false");
        json_string(out, path);
        fputc('}', out);
    }
    fputs("\n]\n", out);
}

typedef struct ButtonList {
    uint16_t *buttons;
    unsigned capacity, count;
    int hide_logo;
} ButtonList;
static void button_visitor(void *user, uint16_t instance, uint16_t symbol,
                           const PicoMatrix *world) {
    ButtonList *list = (ButtonList *)user;
    (void)world;
    if (symbol == 288 && list->hide_logo)
        return;
    if (list->count < list->capacity)
        list->buttons[list->count++] = instance;
}
unsigned host_buttons(const HostApp *app, uint16_t *buttons, unsigned capacity) {
    ButtonList list = {buttons, capacity, 0, hide_corner_logo(app)};
    pico_buttons(&app->game, button_visitor, &list);
    return list.count;
}

void host_caption(const HostApp *app, uint16_t instance, char *out, size_t capacity) {
    if (instance == PICO_NONE || instance >= PICO_MAX_INSTANCES)
        snprintf(out, capacity, "TAB SELECT");
    else
        snprintf(out, capacity, "%s", pico_button_label(app->game.instances[instance].symbol));
}

void host_select(HostApp *app, int direction) {
    uint16_t buttons[PICO_MAX_INSTANCES];
    unsigned count = host_buttons(app, buttons, PICO_MAX_INSTANCES), i, current = 0;
    if (!count) {
        app->selected = PICO_NONE;
        return;
    }
    for (i = 0; i < count; i++) {
        if (buttons[i] == app->selected &&
            app->game.instances[buttons[i]].generation == app->selected_generation) {
            current = (unsigned)((int)i + (direction < 0 ? (int)count - 1 : direction)) % count;
            break;
        }
    }
    if (app->selected != buttons[current] ||
        app->selected_generation != app->game.instances[buttons[current]].generation)
        app->caption_epoch = app->game.tick;
    app->selected = buttons[current];
    app->selected_generation = app->game.instances[app->selected].generation;
}

void host_activate(HostApp *app) {
    uint16_t instance = app->selected;
    uint32_t generation = app->selected_generation;
    if (instance == PICO_NONE || instance >= PICO_MAX_INSTANCES ||
        !app->game.instances[instance].alive ||
        app->game.instances[instance].generation != generation) {
        host_select(app, 0);
        return;
    }
    pico_button(&app->game, instance, 1);
    if (app->game.instances[instance].alive &&
        app->game.instances[instance].generation == generation)
        pico_button(&app->game, instance, 0);
    host_select(app, 0);
}
