#include "host_common.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HostApp app;
static Pico saved_game;
static const char long_text[] =
    "THE GAME SHOULD KEEP ITS PLACE WHEN THE WINDOW CHANGES SIZE. "
    "A SMALL DISPLAY NEEDS SEVERAL PAGES OF TEXT. A LARGE DISPLAY CAN SHOW MORE. "
    "THE SELECTED ACTION AND THE CURRENT ROOM MUST SURVIVE EVERY CHANGE. "
    "READING THIS TEXT SHOULD NOT ADVANCE THE GAME OR REPLACE ITS CONTROLLERS. ";

/* An interior buffer catches writes into the letterbox margins or past the
 * allocation. Restore the owned allocation before resize or close frees it. */
static void guarded_render(void) {
    enum { GUARD = 32 };
    size_t count = (size_t)app.width * app.height * host_pixel_bytes(&app), i;
    void *owned = app.pixels;
    uint8_t *guarded = (uint8_t *)malloc(count + GUARD * 2);
    assert(guarded);
    for (i = 0; i < count + GUARD * 2; i++)
        guarded[i] = 0xa5;
    app.pixels = guarded + GUARD;
#ifndef PICO_STREAM_RENDER
    /* This fresh allocation has no previous picture to retain, even when
     * malloc reuses the address of the last temporary buffer. */
    app.retained_valid = 0;
#endif
    assert(host_render(&app));
    for (i = 0; i < GUARD; i++) {
        assert(guarded[i] == 0xa5);
        assert(guarded[GUARD + count + i] == 0xa5);
    }
    memcpy(owned, app.pixels, count);
    app.pixels = owned;
    free(guarded);
    assert(!app.game.errors);
    assert(!app.draw_overflows);
}

static void check_view(void) {
    unsigned reserved = app.assist ? 8 * host_text_scale(&app) : 0;
    unsigned available = app.height - reserved, x, y;
    long aspect_error = (long)app.view_width * 350 - (long)app.view_height * 550;
    assert(app.view_width > 0 && app.view_height > 0);
    assert(app.view_width <= app.width && app.view_height <= available);
    assert(app.view_width == app.width || app.view_height == available);
    assert(aspect_error > -550 && aspect_error < 550);
    assert(app.view_x == (app.width - app.view_width) / 2);
    assert(app.view_y == (available - app.view_height) / 2);
    for (y = 0; y < available; y++) {
        for (x = 0; x < app.width; x++) {
            if (x < app.view_x || x >= app.view_x + app.view_width || y < app.view_y ||
                y >= app.view_y + app.view_height)
                assert(host_pixel_rgb(&app, (size_t)y * app.width + x) == 0);
        }
    }
}

static void check_cache(void) {
#ifdef PICO_STREAM_RENDER
    assert(!app.assets.cache);
#else
    PicoAssetCache *cache = app.assets.cache;
    if (app.assets.version != 3) {
        assert(!cache && !app.asset_cache_memory);
    } else {
        uint32_t hash;
        size_t capacity;
        assert(cache && app.asset_cache_memory == cache);
        capacity = cache->capacity;
        assert(capacity ==
               (app.assets.scale_num <= app.assets.scale_den ? 512u * 1024 : 2u * 1024 * 1024));
        guarded_render();
        hash = host_frame_hash(&app);
        assert(cache->used <= capacity);
        /* A cache too small for one pixel must still render the same frame. */
        cache->capacity = 1;
        guarded_render();
        assert(host_frame_hash(&app) == hash);
        assert(!cache->used && !cache->count);
        cache->capacity = capacity;
        app.assets.cache = NULL;
        guarded_render();
        assert(host_frame_hash(&app) == hash);
        app.assets.cache = cache;
        guarded_render();
        assert(host_frame_hash(&app) == hash);
    }
#endif
}

static void resize_preserves_game(unsigned width, unsigned height) {
    uint16_t selected = app.selected;
    uint32_t generation = app.selected_generation;
    const void *assets = app.asset_bytes;
    saved_game = app.game;
    assert(host_resize(&app, width, height));
    assert(app.width == width && app.height == height);
    assert(app.asset_bytes == assets);
    assert(!memcmp(&saved_game, &app.game, sizeof(saved_game)));
    assert(app.selected == selected && app.selected_generation == generation);
    guarded_render();
    assert(!memcmp(&saved_game, &app.game, sizeof(saved_game)));
    assert(app.selected == selected && app.selected_generation == generation);
    check_view();
}

static void reject_unchanged(unsigned width, unsigned height) {
    HostApp previous = app;
    uint32_t pixels = host_frame_hash(&app);
    assert(!host_resize(&app, width, height));
    assert(!memcmp(&previous, &app, sizeof(app)));
    assert(host_frame_hash(&app) == pixels);
}

static void check_reader_blocks(void) {
    unsigned scale = host_text_scale(&app), x, y, dx, dy, white = 0;
    assert(scale >= 1);
    app.reading = 1;
    app.visible_text = "A";
    app.reader_page = 0;
    saved_game = app.game;
    guarded_render();
    assert(!memcmp(&saved_game, &app.game, sizeof(saved_game)));
    /* The single glyph makes a failure to scale either axis observable. */
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 6; x++) {
            uint32_t pixel = host_pixel_rgb(&app, (size_t)y * scale * app.width + x * scale);
            assert(pixel == 0 || pixel == 0xffffff);
            white += pixel != 0;
            for (dy = 0; dy < scale; dy++)
                for (dx = 0; dx < scale; dx++)
                    assert(host_pixel_rgb(&app, (size_t)(y * scale + dy) * app.width + x * scale +
                                                    dx) == pixel);
        }
    }
    assert(white > 0 && white < 35);
    app.reading = 0;
}

static void check_caption_blocks(void) {
    unsigned scale, x, y, dx, dy, white = 0, band_y;
    app.assist_auto = 0;
    app.assist = 1;
    guarded_render();
    scale = host_text_scale(&app);
    band_y = app.height - scale * 8;
    for (y = 0; y < 8; y++) {
        for (x = 0; x < app.width / scale; x++) {
            uint32_t pixel =
                host_pixel_rgb(&app, (size_t)(band_y + y * scale) * app.width + x * scale);
            assert(pixel == 0 || pixel == 0xffffff);
            white += pixel != 0;
            for (dy = 0; dy < scale; dy++)
                for (dx = 0; dx < scale; dx++)
                    assert(host_pixel_rgb(&app, (size_t)(band_y + y * scale + dy) * app.width +
                                                    x * scale + dx) == pixel);
        }
    }
    assert(white > 0);
    check_view();
    app.assist_auto = 1;
}

int main(int argc, char **argv) {
    unsigned i, tiny_scale;
    assert(argc == 2);
    assert(host_open(&app, argv[1], 96, 64, 2, 0));
    assert(host_pixel_bytes(&app) == (app.assets.version >= 2 ? 4u : 2u));
    assert(app.assist_auto && app.assist);
    for (i = 0; i < 24; i++)
        host_tick(&app);
    host_select(&app, 1);
    assert(app.selected != PICO_NONE);
    guarded_render();
    check_view();
    check_cache();
    tiny_scale = host_text_scale(&app);
    check_reader_blocks();
    check_caption_blocks();

#if PICO_RENDER_MAX_WIDTH >= 1920
    resize_preserves_game(550, 350);
    assert(!app.assist);
    assert(app.view_width == 550 && app.view_height == 350);
    resize_preserves_game(1920, 1080);
    assert(!app.assist);
    assert(host_text_scale(&app) > tiny_scale);
    check_reader_blocks();
    check_caption_blocks();
    resize_preserves_game(350, 550);
    assert(!app.assist);
    resize_preserves_game(96, 64);
#else
    (void)tiny_scale;
    reject_unchanged(550, 350);
    reject_unchanged(1920, 1080);
    resize_preserves_game(128, 256);
    resize_preserves_game(96, 64);
#endif
    assert(app.assist);
    app.assist_auto = 0;
    app.assist = 0;
    resize_preserves_game(64, 32);
    assert(!app.assist);
    app.assist_auto = 1;
    resize_preserves_game(96, 64);
    assert(app.assist);

    app.reading = 1;
    app.visible_text = long_text;
    assert(host_reader_pages(&app) > 1);
    app.reader_page = UINT_MAX;
    assert(host_resize(&app, 64, 32));
    assert(app.reading && app.visible_text == long_text);
    assert(app.reader_page < host_reader_pages(&app));
    saved_game = app.game;
    guarded_render();
    assert(!memcmp(&saved_game, &app.game, sizeof(saved_game)));
    app.reading = 0;
    resize_preserves_game(96, 64);

    reject_unchanged(0, 64);
    reject_unchanged(96, 0);
    reject_unchanged(PICO_RENDER_MAX_WIDTH + 1u, 64);
    reject_unchanged(96, PICO_HOST_MAX_HEIGHT + 1u);
    reject_unchanged(UINT_MAX, UINT_MAX);
    assert(host_resize(&app, 1, 1));
    assert(!app.assist);
    guarded_render();
    resize_preserves_game(96, 64);
    app.mono = 1;
    guarded_render();
    for (i = 0; i < app.width * app.height; i++)
        assert(host_pixel_rgb(&app, i) == 0 || host_pixel_rgb(&app, i) == 0xffffff);
    host_close(&app);
    puts("Resizing preserves game state, fits the stage and scales readable text.");
    return 0;
}
