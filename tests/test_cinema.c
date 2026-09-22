#include "host_common.h"
#include "touch_input.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HostApp retained, complete;
static unsigned pictures, cinema_text, cinema_panel;

static void count_cinema(void *user, uint16_t symbol, const PicoMatrix *matrix,
                         const PicoColor *colour, uint16_t ratio, uint8_t kind) {
    (void)user;
    (void)matrix;
    (void)colour;
    (void)ratio;
    if (kind == PICO_DRAW_LEAF) {
        cinema_text += symbol == 274;
        cinema_panel += symbol == 272;
    }
}

static void mode(unsigned expected) {
    PicoHost saved = retained.game.host;
    unsigned actual = pico_frame(&retained.game, "/action_semi");
    if (actual != expected)
        fprintf(stderr,
                "Cinema mode: root %u, tick %lu, key %u, expected %u, got %u; "
                "%u pictures matched complete rendering\n",
                pico_frame(&retained.game, "/"), (unsigned long)retained.game.tick,
                pico_frame(&retained.game, "/icon_key1"), expected, actual, pictures);
    assert(actual == expected);
    assert(pico_frame(&complete.game, "/action_semi") == expected);
    cinema_text = cinema_panel = 0;
    retained.game.host.draw = count_cinema;
    pico_render(&retained.game);
    retained.game.host = saved;
    assert(cinema_text == (expected == 3));
    assert(cinema_panel == (expected != 1));
}

static void picture(void) {
    size_t bytes = (size_t)retained.width * retained.height * host_pixel_bytes(&retained);
#ifndef PICO_STREAM_RENDER
    complete.retained_valid = 0;
#endif
    assert(host_render(&retained));
    assert(host_render(&complete));
    assert(!retained.game.errors && !complete.game.errors);
    assert(!retained.draw_overflows && !complete.draw_overflows);
    assert(!memcmp(retained.pixels, complete.pixels, bytes));
    pictures++;
}

static void ticks(unsigned count) {
    while (count--) {
        host_tick(&retained);
        host_tick(&complete);
        picture();
    }
}

static void go(const char *path, unsigned frame, int play) {
    pico_goto(&retained.game, path, (uint16_t)frame, play);
    pico_goto(&complete.game, path, (uint16_t)frame, play);
    picture();
}

static void tap(unsigned symbol) {
    unsigned x, y;
    int down;
    assert(!retained.game.pointer_down && !complete.game.pointer_down);
    for (y = 1; y < 350; y += 2) {
        for (x = 1; x < 550; x += 2) {
            uint16_t n = pico_hit_test(&retained.game, (int32_t)x * 65536, (int32_t)y * 65536);
            if (n == PICO_NONE || retained.game.instances[n].symbol != symbol)
                continue;
            /* No hover before the press. Render each edge to retain the
             * picture that existed immediately before leaving the room. */
            for (down = 1; down >= 0; down--) {
                assert(host_touch_game_pointer(&retained.game, (int32_t)x * 65536,
                                               (int32_t)y * 65536, down, 14 * 65536) <= 257);
                assert(host_touch_game_pointer(&complete.game, (int32_t)x * 65536,
                                               (int32_t)y * 65536, down, 14 * 65536) <= 257);
                picture();
            }
            return;
        }
    }
    fprintf(stderr, "Missing button %u at root %u, room %u\n", symbol,
            pico_frame(&retained.game, "/"), pico_frame(&retained.game, "/room2"));
    assert(0);
}

static void reward(const char *assets, unsigned width, unsigned height) {
    assert(host_open(&retained, assets, width, height, 2, 0));
    assert(host_open(&complete, assets, width, height, 2, 0));
    retained.assist_auto = complete.assist_auto = 0;
    retained.assist = complete.assist = 0;
    /* Only the battle is skipped. Its authored victory and first dialogue
     * continuation establish the key pickup view and cinema state. */
    go("/", 3, 0);
    go("/room2", 294, 1);
    ticks(24);
    assert(pico_frame(&retained.game, "/room2") == 298);
    tap(120);
    ticks(24);
    assert(pico_frame(&retained.game, "/room2") == 308);
    mode(3);
}

static void key_finished(void) {
    assert(pico_frame(&retained.game, "/icon_key1") == 14);
    assert(pico_frame(&complete.game, "/icon_key1") == 14);
    assert(pico_frame(&retained.game, "/action_key1") == 3);
    assert(pico_frame(&complete.game, "/action_key1") == 3);
}

static void close_game(void) {
    host_close(&retained);
    host_close(&complete);
}

static void leave(const char *assets, unsigned width, unsigned height, unsigned delay) {
    reward(assets, width, height);
    tap(382);
    ticks(delay);
    tap(371);
    assert(pico_frame(&retained.game, "/") == 5);
    mode(1);
    /* The persistent inventory animation finishes after the room is gone.
     * It must retain the key without bringing its old cinema panel back. */
    ticks(24);
    key_finished();
    assert(pico_frame(&retained.game, "/") == 5);
    mode(1);
    close_game();
    printf("Alucard key: leave after %u ticks keeps the hallway inventory clear.\n", delay);
}

int main(int argc, char **argv) {
    unsigned width, height;
    assert(argc == 4);
    width = (unsigned)strtoul(argv[2], NULL, 10);
    height = (unsigned)strtoul(argv[3], NULL, 10);
    reward(argv[1], width, height);
    tap(382);
    ticks(24);
    key_finished();
    assert(pico_frame(&retained.game, "/") == 3);
    mode(3);
    close_game();
    puts("Alucard key: staying in the room preserves its authored cinema state.");
    leave(argv[1], width, height, 14);
    leave(argv[1], width, height, 0);
    leave(argv[1], width, height, 1);
    printf("Cinema regression: %u pictures match complete rendering.\n", pictures);
    return 0;
}
