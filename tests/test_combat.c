#include "host_common.h"
#include "touch_input.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

enum { Q = 65536, TARGETS = 5 };
static const unsigned symbols[TARGETS] = {739, 885, 904, 911, 857};
typedef struct Point {
    int32_t x, y;
    int found;
} Point;
static HostApp app;
static Point points[TARGETS];
static unsigned shots[TARGETS], stomp_phases;

static void clean_runtime(void) {
    assert(!app.game.errors && !app.game.missing_labels && !app.draw_overflows);
}

static void tick(void) {
    host_tick(&app);
    clean_runtime();
}

static void find_targets(void) {
    unsigned i, x, y;
    for (i = 0; i < TARGETS; i++)
        points[i].found = 0;
    for (y = 2; y < 350; y += 6) {
        for (x = 2; x < 550; x += 6) {
            uint16_t n = pico_hit_test(&app.game, (int32_t)x * Q, (int32_t)y * Q);
            if (n == PICO_NONE)
                continue;
            for (i = 0; i < TARGETS; i++) {
                uint16_t parent;
                if (points[i].found || app.game.instances[n].symbol != symbols[i])
                    continue;
                parent = app.game.instances[n].parent;
                /* Both eyes use symbol 911. An eye already hit remains a
                 * button, so choose a still-active attack on its own timeline. */
                if (symbols[i] == 911 && (app.game.instances[parent].frame < 30 ||
                                          app.game.instances[parent].frame >= 55))
                    continue;
                points[i].x = (int32_t)x * Q;
                points[i].y = (int32_t)y * Q;
                points[i].found = 1;
            }
        }
    }
}

static void tap(unsigned target) {
    Point p = points[target];
    uint16_t before = pico_frame(&app.game, "/room/demon_health");
    assert(p.found && !app.game.pointer_down);
    /* A real touch has no preceding hover. One authored tick elapses while
     * held, allowing the target to move before release at the same location. */
    assert(host_touch_game_pointer(&app.game, p.x, p.y, 1, 14 * Q) <= 257);
    clean_runtime();
    if (symbols[target] == 885) {
        uint16_t after = pico_frame(&app.game, "/room/demon_health");
        assert(after == before + 1);
        if (after == 50 || after == 104 || after == 160) {
            assert(pico_frame(&app.game, "/room") == 230);
            stomp_phases++;
        }
        if (after == 208)
            assert(pico_frame(&app.game, "/room") == 265);
    }
    shots[target]++;
    tick();
    assert(host_touch_game_pointer(&app.game, p.x, p.y, 0, 14 * Q) <= 257);
    clean_runtime();
    assert(!app.game.pointer_down);
}

static void start(const char *assets, unsigned width, unsigned height, unsigned root,
                  unsigned room) {
    unsigned i;
    for (i = 0; i < TARGETS; i++)
        shots[i] = 0;
    stomp_phases = 0;
    assert(host_open(&app, assets, width, height, 2, 0));
    /* Enter the authored combat start directly. Only the route to the fight
     * is skipped; damage, phases, attacks and rewards use normal game actions. */
    pico_goto(&app.game, "/", (uint16_t)root, 0);
    pico_goto(&app.game, "/room", (uint16_t)room, 1);
    assert(pico_frame(&app.game, "/health") == 1);
    assert(pico_frame(&app.game, "/action_semi") == 2);
    clean_runtime();
}

static void hanzou(const char *assets, unsigned width, unsigned height) {
    unsigned step, elapsed;
    start(assets, width, height, 15, 63);
    for (step = 0; step < 800 && pico_frame(&app.game, "/room") < 250; step++) {
        assert(pico_frame(&app.game, "/") == 15);
        if (step % 6 == 0) {
            find_targets();
            if (points[0].found)
                tap(0);
        }
        tick();
    }
    assert(shots[0] == 50);
    assert(pico_frame(&app.game, "/health") == 4);
    for (elapsed = 0; elapsed < 80 && pico_frame(&app.game, "/room") != 294; elapsed++)
        tick();
    assert(pico_frame(&app.game, "/") == 15);
    assert(pico_frame(&app.game, "/room") == 294);
    assert(pico_frame(&app.game, "/continue") == 5);
    assert(pico_frame(&app.game, "/action_semi") == 1);
    assert(host_render(&app));
    printf("Hanzou: 50 touch hits, attack damage, defeat and checkpoint; %lu ticks, health %u\n",
           (unsigned long)app.game.tick, pico_frame(&app.game, "/health"));
    clean_runtime();
    host_close(&app);
}

static void casandra(const char *assets, unsigned width, unsigned height) {
    unsigned step, elapsed, health_at_victory;
    start(assets, width, height, 18, 189);
    for (step = 0; step < 2400 && pico_frame(&app.game, "/room") < 265; step++) {
        assert(pico_frame(&app.game, "/") == 18);
        if (step % 6 == 0) {
            find_targets();
            if (points[4].found)
                tap(4);
            else if (points[2].found)
                tap(2);
            else if (points[3].found)
                tap(3);
            else if (points[1].found)
                tap(1);
        }
        tick();
    }
    assert(pico_frame(&app.game, "/room/demon_health") == 208);
    assert(shots[1] == 207 && shots[2] && shots[3] && shots[4] == 3);
    assert(stomp_phases == 3);
    assert(pico_frame(&app.game, "/health") == 1);
    health_at_victory = pico_frame(&app.game, "/health");
    for (elapsed = 0; elapsed < 100 && pico_frame(&app.game, "/") == 18; elapsed++)
        tick();
    assert(pico_frame(&app.game, "/") == 19);
    assert(pico_frame(&app.game, "/action_semi") == 1);
    assert(host_render(&app));
    printf("Casandra: 207 touch hits, %u barrels, %u eyes, three ceiling blocks and ending; "
           "%lu ticks, health at victory %u\n",
           shots[2], shots[3], (unsigned long)app.game.tick, health_at_victory);
    clean_runtime();
    host_close(&app);
}

int main(int argc, char **argv) {
    unsigned width, height;
    assert(argc == 4);
    width = (unsigned)strtoul(argv[2], 0, 10);
    height = (unsigned)strtoul(argv[3], 0, 10);
    hanzou(argv[1], width, height);
    casandra(argv[1], width, height);
    puts("Later boss encounters complete with touch-only input.");
    return 0;
}
