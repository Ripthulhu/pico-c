#include "host_common.h"
#include "touch_input.h"
#include <assert.h>
#include <stdio.h>

enum { Q = 65536, RADIUS = 14 * Q, REPEATS = 32 };
typedef struct Point {
    int32_t x, y;
} Point;
typedef struct Target {
    uint16_t instance;
    Point exact, near;
} Target;

static HostApp app;
static unsigned leaf_probes;
static int (*original_hit)(void *, uint16_t, const PicoMatrix *, int32_t, int32_t);

static int counting_hit(void *user, uint16_t symbol, const PicoMatrix *matrix, int32_t x,
                        int32_t y) {
    leaf_probes++;
    return original_hit(user, symbol, matrix, x, y);
}

static uint16_t symbol_instance(unsigned symbol) {
    unsigned i;
    for (i = 0; i < PICO_MAX_INSTANCES; i++)
        if (app.game.instances[i].alive && app.game.instances[i].symbol == symbol)
            return (uint16_t)i;
    return PICO_NONE;
}

static unsigned touch(Point point, int down) {
    unsigned probes = host_touch_game_pointer(&app.game, point.x, point.y, down, RADIUS);
    assert(probes <= 257);
    assert(!app.game.errors);
    return probes;
}

/* Find an authored hit and a nearby empty pixel independently of the helper's
 * ring directions. These buttons act on release, so probes cannot play them. */
static Target target(unsigned symbol) {
    static const int offsets[][2] = {{-8, 0},  {8, 0},  {0, -8}, {0, 8},
                                     {-6, -6}, {6, -6}, {-6, 6}, {6, 6}};
    unsigned x, y, i;
    for (y = 2; y < 350; y += 4) {
        for (x = 2; x < 550; x += 4) {
            Point exact = {(int32_t)x * Q, (int32_t)y * Q};
            uint16_t instance = pico_hit_test(&app.game, exact.x, exact.y);
            if (instance == PICO_NONE || app.game.instances[instance].symbol != symbol)
                continue;
            assert(!app.game.data->symbols[symbol].press_count);
            for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
                Point near = {exact.x + offsets[i][0] * Q, exact.y + offsets[i][1] * Q};
                if (near.x < 0 || near.x >= 550 * Q || near.y < 0 || near.y >= 350 * Q)
                    continue;
                if (pico_hit_test(&app.game, near.x, near.y) == PICO_NONE) {
                    Target result = {instance, exact, near};
                    printf("button %u: exact (%ld,%ld), outside authored hit (%ld,%ld)\n", symbol,
                           (long)(exact.x / Q), (long)(exact.y / Q), (long)(near.x / Q),
                           (long)(near.y / Q));
                    return result;
                }
            }
        }
    }
    fprintf(stderr, "No bounded near-miss fixture for symbol %u at root %u\n", symbol,
            pico_frame(&app.game, "/"));
    assert(0);
    return (Target){PICO_NONE, {0, 0}, {0, 0}};
}

static void exact_then_cancel(Target button) {
    uint32_t actions = app.game.actions_executed;
    assert(pico_hit_test(&app.game, button.exact.x, button.exact.y) == button.instance);
    assert(touch(button.exact, 1) == 1);
    assert(app.game.pressed == button.instance);
    touch((Point){-Q, -Q}, 0);
    assert(app.game.pressed == PICO_NONE && !app.game.pointer_down);
    assert(app.game.actions_executed == actions);
}

static void near_click(Target button) {
    assert(pico_hit_test(&app.game, button.near.x, button.near.y) == PICO_NONE);
    assert(touch(button.near, 1) > 1);
    assert(app.game.pressed == button.instance);
    assert(touch(button.near, 0) > 1);
    assert(!app.game.pointer_down);
}

static void benchmark_near(Target button, const char *label) {
    unsigned i, probes = 0, worst_probes = 0;
    double start, worst = 0, total;
    uint32_t actions = app.game.actions_executed;
    leaf_probes = 0;
    start = host_now();
    for (i = 0; i < REPEATS; i++) {
        double begin = host_now();
        unsigned count = touch(button.near, 1);
        double elapsed = host_now() - begin;
        assert(app.game.pressed == button.instance);
        probes += count;
        if (count > worst_probes)
            worst_probes = count;
        if (elapsed > worst)
            worst = elapsed;
        touch((Point){-Q, -Q}, 0);
    }
    total = host_now() - start;
    assert(app.game.actions_executed == actions);
    printf("%s near tap: mean %.1f us, worst down %.1f us, mean %.1f probes, max %u probes, "
           "mean %.1f leaf tests\n",
           label, total * 1e6 / REPEATS, worst * 1e6, (double)probes / REPEATS, worst_probes,
           (double)leaf_probes / REPEATS);
}

static void benchmark_empty(const char *label) {
    Point empty = {0, 0};
    unsigned x, y, found = 0, i;
    double start, worst = 0, total;
    uint32_t actions = app.game.actions_executed;
    /* Disposable state finds a fully empty search circle without advancing
     * the actual game. Asset hits still use the real host's immutable pack. */
    for (y = 20; y < 330 && !found; y += 32) {
        for (x = 20; x < 530; x += 32) {
            Pico probe = app.game;
            unsigned count = host_touch_game_pointer(&probe, x * Q, y * Q, 1, RADIUS);
            if (count == 257 && probe.pressed == PICO_NONE) {
                empty = (Point){(int32_t)x * Q, (int32_t)y * Q};
                found = 1;
                break;
            }
        }
    }
    assert(found);
    leaf_probes = 0;
    start = host_now();
    for (i = 0; i < REPEATS; i++) {
        double begin = host_now();
        assert(touch(empty, 1) == 257 && app.game.pressed == PICO_NONE);
        double elapsed = host_now() - begin;
        if (elapsed > worst)
            worst = elapsed;
        touch(empty, 0);
        assert(app.game.pressed == PICO_NONE && !app.game.pointer_down);
    }
    total = host_now() - start;
    assert(app.game.actions_executed == actions);
    printf("%s empty tap (%ld,%ld): mean %.1f us, worst down %.1f us, 257 probes/down, "
           "mean %.1f leaf tests, %u live instances\n",
           label, (long)(empty.x / Q), (long)(empty.y / Q), total * 1e6 / REPEATS, worst * 1e6,
           (double)leaf_probes / REPEATS, app.game.live_instances);
}

int main(int argc, char **argv) {
    Target play, next, hall, west, east;
    uint16_t intro;
    unsigned remaining;
    assert(argc == 2);
    assert(host_open(&app, argv[1], 550, 350, 1, 0));
    original_hit = app.game.host.hit;
    app.game.host.hit = counting_hit;
    assert(host_render(&app));
    play = target(21);
    exact_then_cancel(play);
    benchmark_empty("menu");
    benchmark_near(play, "Play");
    near_click(play);
    assert(pico_frame(&app.game, "/") == 23);
    remaining = 8;
    while ((intro = symbol_instance(1024)) != PICO_NONE && app.game.instances[intro].frame < 3) {
        assert(remaining--);
        host_tick(&app);
    }
    assert(intro != PICO_NONE && app.game.instances[intro].frame == 3);
    next = target(120);
    exact_then_cancel(next);
    benchmark_near(next, "Continue");
    near_click(next);
    assert(app.game.instances[intro].playing);
    host_tick(&app);
    assert(app.game.instances[intro].frame == 4);

    /* The complete intro route has a separate regression. Enter a room here
     * to exercise its actual navigation geometry with the same touch helper. */
    pico_goto(&app.game, "/", 2, 0);
    for (remaining = 0; remaining < 24; remaining++)
        host_tick(&app);
    hall = target(84);
    exact_then_cancel(hall);
    benchmark_near(hall, "Hall");
    near_click(hall);
    assert(pico_frame(&app.game, "/") == 4);
    for (remaining = 0; remaining < 24; remaining++)
        host_tick(&app);
    west = target(418);
    east = target(419);
    assert(touch(west.exact, 1) == 1 && app.game.pressed == west.instance);
    touch(east.near, 1);
    assert(app.game.pressed == west.instance && app.game.hover != east.instance);
    touch(east.near, 0);
    assert(pico_frame(&app.game, "/") == 4);
    near_click(east);
    assert(pico_frame(&app.game, "/") == 5);

    pico_goto(&app.game, "/", 11, 0);
    for (remaining = 0; remaining < 24; remaining++)
        host_tick(&app);
    assert(host_render(&app));
    assert(app.draw_count >= 200);
    benchmark_empty("busy room 11");
    assert(!app.game.errors && !app.draw_overflows);
    host_close(&app);
    puts("Real artwork: near misses activate Play, Continue and navigation; capture doesn't "
         "transfer.");
    return 0;
}
