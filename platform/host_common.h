#ifndef PICO_HOST_COMMON_H
#define PICO_HOST_COMMON_H
#include "pico.h"
#include "pico_render.h"
#include <stdio.h>

#ifndef PICO_HOST_MAX_DRAWS
#define PICO_HOST_MAX_DRAWS 1024
#endif
#define PICO_HOST_MAX_HEIGHT 4096
#if PICO_RENDER_MAX_WIDTH < 220
#define PICO_DEFAULT_WIDTH PICO_RENDER_MAX_WIDTH
#define PICO_DEFAULT_HEIGHT (PICO_RENDER_MAX_WIDTH / 2)
#define PICO_DEFAULT_ASSETS "assets/pico_art_micro.pcta"
#else
#define PICO_DEFAULT_WIDTH 550
#define PICO_DEFAULT_HEIGHT 350
#define PICO_DEFAULT_ASSETS "assets/pico_art_rgba.pcta"
#endif

typedef struct HostSoundSink {
    void *user;
    void (*event)(void *user, const PicoSoundEvent *event);
    void (*stop)(void *user);
} HostSoundSink;

typedef struct HostApp {
    Pico game;
    PicoAssets assets;
#ifdef PICO_STREAM_RENDER
    void *scanline;
    unsigned scanline_y;
#else
    PicoDraw draws[PICO_HOST_MAX_DRAWS];
    void *asset_cache_memory;
    void *retained_pixels;
    unsigned retained_draw_count, retained_valid;
    PicoRect damage;
#endif
    const void *asset_bytes;
    size_t asset_size;
    unsigned owns_asset_bytes;
    void *pixels;
    unsigned width, height, mono;
    unsigned view_width, view_height, view_x, view_y;
    unsigned draw_count, peak_draws, draw_overflows, mask_events;
    unsigned sound_events, silent_sound_events, stopped_sounds, medals;
    HostSoundSink sound;
    unsigned render_count;
    unsigned assist, assist_auto, reading, reader_page;
    uint16_t selected;
    uint32_t selected_generation;
    uint32_t caption_epoch;
    const char *visible_text;
    double tick_seconds, render_seconds, worst_tick_seconds;
} HostApp;

int host_open(HostApp *app, const char *assets, unsigned width, unsigned height,
              unsigned start_frame, int mono);
int host_open_with_sound(HostApp *app, const char *assets, unsigned width, unsigned height,
                         unsigned start_frame, int mono, const HostSoundSink *sound);
/* Borrowed bytes must remain readable until host_close returns. */
int host_open_memory(HostApp *app, const void *bytes, size_t size, unsigned width, unsigned height,
                     unsigned start_frame, int mono, const HostSoundSink *sound);
void host_close(HostApp *app);
int host_resize(HostApp *app, unsigned width, unsigned height);
unsigned host_pixel_bytes(const HostApp *app);
uint32_t host_pixel_rgb(const HostApp *app, size_t index);
unsigned host_text_scale(const HostApp *app);
unsigned host_reader_pages(const HostApp *app);
int host_render(HostApp *app);
void host_tick(HostApp *app);
double host_now(void);
int host_ppm(const HostApp *app, const char *path);
int host_pbm(const HostApp *app, const char *path);
uint32_t host_frame_hash(const HostApp *app);
void host_metrics(const HostApp *app, FILE *out);
void host_state(const HostApp *app, FILE *out);
void host_instance_path(const HostApp *app, uint16_t instance, char *out, size_t capacity);
unsigned host_buttons(const HostApp *app, uint16_t *buttons, unsigned capacity);
void host_caption(const HostApp *app, uint16_t instance, char *out, size_t capacity);
void host_select(HostApp *app, int direction);
void host_activate(HostApp *app);
#endif
