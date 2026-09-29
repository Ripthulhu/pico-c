#include "pico.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static Pico game;
static PicoSymbol symbols[7];
static PicoFrame frames[20];
static PicoPlacement placements[8];
static const PicoColor colors[] = {PICO_COLOR_IDENTITY};
static uint16_t refs[12];
static PicoAction actions[20];
static const char *const strings[] = {"",        "health", "room",      "dispatch",
                                      "/health", "/",      "/dispatch", "middle"};
static PicoData data = {symbols, frames, refs, placements, colors, actions, 0, strings,
                        20,      12,     8,    20,         0,      7,       8, 1};
static unsigned draw_count, button_count;
static const PicoSoundEnvelope envelope[] = {{0, 32768, 32768}, {44100, 0, 0}};
static PicoSoundEvent sounds[2];
static const PicoSoundEvent *last_sound;
static unsigned sound_count, stop_count;

static void setup(void) {
    unsigned i;
    PicoMatrix m = PICO_IDENTITY;
    memset(symbols, 0, sizeof(symbols));
    memset(frames, 0, sizeof(frames));
    memset(placements, 0, sizeof(placements));
    memset(actions, 0, sizeof(actions));
    memset(refs, 0, sizeof(refs));
    memset(sounds, 0, sizeof(sounds));
    data.sounds = sounds;
    data.sound_count = 2;
    sound_count = stop_count = 0;
    last_sound = 0;
    symbols[0] = (PicoSymbol){0, 0, 0, 5, 0, 0, PICO_MOVIE, 0};
    symbols[1] = (PicoSymbol){5, 0, 0, 6, 0, 0, PICO_MOVIE, 0};
    symbols[2] = (PicoSymbol){11, 0, 0, 1, 0, 0, PICO_MOVIE, 0};
    symbols[3] = (PicoSymbol){12, 0, 0, 2, 0, 0, PICO_MOVIE, 0};
    symbols[4] = (PicoSymbol){14, 0, 13, 4, 0, 1, PICO_BUTTON, 0};
    symbols[5] = (PicoSymbol){0, 0, 0, 0, 0, 0, PICO_LEAF, 0};
    symbols[6] = (PicoSymbol){18, 0, 0, 2, 0, 0, PICO_MOVIE, 0};
    for (i = 0; i < 8; i++) {
        placements[i].matrix = m;
        placements[i].life = i + 1;
        placements[i].depth = (uint16_t)(i + 1);
    }
    placements[0].symbol = 1;
    placements[0].name = 1; /* Persistent health */
    placements[1].symbol = 2;
    placements[1].name = 2; /* Room */
    placements[2].symbol = 3;
    placements[2].name = 3; /* Delayed controller */
    placements[3] = placements[1];
    placements[3].life = 4;
    placements[3].symbol = 6;
    placements[4] = placements[0];
    placements[4].life = 5;
    placements[5].symbol = 4;
    placements[5].depth = 1; /* Room button */
    placements[6].symbol = 5;
    placements[6].depth = 1; /* Button shape */
    placements[7] = placements[0];
    placements[7].matrix.tx = 65536 * 7;
    refs[0] = 0;
    refs[1] = 1;
    refs[2] = 2;
    refs[3] = 7;
    refs[4] = 3;
    refs[5] = 2;
    refs[6] = 4;
    refs[7] = 5;
    refs[8] = 6;
    actions[0] = (PicoAction){0, 0, 0, PICO_STOP, 0};
    actions[1] = (PicoAction){4, 5, 0, PICO_GOTO_STOP, 0};
    actions[2] = (PicoAction){4, 0, 0, PICO_NEXT, 0};
    actions[3] = (PicoAction){0, 1, 0, PICO_GOTO_STOP, 0};
    actions[4] = (PicoAction){5, 4, 0, PICO_GOTO_STOP, 0};
    actions[5] = (PicoAction){4, 7, 0, PICO_LABEL_STOP, 0};
    for (i = 0; i < 5; i++) {
        frames[i].first_action = 0;
        frames[i].action_count = 1;
    }
    frames[1].first_placement = 0;
    frames[1].placement_count = 3;
    frames[2].first_placement = 3;
    frames[2].placement_count = 3;
    frames[4].first_placement = 6;
    frames[4].placement_count = 1;
    for (i = 5; i < 11; i++) {
        frames[i].first_action = 0;
        frames[i].action_count = 1;
    }
    frames[7].label = 7;
    frames[10].first_action = 4;
    frames[11].first_placement = 7;
    frames[11].placement_count = 1;
    frames[12].action_count = 1;
    frames[13].first_action = 2;
    frames[13].action_count = 2;
    for (i = 14; i < 18; i++) {
        frames[i].first_placement = 8;
        frames[i].placement_count = 1;
    }
    actions[13] = (PicoAction){4, 0, 0, PICO_NEXT, 0};
    draw_count = button_count = 0;
}
static void draw(void *user, uint16_t symbol, const PicoMatrix *m, const PicoColor *c,
                 uint16_t ratio, uint8_t kind) {
    (void)user;
    (void)c;
    (void)ratio;
    assert(symbol == 5);
    assert(kind == 0);
    assert(m->a == 65536);
    draw_count++;
}
static int hit(void *user, uint16_t symbol, const PicoMatrix *m, int32_t x, int32_t y) {
    (void)user;
    assert(symbol == 5);
    return x >= m->tx && x < m->tx + 655360 && y >= m->ty && y < m->ty + 655360;
}
static void button(void *user, uint16_t instance, uint16_t symbol, const PicoMatrix *m) {
    (void)user;
    (void)m;
    assert(game.instances[instance].alive);
    assert(symbol == 4);
    button_count++;
}
static void test_default_start_frame(void) {
    setup();
    assert(pico_init(&game, &data, 0, 0));
    assert(pico_frame(&game, "/") == 1);
    assert(!game.instances[0].playing && game.live_instances == 1);
    assert(pico_find(&game, 0, "/health") == PICO_NONE);
    pico_tick(&game);
    assert(pico_frame(&game, "/") == 1 && game.errors == 0);

    /* Explicit diagnostic entry still builds that frame's own children. */
    assert(pico_init(&game, &data, 0, 2));
    assert(pico_frame(&game, "/") == 2);
    assert(pico_frame(&game, "/health") == 1);
    assert(!game.instances[0].playing && game.errors == 0);
}
static void test_counters(void) {
    uint16_t health;
    uint32_t generation;
    setup();
    assert(pico_init(&game, &data, 0, 2));
    health = pico_find(&game, 0, "/health");
    generation = game.instances[health].generation;
    assert(pico_frame(&game, "/health") == 1);
    pico_play(&game, "/health");
    pico_play(&game, "/health");
    assert(pico_frame(&game, "/health") == 1);
    pico_tick(&game);
    assert(pico_frame(&game, "/health") == 2);
    pico_tick(&game);
    assert(pico_frame(&game, "/health") == 2);
    pico_next(&game, "/health");
    assert(pico_frame(&game, "/health") == 3);
    pico_tick(&game);
    assert(pico_frame(&game, "/health") == 3);
    pico_goto(&game, "/", 3, 0);
    assert(pico_frame(&game, "/health") == 3);
    assert(pico_find(&game, 0, "/health") == health);
    assert(game.instances[health].generation == generation);
    assert(placements[game.instances[health].placement].matrix.tx == 7 * 65536);
    pico_goto(&game, "/", 2, 0);
    assert(pico_frame(&game, "/health") == 3);
    pico_play(&game, "/dispatch");
    assert(pico_frame(&game, "/health") == 3);
    pico_tick(&game);
    assert(pico_frame(&game, "/health") == 4);
    assert(pico_frame(&game, "/dispatch") == 1);
    pico_tick(&game);
    assert(pico_frame(&game, "/health") == 4);
    pico_next(&game, "/health");
    pico_next(&game, "/health");
    assert(pico_frame(&game, "/") == 4);
    assert(!pico_frame(&game, "/health"));
    pico_goto(&game, "/", 5, 0);
    assert(pico_frame(&game, "/health") == 1);
    assert(game.instances[pico_find(&game, 0, "/health")].generation != generation);
    assert(game.errors == 0);
}
static void test_paths_and_stale_actions(void) {
    uint16_t room;
    setup();
    frames[1].first_action = 1;
    assert(pico_init(&game, &data, 0, 2));
    assert(pico_frame(&game, "/health") == 5); /* Child frame1 script invalidated. */
    room = pico_find(&game, 0, "/room");
    assert(pico_find(&game, room, "../health") == pico_find(&game, 0, "_root.health"));
    assert(pico_find(&game, room, "_parent/health") == pico_find(&game, 0, "/health"));
    assert(pico_find(&game, room, "this") == room);
    frames[1].first_action = 5;
    pico_goto(&game, "/", 3, 0);
    pico_goto(&game, "/", 2, 0);
    assert(pico_frame(&game, "/health") == 3);
    assert(game.errors == 0 && game.missing_labels == 0 && game.missing_targets == 0);
}
static void test_input_and_render(void) {
    PicoHost host = {0, draw, hit, 0, 0, 0};
    setup();
    assert(pico_init(&game, &data, &host, 2));
    pico_render(&game);
    assert(draw_count == 1);
    pico_buttons(&game, button, 0);
    assert(button_count == 1);
    pico_pointer(&game, 65536, 65536, 1);
    assert(pico_frame(&game, "/health") == 1);
    pico_pointer(&game, 65536, 65536, 0);
    assert(pico_frame(&game, "/health") == 2);
    /* Releasing outside cancels, and reusing a removed slot must not click it. */
    pico_pointer(&game, 65536, 65536, 1);
    pico_pointer(&game, 65536 * 20, 65536 * 20, 0);
    assert(pico_frame(&game, "/health") == 2);
    pico_pointer(&game, 65536, 65536, 1);
    pico_goto(&game, "/", 3, 0);
    pico_goto(&game, "/", 2, 0);
    pico_pointer(&game, 65536, 65536, 0);
    assert(pico_frame(&game, "/health") == 2);
    assert(game.errors == 0);
}
static void test_running_action_owner_removed(void) {
    uint16_t room, button, replacement;
    uint32_t generation;
    setup();
    symbols[4].first_release = 14;
    symbols[4].release_count = 3;
    actions[14] = (PicoAction){5, 3, 0, PICO_GOTO_STOP, 0};
    actions[15] = (PicoAction){0, 2, 0, PICO_GOTO_STOP, 0};
    actions[16] = (PicoAction){4, 0, 0, PICO_NEXT, 0};
    assert(pico_init(&game, &data, 0, 2));
    room = pico_find(&game, 0, "/room");
    button = game.instances[room].child;
    generation = game.instances[room].generation;
    /* The first action removes its owner. Its relative action must not touch
     * the recycled slot, but the following absolute health action still runs. */
    pico_button(&game, button, 0);
    replacement = pico_find(&game, 0, "/room");
    assert(replacement == room && game.instances[replacement].generation != generation);
    assert(game.instances[replacement].frame == 1 && game.instances[replacement].playing);
    assert(pico_frame(&game, "/health") == 2);
    assert(game.missing_targets == 1 && !game.errors);
    assert(pico_find(&game, PICO_NONE, "_root.health") == pico_find(&game, 0, "/health"));
    assert(pico_find(&game, PICO_NONE, "/health") == pico_find(&game, 0, "/health"));
    assert(pico_find(&game, PICO_NONE, "health") == PICO_NONE);
}
static void test_independent_children(void) {
    setup();
    assert(pico_init(&game, &data, 0, 3));
    assert(!game.instances[0].playing);
    assert(pico_frame(&game, "/room") == 1);
    pico_tick(&game);
    assert(pico_frame(&game, "/room") == 2);
    pico_tick(&game);
    assert(pico_frame(&game, "/room") == 1);
    assert(game.errors == 0);
}
static void test_delayed_root_frame_guard(void) {
    unsigned root;
    for (root = 2; root <= 3; root++) {
        uint16_t dispatch;
        setup();
        frames[13].first_action = 14;
        frames[13].action_count = 3;
        actions[14] = (PicoAction){0, 1, 2, PICO_IF_ROOT_NOT_FRAME_SKIP, 0};
        actions[15] = (PicoAction){4, 0, 0, PICO_NEXT, 0};
        actions[16] = (PicoAction){0, 0, 0, PICO_STOP, 0};
        assert(pico_init(&game, &data, 0, 2));
        pico_play(&game, "/dispatch");
        if (root == 3)
            pico_goto(&game, "/", 3, 0);
        pico_tick(&game);
        assert(pico_frame(&game, "/health") == (root == 2 ? 2 : 1));
        dispatch = pico_find(&game, 0, "/dispatch");
        assert(game.instances[dispatch].frame == 2 && !game.instances[dispatch].playing);
        pico_tick(&game);
        assert(pico_frame(&game, "/health") == (root == 2 ? 2 : 1));
        assert(!game.errors && !game.missing_targets);
    }

    setup();
    frames[13].first_action = 14;
    frames[13].action_count = 3;
    actions[14] = (PicoAction){0, UINT16_MAX, 3, PICO_IF_ROOT_NOT_FRAME_SKIP, 0};
    actions[15] = (PicoAction){4, 0, 0, PICO_NEXT, 0};
    actions[16] = (PicoAction){0, 0, 0, PICO_STOP, 0};
    assert(pico_init(&game, &data, 0, 2));
    pico_play(&game, "/dispatch");
    pico_tick(&game);
    assert(pico_frame(&game, "/health") == 1 && !game.errors);
}
static void sound(void *user, const PicoSoundEvent *event) {
    (void)user;
    sound_count++;
    last_sound = event;
}
static void stop_sounds(void *user) {
    (void)user;
    stop_count++;
}
static void test_hit_queries(void) {
    PicoHost host = {0, draw, hit, sound, stop_sounds, 0};
    Pico before;
    uint16_t bottom, top = PICO_NONE, i;
    setup();
    /* A root button overlaps a button inside the room. The higher authored
     * depth wins, regardless of the nesting of the lower button. */
    placements[7] = placements[5];
    placements[7].life = 8;
    placements[7].depth = 4;
    placements[7].matrix.tx = 5 * 65536;
    frames[1].placement_count = 4;
    symbols[4].first_press = 13;
    symbols[4].press_count = 1;
    sounds[0] = (PicoSoundEvent){218, 1, 0, 0, 0, 0};
    frames[16].sound_count = 1;
    assert(pico_init(&game, &data, &host, 2));
    bottom = game.instances[pico_find(&game, 0, "/room")].child;
    for (i = 1; i < PICO_MAX_INSTANCES; i++)
        if (game.instances[i].alive && game.instances[i].placement == 7)
            top = i;
    assert(top != PICO_NONE && bottom != PICO_NONE && top != bottom);

    before = game;
    assert(pico_hit_test(&game, 65536, 65536) == bottom);
    assert(pico_hit_test(&game, 6 * 65536, 65536) == top);
    assert(pico_hit_test(&game, 20 * 65536, 20 * 65536) == PICO_NONE);
    assert(!memcmp(&before, &game, sizeof(game)));
    assert(!sound_count && !stop_count && !draw_count);
    assert(pico_frame(&game, "/health") == 1);

    pico_pointer(&game, 6 * 65536, 65536, 1);
    assert(game.pressed == top && game.hover == top);
    assert(sound_count == 1 && pico_frame(&game, "/health") == 2);
    before = game;
    assert(pico_hit_test(&game, 65536, 65536) == bottom);
    assert(pico_hit_test(&game, 20 * 65536, 20 * 65536) == PICO_NONE);
    assert(pico_hit_test(&game, 6 * 65536, 65536) == top);
    assert(!memcmp(&before, &game, sizeof(game)));
    assert(sound_count == 1 && !stop_count && !draw_count);
    pico_pointer(&game, 6 * 65536, 65536, 0);
    assert(pico_frame(&game, "/health") == 3 && sound_count == 1);
    assert(!game.errors);

    game.host.hit = NULL;
    before = game;
    assert(pico_hit_test(&game, 65536, 65536) == PICO_NONE);
    assert(!memcmp(&before, &game, sizeof(game)));
    assert(pico_hit_test(NULL, 0, 0) == PICO_NONE);
}
static void test_hit_query_diagnostics(void) {
    PicoHost host = {0, 0, hit, sound, stop_sounds, 0};
    Pico before;
    unsigned expected;
    setup();
    assert(pico_init(&game, &data, &host, 2));
    before = game;
    /* The hit-only frame isn't entered during initialisation. Corrupt it
     * afterwards to compare query diagnostics with the pointer path. */
    frames[17].first_placement = data.placement_ref_count;
    assert(pico_hit_test(&game, 65536, 65536) == PICO_NONE);
    expected = game.errors;
    assert(expected == PICO_ERROR_BAD_DATA);
    before.errors = expected;
    assert(!memcmp(&before, &game, sizeof(game)));
    game.errors = 0;
    pico_pointer(&game, 65536, 65536, 0);
    assert(game.errors == expected && !sound_count && !stop_count);

    setup();
    assert(pico_init(&game, &data, &host, 2));
    before = game;
    placements[6].symbol = 4;
    assert(pico_hit_test(&game, 65536, 65536) == PICO_NONE);
    assert(game.errors == PICO_ERROR_NESTING);
    before.errors = PICO_ERROR_NESTING;
    assert(!memcmp(&before, &game, sizeof(game)));
}
static void test_timeline_sound_cues(void) {
    PicoHost host = {0, 0, 0, sound, stop_sounds, 0};
    setup();
    sounds[0] = (PicoSoundEvent){52, 4, 0, 1, 2, envelope};
    frames[1].sound_count = 1;
    actions[14] = (PicoAction){0, 0, 0, PICO_STOP_SOUNDS, 0};
    frames[2].first_action = 14;
    assert(pico_init(&game, &data, &host, 2));
    assert(sound_count == 1 && last_sound == &sounds[0]);
    assert(last_sound->loops == 4 && last_sound->no_multiple);
    assert(last_sound->envelope_count == 2 && last_sound->envelope == envelope);
    assert(last_sound->envelope[1].position == 44100);
    pico_goto(&game, "/", 2, 0);
    assert(sound_count == 1);
    pico_goto(&game, "/", 3, 0);
    assert(stop_count == 1);
    pico_goto(&game, "/", 2, 0);
    assert(sound_count == 2);
    sounds[0].envelope = 0;
    pico_goto(&game, "/", 3, 0);
    pico_goto(&game, "/", 2, 0);
    assert(game.errors & PICO_ERROR_BAD_DATA);
    assert(sound_count == 2);
}
static void test_button_sound_transitions(void) {
    PicoHost host = {0, 0, hit, sound, 0, 0};
    uint16_t button_instance;
    setup();
    sounds[0] = (PicoSoundEvent){218, 4, 0, 0, 0, 0};
    frames[16].sound_count = 1;
    assert(pico_init(&game, &data, &host, 2));
    assert(sound_count == 0);
    pico_pointer(&game, 65536, 65536, 0);
    assert(sound_count == 0);
    pico_pointer(&game, 65536, 65536, 1);
    button_instance = game.pressed;
    assert(sound_count == 1 && last_sound == &sounds[0]);
    pico_pointer(&game, 65536, 65536, 1);
    pico_pointer(&game, 65536 * 20, 65536 * 20, 1);
    pico_pointer(&game, 65536, 65536, 1);
    assert(sound_count == 1);
    assert(game.instances[button_instance].frame == 3);
    pico_pointer(&game, 65536, 65536, 0);
    assert(sound_count == 1);
    pico_button(&game, button_instance, 1);
    assert(sound_count == 2);
    pico_button(&game, button_instance, 0);
    assert(sound_count == 2 && game.errors == 0);
}
static void test_bounded_failures(void) {
    setup();
    placements[0].clip_depth = 2;
    assert(!pico_init(&game, &data, 0, 2));
    assert(game.errors & PICO_ERROR_BAD_DATA);
    setup();
    frames[12].first_action = 6;
    actions[6] = (PicoAction){0, 2, 0, PICO_GOTO_STOP, 0};
    actions[7] = (PicoAction){0, 1, 0, PICO_GOTO_STOP, 0};
    frames[13].first_action = 7;
    frames[13].action_count = 1;
    assert(!pico_init(&game, &data, 0, 2));
    assert(game.errors & PICO_ERROR_ACTION_LIMIT);
    setup();
    refs[0] = 9999;
    assert(!pico_init(&game, &data, 0, 2));
    assert(game.errors & PICO_ERROR_BAD_DATA);
}
int main(void) {
    test_default_start_frame();
    test_counters();
    test_paths_and_stale_actions();
    test_input_and_render();
    test_running_action_owner_removed();
    test_independent_children();
    test_delayed_root_frame_guard();
    test_hit_queries();
    test_hit_query_diagnostics();
    test_timeline_sound_cues();
    test_button_sound_transitions();
    test_bounded_failures();
    printf("runtime tests passed; context=%lu bytes; instance=%lu bytes; queue=%lu bytes\n",
           (unsigned long)sizeof(Pico), (unsigned long)sizeof(PicoInstance),
           (unsigned long)sizeof(PicoQueued));
    return 0;
}
