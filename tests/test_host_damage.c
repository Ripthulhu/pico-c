#include "host_common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HostApp retained, complete;
static unsigned partial_frames, unchanged_frames, changed_counts;
static unsigned map_blinks;

static void same_picture(void) {
    unsigned old_count = retained.retained_draw_count;
    int was_valid = retained.retained_valid;
    size_t bytes = (size_t)retained.width * retained.height * host_pixel_bytes(&retained);
    complete.retained_valid = 0;
    assert(host_render(&retained));
    assert(host_render(&complete));
    assert(!retained.game.errors && !complete.game.errors);
    assert(!retained.draw_overflows && !complete.draw_overflows);
    assert(!memcmp(retained.pixels, complete.pixels, bytes));
    assert(retained.selected == complete.selected);
    assert(retained.selected_generation == complete.selected_generation);
    if (was_valid && retained.retained_valid) {
        if (!retained.damage.width)
            unchanged_frames++;
        else if ((size_t)retained.damage.width * retained.damage.height <
                 (size_t)retained.view_width * retained.view_height)
            partial_frames++;
        if (old_count != retained.draw_count)
            changed_counts++;
    }
}

static void go(unsigned frame) {
    pico_goto(&retained.game, "/", (uint16_t)frame, 0);
    pico_goto(&complete.game, "/", (uint16_t)frame, 0);
    same_picture();
}

static unsigned map_marker_count(void) {
    unsigned i, count = 0;
    for (i = 0; i < retained.draw_count; i++)
        count += retained.draws[i].symbol == 278;
    return count;
}

static void animate(unsigned frame) {
    unsigned tick, marker_count;
    go(frame);
    marker_count = map_marker_count();
    for (tick = 0; tick < 48; tick++) {
        unsigned next_count;
        host_tick(&retained);
        host_tick(&complete);
        same_picture();
        next_count = map_marker_count();
        if (next_count != marker_count) {
            /* The marker appears and disappears before unchanged map, health
             * and corner-logo leaves. Their shifted indices aren't damage. */
            assert(retained.retained_valid);
            assert(retained.damage.width < retained.view_width);
            map_blinks++;
        }
        marker_count = next_count;
    }
    same_picture();
    assert(!retained.damage.width);
}

static void resize(unsigned width, unsigned height) {
    assert(host_resize(&retained, width, height));
    assert(host_resize(&complete, width, height));
    assert(!retained.retained_valid);
    same_picture();
    same_picture();
}

static void overlays(void) {
    retained.reading = complete.reading = 1;
    retained.visible_text = complete.visible_text = "KEEP THE PICTURE CORRECT AFTER READING.";
    same_picture();
    assert(!retained.retained_valid);
    retained.reading = complete.reading = 0;
    same_picture();
    assert(retained.retained_valid);
    retained.assist = complete.assist = 1;
    same_picture();
    assert(!retained.retained_valid);
    retained.assist = complete.assist = 0;
    same_picture();
    assert(retained.retained_valid);
    retained.mono = complete.mono = 1;
    same_picture();
    assert(!retained.retained_valid);
    retained.mono = complete.mono = 0;
    same_picture();
    assert(retained.retained_valid);
}

static void replacement_buffer(void) {
    size_t bytes = (size_t)retained.width * retained.height * host_pixel_bytes(&retained);
    void *owned = retained.pixels;
    retained.pixels = malloc(bytes);
    assert(retained.pixels);
    memset(retained.pixels, 0xa5, bytes);
    same_picture();
    free(owned);
}

static void failed_frame(void) {
    const uint8_t *data = retained.assets.data;
    retained.retained_valid = 0;
    retained.assets.data = NULL;
    assert(!host_render(&retained));
    assert(!retained.retained_valid);
    retained.assets.data = data;
    same_picture();
}

int main(int argc, char **argv) {
    unsigned width, height;
    assert(argc == 4);
    width = (unsigned)strtoul(argv[2], NULL, 10);
    height = (unsigned)strtoul(argv[3], NULL, 10);
    assert(host_open(&retained, argv[1], width, height, 2, 0));
    assert(host_open(&complete, argv[1], width, height, 2, 0));
    retained.assist_auto = complete.assist_auto = 0;
    retained.assist = complete.assist = 0;
    same_picture();
    same_picture();
    assert(!retained.damage.width);
    animate(4);
    animate(11);
    go(1);
    go(2);
    go(11);
    overlays();
    resize(width / 2 + 1, height + 17);
    resize(width, height);
    replacement_buffer();
    failed_frame();
    assert(partial_frames && unchanged_frames && changed_counts && map_blinks);
    printf("Retained host: %u partial, %u unchanged, %u count changes match full rendering; "
           "%u map blinks keep unchanged artwork.\n",
           partial_frames, unchanged_frames, changed_counts, map_blinks);
    host_close(&retained);
    host_close(&complete);
    return 0;
}
