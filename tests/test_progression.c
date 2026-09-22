#include "host_common.h"
#include <assert.h>
#include <stdio.h>

static HostApp app;
static const char *assets;

static void clean_runtime(void) {
    assert(!app.game.errors && !app.game.missing_labels && !app.draw_overflows);
}

static void ticks(unsigned count) {
    while (count--) {
        host_tick(&app);
        clean_runtime();
    }
}

static void open_game(void) {
    assert(host_open(&app, assets, 96, 64, 2, 0));
}

static void frame(const char *path, unsigned expected) {
    unsigned actual = pico_frame(&app.game, path);
    if (actual != expected) {
        fprintf(stderr, "%s: expected frame %u, got %u at tick %lu\n", path, expected, actual,
                (unsigned long)app.game.tick);
        assert(0);
    }
}

static void click(unsigned symbol) {
    unsigned x, y;
    assert(!app.game.pointer_down);
    for (y = 1; y < 350; y += 2) {
        for (x = 1; x < 550; x += 2) {
            uint16_t n = pico_hit_test(&app.game, (int32_t)x * 65536, (int32_t)y * 65536);
            if (n != PICO_NONE && app.game.instances[n].symbol == symbol) {
                /* A tap has no preceding hover event. */
                pico_pointer(&app.game, (int32_t)x * 65536, (int32_t)y * 65536, 1);
                pico_pointer(&app.game, (int32_t)x * 65536, (int32_t)y * 65536, 0);
                clean_runtime();
                return;
            }
        }
    }
    fprintf(stderr, "No hit pixel for button %u at root %u, room %u\n", symbol,
            pico_frame(&app.game, "/"), pico_frame(&app.game, "/room"));
    assert(0);
}

static void click_and_wait(unsigned symbol) {
    click(symbol);
    ticks(24);
}

static void test_school_key(void) {
    unsigned dialogue;
    open_game();
    /* Diagnostic entry starts after the battle covered by test_alucard.c.
     * All following dialogue, pickup and revisit actions use real hit pixels. */
    pico_goto(&app.game, "/", 3, 0);
    pico_goto(&app.game, "/room2", 294, 1);
    ticks(24);
    frame("/continue", 3);
    frame("/room2", 298);
    for (dialogue = 0; dialogue < 3; dialogue++)
        click_and_wait(120);
    frame("/room2", 310);
    click_and_wait(382);
    frame("/icon_key1", 14);
    frame("/action_key1", 3);
    click_and_wait(371);
    click_and_wait(434);
    frame("/room2", 312);
    frame("/room2/key", 2);
    frame("/icon_key1", 14);

    /* The corridor's entry controller is part of normal navigation. Root 9
     * alone is an incomplete diagnostic entry and leaves the room at frame 1. */
    pico_goto(&app.game, "/", 9, 0);
    pico_play(&app.game, "/action_room9");
    ticks(24);
    frame("/room", 2);
    click(232);
    frame("/icon_key1", 14);
    ticks(48);
    frame("/room", 46);
    frame("/icon_key1", 1);
    frame("/action_room9", 3);
    click_and_wait(593);
    frame("/", 10);
    click_and_wait(605);
    frame("/", 9);
    frame("/room", 46);
    frame("/icon_key1", 1);
    assert(host_render(&app));
    clean_runtime();
    host_close(&app);
    puts("School key: post-fight dialogue, pickup, revisits and corridor gate pass.");
}

static void test_teacher_key_and_fire(void) {
    open_game();
    /* Diagnostic entry begins at Hanzou's authored reward transition. */
    pico_goto(&app.game, "/", 15, 0);
    pico_goto(&app.game, "/room", 253, 1);
    ticks(80);
    frame("/continue", 5);
    frame("/room", 294);
    click_and_wait(751);
    frame("/icon_goggles", 14);
    frame("/action_room15", 5);
    click_and_wait(746);

    /* Skip the intervening corridors, then use the normal classroom entrance. */
    pico_goto(&app.game, "/", 16, 0);
    ticks(24);
    click_and_wait(756);
    frame("/nightvision", 1);
    click_and_wait(228);
    frame("/nightvision", 2);
    frame("/room", 2);
    click_and_wait(766);
    frame("/icon_key2", 14);
    frame("/action_key2", 3);
    click_and_wait(763);
    frame("/nightvision", 1);
    click_and_wait(756);
    click_and_wait(228);
    frame("/room/key", 2);
    frame("/icon_key2", 14);
    click_and_wait(763);
    click_and_wait(757);
    click_and_wait(437);
    frame("/icon_fire", 14);

    /* Skip the return corridors; retain the authored fire-barrier dispatcher. */
    pico_goto(&app.game, "/", 12, 0);
    pico_play(&app.game, "/firewall");
    ticks(24);
    frame("/room", 2);
    click_and_wait(224);
    frame("/room", 15);
    frame("/firewall", 3);
    frame("/action_flamewindow", 3);
    frame("/fire_onwall", 5);
    click_and_wait(235);
    frame("/room", 16);
    frame("/icon_key2", 1);
    click_and_wait(659);
    frame("/", 18);
    frame("/continue", 7);
    frame("/action_semi", 3);
    assert(host_render(&app));
    clean_runtime();
    host_close(&app);
    puts("Goggles, teacher key, extinguished fire and final door pass.");
}

static void test_checkpoints(void) {
    static const unsigned roots[] = {2, 5, 9, 18};
    unsigned checkpoint, damage;
    open_game();
    ticks(24);
    click_and_wait(84);
    click_and_wait(419);
    click_and_wait(437);
    frame("/icon_fire", 14);
    for (checkpoint = 0; checkpoint < 4; checkpoint++) {
        uint16_t icon = pico_find(&app.game, 0, "/icon_fire");
        uint32_t generation = app.game.instances[icon].generation;
        /* Select each established checkpoint without replaying its battle.
         * Player damage and the death-menu tap still use their normal paths. */
        pico_goto(&app.game, "/continue", (uint16_t)(checkpoint * 2 + 1), 0);
        for (damage = 0; damage < 5; damage++) {
            pico_play(&app.game, "/health");
            ticks(1);
        }
        frame("/", 13);
        ticks(24);
        click(670);
        ticks(1);
        frame("/", roots[checkpoint]);
        frame("/health", 1);
        frame("/continue", checkpoint * 2 + 1);
        frame("/action_semi", checkpoint == 3 ? 3 : 1);
        frame("/icon_fire", 14);
        assert(pico_find(&app.game, 0, "/icon_fire") == icon);
        assert(app.game.instances[icon].generation == generation);
    }
    host_close(&app);
    puts("All four death-menu checkpoints restore health and retain inventory.");
}

static void test_ending(void) {
    unsigned dialogue, previous_root, seen_roots = 0, limit = 200;
    uint16_t ending;
    uint32_t generation;
    open_game();
    /* Begin at the authored Casandra death state. This checks its continuation
     * and the ending, without substituting a fabricated winning health value. */
    pico_goto(&app.game, "/", 18, 0);
    pico_goto(&app.game, "/room", 265, 0);
    ticks(200);
    frame("/", 19);
    frame("/room", 115);
    frame("/action_semi", 1);
    ending = pico_find(&app.game, 0, "/room");
    generation = app.game.instances[ending].generation;
    for (dialogue = 0; dialogue < 5; dialogue++) {
        click_and_wait(120);
        frame("/", 19);
        frame("/room", 116 + dialogue);
    }
    click(120);
    previous_root = 19;
    while (pico_frame(&app.game, "/") != 22 && limit--) {
        unsigned root;
        ticks(1);
        root = pico_frame(&app.game, "/");
        assert(root >= previous_root && root <= 22);
        if (root != previous_root) {
            assert(root == previous_root + 1);
            seen_roots |= 1u << (root - 20);
            previous_root = root;
        }
        if (root < 22) {
            assert(pico_find(&app.game, 0, "/room") == ending);
            assert(app.game.instances[ending].generation == generation);
        }
    }
    frame("/", 22);
    assert(seen_roots == 7);
    assert(app.medals == 1);
    assert(app.stopped_sounds > 0);
    assert(host_render(&app));
    clean_runtime();
    host_close(&app);
    puts("Ending dialogue, retained clip across roots 19-21, credits and victory pass.");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assets = argv[1];
    test_school_key();
    test_teacher_key_and_fire();
    test_checkpoints();
    test_ending();
    return 0;
}
