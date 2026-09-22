#ifndef PICO_GAME_DATA_H
#define PICO_GAME_DATA_H
#include "pico.h"
extern const PicoData pico_game_data;
const char *pico_button_label(uint16_t symbol);
const char *pico_text_for_symbol(uint16_t symbol);
#endif
