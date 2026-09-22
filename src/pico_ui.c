#include "pico_ui.h"

/* Simple original bitmap letter definitions; bit4 is the leftmost column. */
typedef struct Glyph {
    char c;
    uint8_t rows[7];
} Glyph;
static const Glyph glyphs[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 18, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},      {',', {0, 0, 0, 0, 0, 4, 8}},
    {'!', {4, 4, 4, 4, 4, 0, 4}},        {'?', {14, 17, 1, 2, 4, 0, 4}},
    {':', {0, 4, 4, 0, 4, 4, 0}},        {';', {0, 4, 4, 0, 4, 4, 8}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},       {'/', {1, 2, 2, 4, 8, 8, 16}},
    {'\'', {4, 4, 8, 0, 0, 0, 0}},       {'"', {10, 10, 20, 0, 0, 0, 0}},
    {'(', {2, 4, 8, 8, 8, 4, 2}},        {')', {8, 4, 2, 2, 2, 4, 8}},
    {'+', {0, 4, 4, 31, 4, 4, 0}},       {'=', {0, 0, 31, 0, 31, 0, 0}},
    {'<', {1, 2, 4, 8, 4, 2, 1}},        {'>', {16, 8, 4, 2, 4, 8, 16}},
    {'[', {14, 8, 8, 8, 8, 8, 14}},      {']', {14, 2, 2, 2, 2, 2, 14}},
    {'&', {12, 18, 20, 8, 21, 18, 13}},  {'%', {17, 2, 4, 4, 8, 16, 17}},
    {'*', {0, 21, 14, 31, 14, 21, 0}},   {'_', {0, 0, 0, 0, 0, 0, 31}},
    {' ', {0, 0, 0, 0, 0, 0, 0}}};

static uint8_t glyph_row(char c, int row) {
    unsigned i;
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
        if (glyphs[i].c == c)
            return glyphs[i].rows[row];
    return row == 0 || row == 6 ? 31 : 17;
}
static int whitespace(char c) {
    return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}
static int next_line(const char **cursor, char *out, int columns) {
    const char *p = *cursor, *start, *last_space = 0;
    int n = 0, i;
    while (*p && whitespace(*p))
        p++;
    if (!*p) {
        out[0] = 0;
        *cursor = p;
        return 0;
    }
    start = p;
    while (*p && n < columns) {
        if (whitespace(*p))
            last_space = p;
        p++;
        n++;
    }
    if (*p && !whitespace(*p) && last_space && last_space > start) {
        p = last_space;
        n = (int)(p - start);
    }
    for (i = 0; i < n; i++)
        out[i] = whitespace(start[i]) ? ' ' : start[i];
    out[n] = 0;
    *cursor = p;
    return 1;
}
static int columns_for(int width) {
    int n = width / 6;
    return n > 95 ? 95 : n;
}
int pico_ui_pages(const char *text, int width, int rows_per_page) {
    char line[96];
    int lines = 0, cols = columns_for(width);
    if (!text || cols < 1 || rows_per_page < 1)
        return 1;
    while (next_line(&text, line, cols))
        lines++;
    return lines ? (lines + rows_per_page - 1) / rows_per_page : 1;
}
void pico_ui_text_row(uint16_t *row, int width, int text_row, const char *text, int page,
                      int rows_per_page) {
    char line[96];
    int i, j, want, cols = columns_for(width), gr = text_row % 8;
    for (i = 0; i < width; i++)
        row[i] = 0;
    if (!text || cols < 1 || page < 0 || rows_per_page < 1 || text_row < 0 || gr == 7)
        return;
    want = page * rows_per_page + text_row / 8;
    for (i = 0; i <= want; i++)
        if (!next_line(&text, line, cols))
            return;
    for (i = 0; line[i] && i < cols; i++) {
        uint8_t bits = glyph_row(line[i], gr);
        for (j = 0; j < 5; j++)
            if (bits & (16 >> j))
                row[i * 6 + j] = 65535;
    }
}
