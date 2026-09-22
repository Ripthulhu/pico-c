#include "host_common.h"
#include "touch_input.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Q = 65536, TRACE_LIMIT = 96 };
typedef struct Aim {
    unsigned tick;
    int x, y, valid;
} Aim;
static HostApp app;
static Aim trace[TRACE_LIMIT];
static unsigned trace_count;

static void clean_runtime(void) {
    assert(!app.game.errors && !app.game.missing_labels && !app.draw_overflows);
}

static void tick(void) {
    host_tick(&app);
    clean_runtime();
}

static unsigned health(void) {
    return pico_frame(&app.game, "/room/ninja_health");
}

static void pointer(Aim a, int down, int assisted) {
    unsigned probes;
    if (assisted)
        probes = host_touch_game_pointer(&app.game, a.x * Q, a.y * Q, down, 14 * Q);
    else
        probes = host_touch_pointer(&app.game, a.x * Q, a.y * Q, down, 14 * Q);
    assert(probes <= (assisted ? 513u : 257u));
    clean_runtime();
}

static void read_trace(const char *path) {
    char line[256];
    FILE *file = fopen(path, "r");
    assert(file);
    while (fgets(line, sizeof(line), file)) {
        Aim a = {0, 0, 0, 1};
        if (line[0] == '#')
            continue;
        assert(sscanf(line, "%u %d %d", &a.tick, &a.x, &a.y) == 3);
        assert(trace_count < TRACE_LIMIT && a.tick % 6 == 0);
        assert(a.x >= 0 && a.x < 550 && a.y >= 0 && a.y < 350);
        assert(!trace_count || a.tick > trace[trace_count - 1].tick);
        trace[trace_count++] = a;
    }
    assert(!ferror(file) && trace_count == 62);
    fclose(file);
}

/* Observe only displayed artwork. There is no button enumeration, hit query or
 * timeline-position lookup in this aiming rule. The red eye slit is the only
 * pure-red feature inside this crop while the room is dark. */
static Aim eyes(void) {
    Aim a = {0, 0, 0, 0};
    unsigned x, y, count = 0, sx = 0, sy = 0;
    assert(app.width == 1100 && app.height == 700);
    for (y = 75; y < 325; y++) {
        for (x = 20; x < 530; x++) {
            if (host_pixel_rgb(&app, (size_t)y * 2 * app.width + x * 2) == 0xff0000) {
                count++;
                sx += x;
                sy += y;
            }
        }
    }
    if (count) {
        a.x = (int)(sx / count);
        a.y = (int)(sy / count);
        a.valid = 1;
    }
    return a;
}

static void fight(const char *assets, unsigned width, unsigned height, unsigned delay, int visual,
                  int assisted) {
    Aim history[8] = {{0, 0, 0, 0}}, held = {0, 0, 0, 0};
    unsigned step, shot = 0, hits = 0, next_trace = 0, victory_tick;
    assert(delay < 8);
    assert(host_open(&app, assets, width, height, 2, 0));
    /* Skip travel and dialogue, then retain the authored attack cycle, damage,
     * health and defeat actions. Neither timeline nor health is forced again. */
    pico_goto(&app.game, "/", 15, 0);
    pico_goto(&app.game, "/room", 63, 1);
    assert(health() == 1 && pico_frame(&app.game, "/health") == 1);
    for (step = 0;
         step < 800 && pico_frame(&app.game, "/") == 15 && pico_frame(&app.game, "/room") < 250;
         step++) {
        Aim aim = {0, 0, 0, 0};
        if (visual) {
            assert(host_render(&app));
            history[step % 8] = eyes();
        }
        if (held.valid) {
            unsigned before = health();
            /* A held finger may move over the actor after its initial press.
             * It must not turn that movement into another shot. */
            pointer(held, 1, assisted);
            assert(health() == before);
            pointer(held, 0, assisted);
            assert(health() == before && !app.game.pointer_down);
            held.valid = 0;
        }
        if (visual && step >= delay && step % 6 == 0)
            aim = history[(step - delay) % 8];
        if (!visual && next_trace < trace_count && trace[next_trace].tick == step)
            aim = trace[next_trace++];
        if (aim.valid) {
            unsigned before = health(), after;
            if (visual && delay == 5) {
                assert(next_trace < trace_count && trace[next_trace].tick == step);
                assert(aim.x == trace[next_trace].x && aim.y == trace[next_trace].y);
                next_trace++;
            }
            assert(!app.game.pointer_down);
            pointer(aim, 1, assisted);
            after = health();
            assert(after == before || after == before + 1 ||
                   (before == 50 && pico_frame(&app.game, "/room") >= 250));
            if (after != before)
                hits++;
            shot++;
            held = aim;
        }
        tick();
    }
    victory_tick = app.game.tick;
    if (held.valid)
        pointer(held, 0, assisted);
    if (assisted) {
        assert(hits == 50 && pico_frame(&app.game, "/") == 15);
        assert(pico_frame(&app.game, "/health") == 5);
        assert(pico_frame(&app.game, "/room") >= 250);
        if (delay == 5)
            assert(shot == 62 && next_trace == trace_count && victory_tick == 637);
        else
            assert(delay == 3 && shot == 50 && victory_tick == 541);
        for (step = 0; step < 80 && pico_frame(&app.game, "/room") != 294; step++)
            tick();
        assert(pico_frame(&app.game, "/room") == 294);
        assert(pico_frame(&app.game, "/continue") == 5);
        assert(pico_frame(&app.game, "/action_semi") == 1);
    } else {
        /* This same delayed input used to lose. Keep the contrast explicit so
         * a test cannot silently pass by finding the current invisible target. */
        assert(delay == 5 && hits < 50);
        assert(pico_frame(&app.game, "/") == 13 && victory_tick == 641);
        assert(pico_frame(&app.game, "/health") == 6);
    }
    assert(host_render(&app));
    clean_runtime();
    printf("Hanzou %s %s: delay %u ticks, %u/%u hits, combat %u ticks, player health %u\n",
           visual ? "visible pixels" : "recorded visible pixels",
           assisted ? "assisted" : "baseline", delay, hits, shot, victory_tick,
           pico_frame(&app.game, "/health"));
    host_close(&app);
}

int main(int argc, char **argv) {
    unsigned width, height;
    int visual;
    assert(argc == 6);
    width = (unsigned)strtoul(argv[2], NULL, 10);
    height = (unsigned)strtoul(argv[3], NULL, 10);
    visual = !strcmp(argv[4], "visual");
    assert(visual || !strcmp(argv[4], "replay"));
    read_trace(argv[5]);
    if (visual)
        fight(argv[1], width, height, 3, 1, 1);
    else
        fight(argv[1], width, height, 5, 0, 0);
    fight(argv[1], width, height, 5, visual, 1);
    return 0;
}
