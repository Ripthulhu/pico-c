#include "host_common.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static HostApp app;
static uint32_t first[64 * 8], second[64 * 8];

static void caption_pixels(uint32_t *out) {
    unsigned i;
    for (i = 0; i < 64 * 8; i++)
        out[i] = host_pixel_rgb(&app, 64 * 24 + i);
}

static void ticks(unsigned count) {
    while (count--)
        host_tick(&app);
    assert(!app.game.errors);
}

static void check_reader_text(void) {
    static const char page[] = "KEEP THIS READER PAGE OPEN.";
    const char *scene_text;
    assert(host_render(&app));
    scene_text = app.visible_text;
    assert(scene_text && scene_text[0]);
    app.reading = 1;
    app.visible_text = page;
    assert(host_render(&app));
    assert(app.visible_text == page);
    app.reading = 0;
    app.visible_text = NULL;
    /* A traversal outside a streamed picture must still discover its text,
     * even though the host's previous scanline was the bottom of the view. */
    pico_render(&app.game);
    assert(app.visible_text == scene_text);
    assert(!app.game.errors);
}

int main(int argc, char **argv) {
    unsigned count;
    char label[64];
    assert(argc == 2);
    assert(host_open(&app, argv[1], 64, 32, 2, 0));
    ticks(24);
    pico_goto(&app.game, "/icon_key1", 14, 0);
    host_select(&app, 0);
    for (count = 0; count < PICO_MAX_INSTANCES; count++) {
        if (app.selected != PICO_NONE && app.game.instances[app.selected].symbol == 232)
            break;
        host_select(&app, 1);
    }
    assert(count < PICO_MAX_INSTANCES);
    host_caption(&app, app.selected, label, sizeof(label));
    assert(!strcmp(label, "USE SCHOOL KEY"));
    assert(host_render(&app));
    caption_pixels(first);
    ticks(47);
    assert(host_render(&app));
    caption_pixels(second);
    assert(!memcmp(first, second, sizeof(first)));
    ticks(1);
    assert(host_render(&app));
    caption_pixels(second);
    assert(memcmp(first, second, sizeof(first)));
    ticks(48);
    assert(host_render(&app));
    caption_pixels(second);
    assert(!memcmp(first, second, sizeof(first)));
    check_reader_text();
    printf("64-pixel caption holds for 48 ticks, shows the suffix, then repeats.\n");
    host_close(&app);
    return 0;
}
