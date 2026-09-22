#ifndef PICO_TOUCH_INPUT_H
#define PICO_TOUCH_INPUT_H
#include "pico.h"

/* Optional finger tolerance in original stage pixels, signed 16.16.
 * Exact hits win. A held gesture can only snap back to its captured instance.
 * Outside-stage coordinates cancel. Radius zero retains precise input.
 * Returns the number of geometry probes, excluding pico_pointer's final hit.
 * No additional persistent state or allocation is required. */
unsigned host_touch_pointer(Pico *game, int32_t x, int32_t y, int down, int32_t radius);

/* Game-specific finger allowance: a fresh press in Hanzou's fight first uses
 * the ordinary tolerance, then searches up to 64 stage pixels for his target
 * only. Precise input, held gestures and all other buttons stay unchanged.
 * At most 513 probes precede one normal pointer dispatch. */
unsigned host_touch_game_pointer(Pico *game, int32_t x, int32_t y, int down, int32_t radius);
#endif
