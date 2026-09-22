#include "host_common.h"
#include <assert.h>
#include <stdio.h>

static HostApp app;

/* Find an actual visible hit pixel on a coarse stage grid, then deliver the
 * same press/release a physical pointer would. No timeline controls are used. */
static void click_symbol(unsigned symbol) {
    unsigned x, y, t;
    for (t = 0; t < 24; t++)
        host_tick(&app);
    for (y = 2; y < 350; y += 4) {
        for (x = 2; x < 550; x += 4) {
            pico_pointer(&app.game, (int32_t)x * 65536, (int32_t)y * 65536, 0);
            if (app.game.hover != PICO_NONE &&
                app.game.instances[app.game.hover].symbol == symbol) {
                pico_pointer(&app.game, (int32_t)x * 65536, (int32_t)y * 65536, 1);
                pico_pointer(&app.game, (int32_t)x * 65536, (int32_t)y * 65536, 0);
                printf("clicked symbol %u at (%u,%u), root=%u\n", symbol, x, y,
                       app.game.instances[0].frame);
                assert(!app.game.errors);
                return;
            }
        }
    }
    fprintf(stderr, "No clickable stage pixel found for symbol %u in root frame %u.\n", symbol,
            app.game.instances[0].frame);
    assert(0);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(host_open(&app, argv[1], 96, 64, 2, 0));
    click_symbol(84);
    assert(app.game.instances[0].frame == 4);
    click_symbol(419);
    assert(app.game.instances[0].frame == 5);
    click_symbol(432);
    assert(app.game.instances[0].frame == 6);
    click_symbol(451);
    assert(app.game.instances[0].frame == 7);
    click_symbol(207);
    assert(pico_frame(&app.game, "/icon_gun") > 1);
    printf("Pointer route passed: classroom, hall, east hall, closet, gun pickup.\n");
    host_close(&app);
    return 0;
}
