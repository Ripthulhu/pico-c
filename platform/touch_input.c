#include "touch_input.h"

/* Eight nearest-first rings, with 32 directions per ring. Q14 directions
 * keep the optional adapter independent of floating point and libm. */
static const int16_t directions[32][2] = {
    {16384, 0},       {16069, 3196},   {15137, 6270},   {13623, 9102},   {11585, 11585},
    {9102, 13623},    {6270, 15137},   {3196, 16069},   {0, 16384},      {-3196, 16069},
    {-6270, 15137},   {-9102, 13623},  {-11585, 11585}, {-13623, 9102},  {-15137, 6270},
    {-16069, 3196},   {-16384, 0},     {-16069, -3196}, {-15137, -6270}, {-13623, -9102},
    {-11585, -11585}, {-9102, -13623}, {-6270, -15137}, {-3196, -16069}, {0, -16384},
    {3196, -16069},   {6270, -15137},  {9102, -13623},  {11585, -11585}, {13623, -9102},
    {15137, -6270},   {16069, -3196}};

static int on_stage(int32_t x, int32_t y) {
    return x >= 0 && x < 550 * 65536 && y >= 0 && y < 350 * 65536;
}

static unsigned touch_pointer(Pico *game, int32_t x, int32_t y, int down, int32_t radius,
                              int game_targets) {
    uint16_t hit, captured = PICO_NONE;
    unsigned probes = 0, ring, direction, pass, passes = 1;
    if (!game || !game->data)
        return 0;
    if (!on_stage(x, y)) {
        pico_pointer(game, -65536, -65536, 0);
        return 0;
    }
    if (radius <= 0) {
        pico_pointer(game, x, y, down);
        return 0;
    }
    if (radius > 32 * 65536)
        radius = 32 * 65536;
    hit = pico_hit_test(game, x, y);
    probes++;
    if (hit != PICO_NONE || (!down && !game->pointer_down))
        goto deliver;
    if (game->pointer_down) {
        captured = game->pressed;
        if (captured >= PICO_MAX_INSTANCES || !game->instances[captured].alive ||
            game->instances[captured].generation != game->pressed_generation)
            goto deliver;
    }
    if (game_targets && down && !game->pointer_down && game->instances[0].frame == 15)
        passes = 2;
    for (pass = 0; pass < passes; pass++) {
        int32_t reach = pass ? 64 * 65536 : radius;
        for (ring = 1; ring <= 8; ring++) {
            int32_t distance = (int32_t)((int64_t)reach * ring / 8);
            for (direction = 0; direction < 32; direction++) {
                int32_t candidate_x =
                    x + (int32_t)((int64_t)distance * directions[direction][0] / 16384);
                int32_t candidate_y =
                    y + (int32_t)((int64_t)distance * directions[direction][1] / 16384);
                if (!on_stage(candidate_x, candidate_y))
                    continue;
                hit = pico_hit_test(game, candidate_x, candidate_y);
                probes++;
                if (hit == PICO_NONE || (captured != PICO_NONE && hit != captured))
                    continue;
                /* Hanzou moves farther than his hitbox during a finger's
                 * approach. Give only his press target the wider fallback,
                 * after every ordinary nearby button has had priority. */
                if (pass && (hit >= PICO_MAX_INSTANCES || !game->instances[hit].alive ||
                             game->instances[hit].symbol != 739))
                    continue;
                x = candidate_x;
                y = candidate_y;
                goto deliver;
            }
        }
    }
deliver:
    /* Only this call changes hover/capture or dispatches press/release actions.
     * The core validates the captured generation if a press replaced a clip. */
    pico_pointer(game, x, y, down);
    return probes;
}

unsigned host_touch_pointer(Pico *game, int32_t x, int32_t y, int down, int32_t radius) {
    return touch_pointer(game, x, y, down, radius, 0);
}

unsigned host_touch_game_pointer(Pico *game, int32_t x, int32_t y, int down, int32_t radius) {
    return touch_pointer(game, x, y, down, radius, 1);
}
