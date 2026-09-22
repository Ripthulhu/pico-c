#include "touch_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FX(x) ((x) * 65536)

static Pico game;
static PicoSymbol symbols[4];
static PicoFrame frames[10];
static PicoPlacement placements[3];
static const uint16_t refs[] = {0, 1, 2};
static const PicoColor colors[] = {PICO_COLOR_IDENTITY};
static PicoAction actions[5];
static const char *const strings[] = {"", "press-a", "press-b", "release-a", "release-b", "/"};
static const PicoData data = {symbols, frames, refs, placements, colors, actions, NULL, strings,
                              10,      3,      3,    5,          0,      4,       6,    1};
static unsigned events[5];

static int hit(void *user, uint16_t symbol, const PicoMatrix *m, int32_t x, int32_t y) {
    (void)user;
    assert(symbol == 3);
    return x >= m->tx && x < m->tx + FX(20) && y >= m->ty && y < m->ty + FX(20);
}

static void medal(void *user, const char *name) {
    unsigned i;
    (void)user;
    for (i = 1; i <= 4; i++)
        if (!strcmp(name, strings[i])) {
            events[i]++;
            return;
        }
    assert(0);
}

static void setup(void) {
    unsigned i;
    PicoHost host = {0};
    PicoMatrix identity = PICO_IDENTITY;
    memset(symbols, 0, sizeof(symbols));
    memset(frames, 0, sizeof(frames));
    memset(placements, 0, sizeof(placements));
    memset(actions, 0, sizeof(actions));
    memset(events, 0, sizeof(events));
    symbols[0] = (PicoSymbol){0, 0, 0, 2, 0, 0, PICO_MOVIE, 0};
    symbols[1] = (PicoSymbol){2, 1, 3, 4, 1, 1, PICO_BUTTON, 0};
    symbols[2] = (PicoSymbol){6, 2, 4, 4, 1, 1, PICO_BUTTON, 0};
    symbols[3].kind = PICO_LEAF;
    actions[0].op = PICO_STOP;
    for (i = 1; i <= 4; i++)
        actions[i] = (PicoAction){0, (uint16_t)i, 0, PICO_MEDAL, 0};
    for (i = 0; i < 3; i++) {
        placements[i].matrix = identity;
        placements[i].life = i + 1;
        placements[i].symbol = (uint16_t)(i + 1);
        placements[i].depth = (uint16_t)(i + 1);
    }
    placements[0].matrix.tx = FX(100);
    placements[1].matrix.tx = FX(124);
    placements[0].matrix.ty = placements[1].matrix.ty = FX(100);
    frames[0].placement_count = 2;
    frames[0].action_count = frames[1].action_count = 1;
    for (i = 2; i < 10; i++) {
        frames[i].first_placement = 2;
        frames[i].placement_count = 1;
    }
    host.hit = hit;
    host.medal = medal;
    assert(pico_init(&game, &data, &host, 1));
}

static unsigned pointer(int x, int y, int down, int radius) {
    unsigned probes = host_touch_pointer(&game, FX(x), FX(y), down, FX(radius));
    assert(probes <= 257);
    assert(!game.errors);
    return probes;
}

static void test_tolerance(void) {
    unsigned i;
    static const int misses[][2] = {{90, 110}, {110, 90}, {110, 130}, {93, 93}, {93, 127}};
    for (i = 0; i < sizeof(misses) / sizeof(misses[0]); i++) {
        setup();
        assert(pico_hit_test(&game, FX(misses[i][0]), FX(misses[i][1])) == PICO_NONE);
        assert(pointer(misses[i][0], misses[i][1], 1, 14) > 1);
        pointer(misses[i][0], misses[i][1], 0, 14);
        assert(events[1] == 1 && events[3] == 1);
        assert(!events[2] && !events[4]);
    }
    setup();
    assert(pointer(90, 110, 1, 0) == 0);
    pointer(90, 110, 0, 0);
    assert(!events[1] && !events[3]);
    pointer(85, 110, 1, 14);
    pointer(85, 110, 0, 14);
    assert(!events[1] && !events[3]);
    assert(pointer(125, 110, 1, 14) == 1); /* Original B hit outranks padded A. */
    pointer(125, 110, 0, 14);
    assert(!events[1] && !events[3] && events[2] == 1 && events[4] == 1);
}

static void test_gestures(void) {
    setup();
    pointer(90, 110, 1, 14);
    pointer(110, 130, 1, 14);
    pointer(110, 90, 0, 14);
    assert(events[1] == 1 && events[3] == 1); /* Jitter dispatches once. */

    setup();
    pointer(110, 110, 1, 14);
    pointer(122, 110, 1, 14); /* Gap: retain A, even when B is nearby. */
    pointer(122, 110, 0, 14);
    assert(events[1] == 1 && events[3] == 1 && !events[2] && !events[4]);

    setup();
    pointer(110, 110, 1, 14);
    pointer(151, 110, 1, 14); /* Only B is within tolerance here. */
    pointer(151, 110, 0, 14);
    assert(events[1] == 1 && !events[3] && !events[2] && !events[4]);

    setup();
    pointer(110, 110, 1, 14);
    pointer(125, 110, 0, 14); /* Releasing over B must not click either button. */
    assert(events[1] == 1 && !events[3] && !events[2] && !events[4]);

    setup();
    pointer(50, 50, 1, 14);
    pointer(90, 110, 1, 14);
    pointer(110, 110, 0, 14); /* Missed down cannot acquire a button during drag. */
    assert(!events[1] && !events[3]);

    setup();
    pointer(110, 110, 1, 14);
    pointer(-1, 110, 1, 14);
    assert(!game.pointer_down && game.pressed == PICO_NONE);
    pointer(110, 110, 0, 14);
    assert(events[1] == 1 && !events[3]);
}

static void test_replaced_capture(void) {
    uint16_t old;
    uint32_t generation;
    setup();
    pointer(110, 110, 1, 14);
    old = game.pressed;
    generation = game.pressed_generation;
    pico_goto(&game, "/", 2, 0);
    pico_goto(&game, "/", 1, 0);
    assert(game.instances[old].generation != generation);
    pointer(90, 110, 0, 14);
    assert(events[1] == 1 && !events[3] && !events[4]);

    setup();
    actions[1] = (PicoAction){5, 2, 0, PICO_GOTO_STOP, 0};
    pointer(90, 110, 1, 14);
    assert(pico_frame(&game, "/") == 2);
    pico_goto(&game, "/", 1, 0);
    pointer(110, 110, 0, 14);
    assert(!events[3] && !events[4]);
}

static void test_bounds(void) {
    setup();
    assert(!host_touch_pointer(NULL, 0, 0, 1, INT32_MAX));
    assert(!host_touch_pointer(&game, INT32_MAX, INT32_MIN, 1, INT32_MAX));
    assert(!game.pointer_down);
    assert(host_touch_pointer(&game, FX(50), FX(110), 1, INT32_MAX) == 257);
    assert(game.pressed == PICO_NONE); /* Radius is capped at 32 stage pixels. */
    pointer(50, 110, 0, 14);
    assert(!host_touch_pointer(&game, FX(90), FX(110), 1, INT32_MIN));
    assert(!events[1]);
}

static void fight_setup(void) {
    static PicoSymbol fight_symbols[740];
    static PicoFrame fight_frames[15];
    static PicoData fight_data;
    PicoHost host;
    setup();
    host = game.host;
    memset(fight_symbols, 0, sizeof(fight_symbols));
    memset(fight_frames, 0, sizeof(fight_frames));
    memcpy(fight_symbols, symbols, sizeof(symbols));
    memcpy(fight_frames, frames, sizeof(frames));
    fight_symbols[0].frame_count = 15;
    fight_symbols[739] = symbols[1];
    fight_frames[14] = frames[0];
    fight_data = data;
    fight_data.symbols = fight_symbols;
    fight_data.symbol_count = 740;
    fight_data.frames = fight_frames;
    fight_data.frame_count = 15;
    placements[0].symbol = 739;
    assert(pico_init(&game, &fight_data, &host, 15));
}

static unsigned fight_pointer(int x, int y, int down, int radius) {
    unsigned probes = host_touch_game_pointer(&game, FX(x), FX(y), down, FX(radius));
    assert(probes <= 513);
    assert(!game.errors);
    return probes;
}

static void test_fight_allowance(void) {
    fight_setup();
    assert(fight_pointer(50, 110, 1, 14) > 257);
    assert(events[1] == 1 && !events[2]);
    fight_pointer(50, 110, 1, 14);
    fight_pointer(50, 110, 0, 14);
    assert(events[1] == 1 && !events[3]); /* No wider held or release target. */

    fight_setup();
    fight_pointer(300, 110, 1, 14);
    fight_pointer(50, 110, 1, 14);
    fight_pointer(110, 110, 0, 14);
    assert(!events[1] && !events[3]); /* A missed press cannot acquire during drag. */

    fight_setup();
    assert(fight_pointer(50, 110, 0, 14) == 1);
    assert(fight_pointer(50, 110, 1, 0) == 0);
    fight_pointer(50, 110, 0, 0);
    assert(!events[1]); /* Precise pointers and bare releases stay precise. */

    fight_setup();
    pico_goto(&game, "/", 1, 0);
    assert(fight_pointer(50, 110, 1, 14) <= 257);
    assert(!events[1]);

    fight_setup();
    assert(pointer(50, 110, 1, 14) <= 257);
    assert(!events[1]); /* The generic helper retains its original range. */

    fight_setup();
    assert(fight_pointer(190, 110, 1, 14) == 513);
    assert(!events[1] && !events[2]); /* The other button gets no wider allowance. */

    fight_setup();
    assert(fight_pointer(125, 110, 1, 14) == 1);
    fight_pointer(125, 110, 0, 14);
    assert(!events[1] && events[2] == 1 && events[4] == 1);

    fight_setup();
    assert(fight_pointer(150, 110, 1, 14) <= 257);
    fight_pointer(150, 110, 0, 14);
    assert(!events[1] && events[2] == 1 && events[4] == 1);

    fight_setup();
    placements[1].matrix.tx = FX(100);
    fight_pointer(50, 110, 1, 14);
    assert(!events[1] && !events[2]); /* A different button covering him blocks fallback. */

    fight_setup();
    assert(host_touch_game_pointer(&game, FX(35), FX(110), 1, INT32_MAX) <= 513);
    assert(!events[1] && !events[2]); /* Even an excessive radius stops at 64 pixels. */
    assert(!host_touch_game_pointer(NULL, 0, 0, 1, INT32_MAX));

    fight_setup();
    fight_pointer(50, 110, 1, 14);
    pico_goto(&game, "/", 2, 0);
    pico_goto(&game, "/", 15, 0);
    fight_pointer(50, 110, 0, 14);
    assert(events[1] == 1 && !events[3]);

    fight_setup();
    fight_pointer(50, 110, 1, 14);
    assert(!fight_pointer(-1, 110, 1, 14));
    assert(!game.pointer_down && game.pressed == PICO_NONE);
    assert(events[1] == 1 && !events[3]);
}

int main(void) {
    test_tolerance();
    test_gestures();
    test_replaced_capture();
    test_bounds();
    test_fight_allowance();
    puts("touch tolerance: exact hits, near misses, capture, cancellation, bounds and fight "
         "allowance passed");
    return 0;
}
