#ifndef PICO_UI_H
#define PICO_UI_H
#include <stdint.h>
/* Readable 5x7 text with 6x8 cells; word-wrapped, uppercase English glyphs.
 * Caller supplies one RGB565 scanline. No allocation or framebuffer required.
 * text_row is the row inside the reader area; page starts at zero.
 */
int pico_ui_pages(const char *text, int width, int rows_per_page);
void pico_ui_text_row(uint16_t *row, int width, int text_row, const char *text, int page,
                      int rows_per_page);
#endif
