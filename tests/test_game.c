#include "game_data.h"
#include "pico.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static Pico game;
static uint16_t buttons[128];
static uint32_t generations[128];
static unsigned button_count;
static unsigned draws, peak_draws, peak_draw_root;
static uint32_t bound_instances[1025], bound_leaves[1025];
static uint8_t bound_status[1025];
static void structural_bound(uint16_t symbol) {
    const PicoSymbol *s;
    unsigned frame, count;
    uint32_t max_instances = 0, max_leaves = 0;
    assert(symbol < 1025 && symbol < pico_game_data.symbol_count);
    if (bound_status[symbol] == 2)
        return;
    assert(bound_status[symbol] != 1); /* The authored character graph is a DAG. */
    bound_status[symbol] = 1;
    s = &pico_game_data.symbols[symbol];
    if (s->kind == PICO_LEAF)
        bound_leaves[symbol] = 1;
    else if (s->kind == PICO_MOVIE || s->kind == PICO_BUTTON) {
        count = s->kind == PICO_BUTTON ? 3 : s->frame_count;
        for (frame = 0; frame < count; frame++) {
            const PicoFrame *f = &pico_game_data.frames[s->first_frame + frame];
            uint32_t instances = 0, leaves = 0;
            unsigned j;
            for (j = 0; j < f->placement_count; j++) {
                const PicoPlacement *pl =
                    &pico_game_data
                         .placements[pico_game_data.placement_refs[f->first_placement + j]];
                structural_bound(pl->symbol);
                instances += bound_instances[pl->symbol];
                leaves += bound_leaves[pl->symbol];
            }
            if (instances > max_instances)
                max_instances = instances;
            if (leaves > max_leaves)
                max_leaves = leaves;
        }
        bound_instances[symbol] = max_instances + 1;
        bound_leaves[symbol] = max_leaves;
    }
    bound_status[symbol] = 2;
}
static void count_draw(void *user, uint16_t symbol, const PicoMatrix *world, const PicoColor *color,
                       uint16_t ratio, uint8_t kind) {
    (void)user;
    (void)symbol;
    (void)world;
    (void)color;
    (void)ratio;
    assert(kind == PICO_DRAW_LEAF);
    draws++;
}
static const PicoHost host = {0, count_draw, 0, 0, 0, 0};
static void sample_draws(void) {
    draws = 0;
    pico_render(&game);
    if (draws > peak_draws) {
        peak_draws = draws;
        peak_draw_root = pico_frame(&game, "/");
    }
}
static void collect(void *user, uint16_t instance, uint16_t symbol, const PicoMatrix *world) {
    (void)user;
    (void)symbol;
    (void)world;
    if (button_count < 128) {
        buttons[button_count] = instance;
        generations[button_count++] = game.instances[instance].generation;
    }
}
static void clear_game(void) {
    assert(pico_init(&game, &pico_game_data, &host, 2));
    sample_draws();
}
static void ticks(unsigned n) {
    while (n--) {
        pico_tick(&game);
        sample_draws();
    }
    assert(!game.errors);
}
static void test_health(void) {
    unsigned i;
    uint16_t health;
    uint32_t generation;
    clear_game();
    health = pico_find(&game, 0, "/health");
    assert(health != PICO_NONE);
    generation = game.instances[health].generation;
    assert(pico_frame(&game, "/pico") == 5);
    for (i = 1; i < 6; i++) {
        assert(pico_frame(&game, "/health") == i);
        pico_play(&game, "/health");
        pico_play(&game, "/health");
        ticks(1);
    }
    assert(pico_frame(&game, "/") == 13);
    pico_goto(&game, "/", 5, 0);
    assert(pico_find(&game, 0, "/health") == health);
    assert(game.instances[health].generation == generation);
    assert(pico_frame(&game, "/health") == 6);
    pico_goto(&game, "/health", 1, 0);
    pico_next(&game, "/health");
    ticks(2);
    assert(pico_frame(&game, "/health") == 2);
    assert(!game.errors);
}
static void test_heal_and_continue(void) {
    clear_game();
    pico_goto(&game, "/health", 5, 0);
    pico_play(&game, "/smoke");
    ticks(29);
    assert(pico_frame(&game, "/health") == 5);
    ticks(1);
    assert(pico_frame(&game, "/health") == 1);
    pico_goto(&game, "/continue", 5, 0);
    pico_goto(&game, "/", 13, 0);
    pico_play(&game, "/continue");
    ticks(1);
    assert(pico_frame(&game, "/") == 9);
    assert(pico_frame(&game, "/continue") == 5);
    assert(pico_frame(&game, "/health") == 1);
    /* Replay returns to the original Play screen and discards old controllers.
     * The pointer-driven startup test covers the complete intro from here. */
    pico_goto(&game, "/health", 4, 0);
    pico_goto(&game, "/", 1, 0);
    assert(pico_frame(&game, "/") == 1);
    assert(!game.instances[0].playing);
    assert(pico_find(&game, 0, "/health") == PICO_NONE);
    assert(pico_find(&game, 0, "/icon_gun") == PICO_NONE);
    ticks(48);
    assert(pico_frame(&game, "/") == 1);
    pico_goto(&game, "/", 2, 0);
    assert(pico_frame(&game, "/health") == 1);
    assert(pico_frame(&game, "/icon_gun") == 1);
}
static void test_boss_counters(void) {
    clear_game();
    pico_goto(&game, "/", 15, 0);
    pico_goto(&game, "/room", 63, 0);
    assert(pico_frame(&game, "/room/ninja_health") == 1);
    pico_goto(&game, "/room/ninja_health", 50, 0);
    pico_next(&game, "/room/ninja_health");
    assert(pico_frame(&game, "/room") == 250);
    clear_game();
    pico_goto(&game, "/", 18, 0);
    pico_goto(&game, "/room", 189, 0);
    assert(pico_frame(&game, "/room/demon_health") == 1);
    pico_goto(&game, "/room/demon_health", 49, 0);
    pico_next(&game, "/room/demon_health");
    assert(pico_frame(&game, "/room") == 230);
    pico_goto(&game, "/room/demon_health", 207, 0);
    pico_next(&game, "/room/demon_health");
    assert(pico_frame(&game, "/room") == 265);
    assert(!game.errors);
}
static void test_casandra_barrel_restart(void) {
    unsigned i, elapsed;
    uint16_t barrel = PICO_NONE, demon, room;
    uint32_t generation;
    clear_game();
    pico_goto(&game, "/", 18, 0);
    pico_goto(&game, "/room", 189, 1);
    ticks(1);
    assert(pico_frame(&game, "/room") == 190);
    button_count = 0;
    pico_buttons(&game, collect, 0);
    for (i = 0; i < button_count; i++)
        if (game.instances[buttons[i]].symbol == 904)
            barrel = buttons[i];
    assert(barrel != PICO_NONE);
    pico_button(&game, barrel, 1);
    pico_button(&game, barrel, 0);
    for (elapsed = 0; elapsed < 32 && pico_frame(&game, "/room") != 221; elapsed++)
        ticks(1);
    assert(pico_frame(&game, "/room") == 221);
    room = pico_find(&game, 0, "/room");
    demon = pico_find(&game, 0, "/room/demon");
    assert(room != PICO_NONE && demon != PICO_NONE);
    assert(!game.instances[room].playing);
    generation = game.instances[demon].generation;
    /* Demon frame 91 removes its own clip by returning the room to the barrel
     * toss. Its following absolute play() must still restart the room. */
    for (elapsed = 0; elapsed < 100 && pico_frame(&game, "/room") == 221; elapsed++)
        ticks(1);
    assert(pico_frame(&game, "/room") == 190);
    assert(!game.instances[demon].alive || game.instances[demon].generation != generation);
    assert(game.instances[room].playing);
    ticks(1);
    assert(pico_frame(&game, "/room") == 191);
    puts("Casandra barrel restart continues after its demon clip is removed");
}
static void test_scenes(void) {
    unsigned root, peak = 0, peak_queue = 0;
    uint32_t missing = 0;
    for (root = 2; root <= 23; root++) {
        clear_game();
        pico_goto(&game, "/", (uint16_t)root, 0);
        sample_draws();
        ticks(480);
        if (game.peak_instances > peak)
            peak = game.peak_instances;
        if (game.peak_queue > peak_queue)
            peak_queue = game.peak_queue;
        missing += game.missing_targets;
        assert(game.missing_labels == 0);
    }
    printf(
        "all 22 scene entries + 480 ticks each: peak=%u instances, queue=%u, missing targets=%lu\n",
        peak, peak_queue, (unsigned long)missing);
}
static void test_button_stress(void) {
    unsigned root, step, peak = 0, peak_queue = 0;
    uint32_t rng = 0x5049434fu, missing = 0;
    for (root = 2; root <= 23; root++) {
        clear_game();
        pico_goto(&game, "/", (uint16_t)root, 0);
        sample_draws();
        for (step = 0; step < 2000; step++) {
            if (step % 3 == 0) {
                unsigned at;
                button_count = 0;
                pico_buttons(&game, collect, 0);
                rng = rng * 1664525u + 1013904223u;
                if (button_count) {
                    uint16_t n;
                    uint32_t generation;
                    at = (unsigned)(rng % button_count);
                    n = buttons[at];
                    generation = generations[at];
                    pico_button(&game, n, 1);
                    if (game.instances[n].alive && game.instances[n].generation == generation)
                        pico_button(&game, n, 0);
                }
            }
            ticks(1);
            assert(game.missing_labels == 0);
        }
        if (game.peak_instances > peak)
            peak = game.peak_instances;
        if (game.peak_queue > peak_queue)
            peak_queue = game.peak_queue;
        missing += game.missing_targets;
    }
    printf("deterministic button stress, 44000 ticks: peak=%u instances, queue=%u, missing "
           "targets=%lu\n",
           peak, peak_queue, (unsigned long)missing);
}
static void test_casandra_pressure(void) {
    unsigned phase, step, peak = 0, peak_queue = 0;
    uint32_t rng = 0x43415341u;
    /* Exercise every main encounter frame directly, allowing nested animations
     * to advance while removing player-death truncation from the capacity test.
     * These are structural stress states, not a claim of reachable play order. */
    for (phase = 1; phase <= pico_game_data.symbols[930].frame_count; phase++) {
        clear_game();
        pico_goto(&game, "/", 18, 0);
        pico_goto(&game, "/room", (uint16_t)phase, 0);
        sample_draws();
        for (step = 0; step < 120; step++) {
            pico_goto(&game, "/health", 1, 0);
            if (step % 2 == 0) {
                button_count = 0;
                pico_buttons(&game, collect, 0);
                rng = rng * 1664525u + 1013904223u;
                if (button_count) {
                    unsigned at = (unsigned)(rng % button_count);
                    uint16_t n = buttons[at];
                    uint32_t generation = generations[at];
                    pico_button(&game, n, 1);
                    if (game.instances[n].alive && game.instances[n].generation == generation)
                        pico_button(&game, n, 0);
                }
            }
            ticks(1);
            assert(game.missing_labels == 0);
        }
        if (game.peak_instances > peak)
            peak = game.peak_instances;
        if (game.peak_queue > peak_queue)
            peak_queue = game.peak_queue;
    }
    printf("Casandra every-frame capacity stress: peak=%u instances, queue=%u\n", peak, peak_queue);
}
int main(void) {
    unsigned symbol;
    assert(strcmp(pico_text_for_symbol(315), "You sure are a nosey bastard, Pico.") == 0);
    for (symbol = 0; symbol < pico_game_data.symbol_count; symbol++)
        assert(strstr(pico_text_for_symbol((uint16_t)symbol), "RECORDSEPARATOR") == 0);
    structural_bound(0);
    printf("authored structural upper bounds: %lu instances including root, %lu visible leaves\n",
           (unsigned long)bound_instances[0], (unsigned long)bound_leaves[0]);
    test_health();
    test_heal_and_continue();
    test_boss_counters();
    test_casandra_barrel_restart();
    test_scenes();
    test_button_stress();
    test_casandra_pressure();
    printf("compiled game tests passed; context=%lu bytes\n", (unsigned long)sizeof(Pico));
    printf("every-tick render traversal: peak=%u visible leaves at root=%u\n", peak_draws,
           peak_draw_root);
    assert(peak_draws <= bound_leaves[0]);
    return 0;
}
