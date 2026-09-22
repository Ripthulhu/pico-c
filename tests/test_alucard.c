#include "host_common.h"
#include "touch_input.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

enum { Q = 65536 };
typedef struct Point {
    int32_t x, y;
} Point;

static HostApp app;
static int finger;

static uint16_t symbol_instance(unsigned symbol) {
    unsigned i;
    for (i = 0; i < PICO_MAX_INSTANCES; i++)
        if (app.game.instances[i].alive && app.game.instances[i].symbol == symbol)
            return (uint16_t)i;
    return PICO_NONE;
}

static void clean_runtime(void) {
    assert(!app.game.errors && !app.game.missing_labels && !app.draw_overflows);
    assert(pico_frame(&app.game, "/") == 3);
}

static void tick(void) {
    host_tick(&app);
    clean_runtime();
}

static int point_for(unsigned symbol, Point *point) {
    unsigned x, y;
    for (y = 2; y < 350; y += 4) {
        for (x = 2; x < 550; x += 4) {
            uint16_t n = pico_hit_test(&app.game, (int32_t)x * Q, (int32_t)y * Q);
            if (n != PICO_NONE && app.game.instances[n].symbol == symbol) {
                point->x = (int32_t)x * Q;
                point->y = (int32_t)y * Q;
                return 1;
            }
        }
    }
    return 0;
}

static void pointer(Point point, int down) {
    if (finger)
        assert(host_touch_game_pointer(&app.game, point.x, point.y, down, 14 * Q) <= 257);
    else
        pico_pointer(&app.game, point.x, point.y, down);
    clean_runtime();
}

static void shoot(unsigned symbol) {
    Point point;
    uint32_t actions;
    assert(!app.game.pointer_down);
    assert(point_for(symbol, &point));
    if (!finger) {
        pointer(point, 0);
        assert(app.game.hover != PICO_NONE);
        assert(app.game.instances[app.game.hover].symbol == symbol);
    }
    pointer(point, 1);
    assert(pico_frame(&app.game, "/pico") == 22);
    actions = app.game.actions_executed;
    pointer(point, 1);
    assert(app.game.actions_executed == actions);
    pointer(point, 0);
    printf("%s shot %u at (%ld,%ld), room %u\n", finger ? "finger" : "pointer", symbol,
           (long)(point.x / Q), (long)(point.y / Q), pico_frame(&app.game, "/room2"));
}

static void wait_room(unsigned frame, unsigned limit) {
    while (pico_frame(&app.game, "/room2") != frame && limit--)
        tick();
    if (pico_frame(&app.game, "/room2") != frame) {
        fprintf(stderr, "Expected Alucard room %u, stopped at %u after %lu ticks\n", frame,
                pico_frame(&app.game, "/room2"), (unsigned long)app.game.tick);
        assert(0);
    }
}

static void attack_to_stun(void) {
    uint16_t room;
    wait_room(101, 4);
    shoot(350);
    wait_room(130, 8);
    room = pico_find(&app.game, 0, "/room2");
    /* The projectile deletes its own clip with gotoAndStop, then resumes the
     * parent in the same action block. Losing that play leaves no next target. */
    assert(app.game.instances[room].playing);
    assert(pico_frame(&app.game, "/room2/stun") == 2);
    tick();
    assert(pico_frame(&app.game, "/room2") == 131);
    shoot(355);
    wait_room(175, 8);
    assert(app.game.instances[room].playing);
    assert(pico_frame(&app.game, "/room2/stun") == 3);
    tick();
    assert(pico_frame(&app.game, "/room2") == 176);
    shoot(357);
    wait_room(213, 10);
    assert(pico_frame(&app.game, "/room2/stun") == 4);
    assert(symbol_instance(341) != PICO_NONE);
}

static void release_damage(void) {
    Point point;
    uint16_t health = pico_frame(&app.game, "/room2/punk3_health");
    assert(!app.game.pointer_down);
    assert(point_for(341, &point));
    pointer(point, 1);
    assert(pico_frame(&app.game, "/room2/punk3_health") == health);
    pointer(point, 0);
    assert(pico_frame(&app.game, "/room2/punk3_health") == health + 1);
    assert(pico_frame(&app.game, "/pico") == 23);
}

static void run(const char *assets, unsigned width, unsigned height, int use_finger) {
    unsigned hit;
    finger = use_finger;
    assert(host_open(&app, assets, width, height, 2, 0));
    pico_goto(&app.game, "/", 3, 0);
    /* Diagnostic scene entry isolates combat. The normal gun dispatcher still
     * performs its authored stop-and-play sequence to start the encounter. */
    pico_goto(&app.game, "/action_room2gun", 3, 0);
    pico_play(&app.game, "/action_room2gun");
    tick();
    assert(pico_frame(&app.game, "/room2") == 100);
    assert(pico_frame(&app.game, "/action_semi") == 2);
    assert(pico_frame(&app.game, "/action_room2gun") == 3);
    attack_to_stun();
    assert(pico_frame(&app.game, "/health") == 1);
    assert(pico_frame(&app.game, "/room2/punk3_health") == 1);
    assert(host_render(&app));
    release_damage();
    assert(pico_frame(&app.game, "/room2/punk3_health") == 2);
    /* Let the vulnerable animation finish. Its final actions also navigate
     * away from their own clip before restoring the next attack. */
    wait_room(101, 30);
    assert(app.game.instances[pico_find(&app.game, 0, "/room2")].playing);
    assert(pico_frame(&app.game, "/room2/punk3/punk3_actions") == 1);
    assert(pico_frame(&app.game, "/room2/punk3_health") == 2);
    attack_to_stun();
    /* Release actions are immediate, including a burst before the next tick.
     * The eleventh successful hit interrupts this window with the chair. */
    for (hit = 0; hit < 10; hit++)
        release_damage();
    assert(pico_frame(&app.game, "/room2/punk3_health") == 12);
    assert(pico_frame(&app.game, "/room2") == 217);
    assert(app.game.instances[pico_find(&app.game, 0, "/room2")].playing);
    wait_room(101, 40);
    assert(pico_frame(&app.game, "/health") == 2);
    attack_to_stun();
    for (hit = 0; hit < 9; hit++)
        release_damage();
    assert(pico_frame(&app.game, "/room2/punk3_health") == 21);
    assert(pico_frame(&app.game, "/room2") == 257);
    assert(app.game.instances[pico_find(&app.game, 0, "/room2")].playing);
    wait_room(294, 45);
    assert(pico_frame(&app.game, "/continue") == 3);
    assert(host_render(&app));
    clean_runtime();
    host_close(&app);
}

int main(int argc, char **argv) {
    unsigned width, height;
    assert(argc == 4);
    width = (unsigned)strtoul(argv[2], 0, 10);
    height = (unsigned)strtoul(argv[3], 0, 10);
    run(argv[1], width, height, 0);
    run(argv[1], width, height, 1);
    puts("Alucard: bodies, stun, release damage, resumed attacks, chair and defeat pass.");
    return 0;
}
