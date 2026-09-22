#include "game_data.h"
#include "host_common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STOP_ALL = 65535 };
typedef struct SoundLog {
    unsigned sequence[1024];
    unsigned count, theme_starts;
} SoundLog;

static HostApp app;
static SoundLog sound;
static unsigned filter_logo, filter_title, logo_draws, title_draws;
static void (*host_draw)(void *, uint16_t, const PicoMatrix *, const PicoColor *, uint16_t,
                         uint8_t);

static void sound_event(void *user, const PicoSoundEvent *event) {
    SoundLog *log = (SoundLog *)user;
    assert(log->count < sizeof(log->sequence) / sizeof(log->sequence[0]));
    log->sequence[log->count++] = event->symbol;
    if (event->symbol == 992) {
        assert(!event->stop && event->loops == 1000);
        assert(event->envelope_count == 2 && event->envelope);
        log->theme_starts++;
    }
}

static void stop_sounds(void *user) {
    SoundLog *log = (SoundLog *)user;
    assert(log->count < sizeof(log->sequence) / sizeof(log->sequence[0]));
    log->sequence[log->count++] = STOP_ALL;
}

static uint16_t find_symbol(unsigned symbol) {
    unsigned i;
    for (i = 0; i < PICO_MAX_INSTANCES; i++)
        if (app.game.instances[i].alive && app.game.instances[i].symbol == symbol)
            return (uint16_t)i;
    return PICO_NONE;
}

static void clean_runtime(void) {
    assert(!app.game.errors && !app.draw_overflows && !app.game.missing_labels);
    assert(find_symbol(34) == PICO_NONE);
    assert(find_symbol(47) == PICO_NONE);
    assert(find_symbol(48) == PICO_NONE);
}

static void ticks(unsigned count) {
    while (count--) {
        host_tick(&app);
        clean_runtime();
    }
}

static void probe_draw(void *user, uint16_t symbol, const PicoMatrix *world, const PicoColor *color,
                       uint16_t ratio, uint8_t kind) {
    if (kind == PICO_DRAW_LEAF && symbol >= 11 && symbol <= 13) {
        logo_draws++;
        if (filter_logo)
            return;
    }
    if (kind == PICO_DRAW_LEAF && symbol == 1023) {
        title_draws++;
        if (filter_title)
            return;
    }
    host_draw(user, symbol, world, color, ratio, kind);
}

/* Compare real raster output with just the named artwork omitted. Traversal
 * alone wouldn't catch a logo accidentally clipped or hidden at tiny sizes. */
static void visible_artwork(int logo) {
    uint32_t complete;
    host_draw = app.game.host.draw;
    app.game.host.draw = probe_draw;
    logo_draws = title_draws = filter_logo = filter_title = 0;
    assert(host_render(&app));
    complete = host_frame_hash(&app);
    assert(logo ? logo_draws >= 3 : title_draws > 0);
    filter_logo = logo != 0;
    filter_title = logo == 0;
    assert(host_render(&app));
    assert(host_frame_hash(&app) != complete);
    app.game.host.draw = host_draw;
    filter_logo = filter_title = 0;
    assert(host_render(&app));
    assert(host_frame_hash(&app) == complete);
    clean_runtime();
}

/* Search physical pixel centres so the same tap can be delivered on the
 * selected display. Large outputs only need a coarse grid to find the button. */
static void click_symbol(unsigned symbol) {
    unsigned x, y, stride = app.view_width / 128;
    if (!stride)
        stride = 1;
    for (y = 0; y < app.view_height; y += stride) {
        for (x = 0; x < app.view_width; x += stride) {
            int32_t stage_x = (int32_t)((int64_t)(2 * x + 1) * 550 * 32768 / app.view_width);
            int32_t stage_y = (int32_t)((int64_t)(2 * y + 1) * 350 * 32768 / app.view_height);
            pico_pointer(&app.game, stage_x, stage_y, 0);
            if (app.game.hover != PICO_NONE &&
                app.game.instances[app.game.hover].symbol == symbol) {
                pico_pointer(&app.game, stage_x, stage_y, 1);
                pico_pointer(&app.game, stage_x, stage_y, 0);
                printf("button %u: stage (%ld.%02ld,%ld.%02ld)\n", symbol, (long)(stage_x / 65536),
                       (long)((stage_x % 65536) * 100 / 65536), (long)(stage_y / 65536),
                       (long)((stage_y % 65536) * 100 / 65536));
                clean_runtime();
                return;
            }
        }
    }
    fprintf(stderr, "No visible pointer target for button %u at root frame %u.\n", symbol,
            pico_frame(&app.game, "/"));
    assert(0);
}

static uint16_t intro_at(unsigned target) {
    unsigned remaining = 180;
    uint16_t intro;
    for (;;) {
        assert(pico_frame(&app.game, "/") == 23);
        intro = find_symbol(1024);
        assert(intro != PICO_NONE && app.game.instances[intro].frame <= target);
        if (app.game.instances[intro].frame == target)
            return intro;
        assert(remaining--);
        ticks(1);
    }
}

static void original_audio_order(void) {
    static const unsigned expected[] = {STOP_ALL, 992, STOP_ALL, 316, 52, STOP_ALL, 242};
    unsigned actual = 0, wanted = 0;
    while (actual < sound.count && wanted < sizeof(expected) / sizeof(expected[0])) {
        if (sound.sequence[actual] == expected[wanted])
            wanted++;
        actual++;
    }
    assert(wanted == sizeof(expected) / sizeof(expected[0]));
    assert(sound.theme_starts == 1 && app.stopped_sounds >= 3);
    assert(!app.silent_sound_events);
}

int main(int argc, char **argv) {
    static const unsigned dialogue_frames[] = {3, 4, 5, 6, 7, 31, 32, 33, 34, 35, 36, 37, 38};
    static const char *inventory[] = {"/icon_gun",  "/icon_fire", "/icon_goggles",
                                      "/icon_key1", "/icon_key2", "/icon_herb"};
    HostSoundSink sink = {&sound, sound_event, stop_sounds};
    unsigned width, height, i, remaining, continues = 0;
    uint16_t preloader, intro;
    assert(argc == 4);
    width = (unsigned)strtoul(argv[2], NULL, 10);
    height = (unsigned)strtoul(argv[3], NULL, 10);
    /* Exercise the shared default through the complete original opening. */
    assert(host_open_with_sound(&app, argv[1], width, height, 0, 0, &sink));
    assert(pico_frame(&app.game, "/") == 1 && !app.game.instances[0].playing);
    preloader = find_symbol(22);
    assert(preloader != PICO_NONE && app.game.instances[preloader].frame == 3);
    assert(!app.game.instances[preloader].playing && find_symbol(21) != PICO_NONE);
    assert(pico_find(&app.game, 0, "/health") == PICO_NONE);
    ticks(72);
    assert(pico_frame(&app.game, "/") == 1);
    visible_artwork(1);

    click_symbol(21);
    assert(pico_frame(&app.game, "/") == 23);
    assert(find_symbol(22) == PICO_NONE && find_symbol(1024) != PICO_NONE);
    for (i = 0; i < sizeof(dialogue_frames) / sizeof(dialogue_frames[0]); i++) {
        intro = intro_at(dialogue_frames[i]);
        assert(!app.game.instances[intro].playing);
        ticks(3);
        assert(app.game.instances[intro].frame == dialogue_frames[i]);
        assert(host_render(&app));
        click_symbol(120);
        continues++;
        assert(app.game.instances[intro].playing);
    }
    assert(continues == 13);
    intro_at(160);
    assert(strcmp(pico_text_for_symbol(1023), "PICO'S SCHOOL") == 0);
    visible_artwork(0);
    remaining = 24;
    while (pico_frame(&app.game, "/") == 23) {
        assert(remaining--);
        ticks(1);
    }
    assert(pico_frame(&app.game, "/") == 2 && find_symbol(1024) == PICO_NONE);
    assert(pico_frame(&app.game, "/health") == 1);
    for (i = 0; i < sizeof(inventory) / sizeof(inventory[0]); i++)
        assert(pico_frame(&app.game, inventory[i]) == 1);
    assert(pico_frame(&app.game, "/pico") == 5);
    original_audio_order();
    assert(host_render(&app));
    clean_runtime();
    printf("Startup at %ux%u: original logo, Play, 13 dialogue advances, title, fresh classroom; "
           "%u sound cues and stops.\n",
           width, height, sound.count);
    host_close(&app);
    return 0;
}
