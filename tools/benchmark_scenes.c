#include "host_common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Diagnostic root entry avoids playing the route before a repeatable sample.
 * Timings include host composition, but no audio or display transfer. */
static HostApp app;
static int compare(const void *a, const void *b) {
    double aa = *(const double *)a, bb = *(const double *)b;
    return (aa > bb) - (aa < bb);
}
int main(int argc, char **argv) {
    unsigned root, width, height, frames, i, initial, profiling;
    size_t capacity;
    PicoAssetCache cache;
    void *arena = NULL;
    double times[240], total = 0, ticks = 0;
    char path[1024];
    if (argc != 10) {
        fprintf(stderr, "usage: benchmark-scenes PACK WIDTH HEIGHT ROOT INITIAL_TICKS FRAMES "
                        "CACHE_KIB PREFIX PROFILE_LEAVES\n");
        return 2;
    }
    width = (unsigned)atoi(argv[2]);
    height = (unsigned)atoi(argv[3]);
    root = (unsigned)atoi(argv[4]);
    initial = (unsigned)atoi(argv[5]);
    frames = (unsigned)atoi(argv[6]);
    capacity = (size_t)atoi(argv[7]) * 1024;
    profiling = (unsigned)atoi(argv[9]);
    if (frames < 1 || frames > 240 || !host_open(&app, argv[1], width, height, root, 0))
        return 2;
    pico_assets_bind_cache(&app.assets, NULL, NULL, 0);
#ifndef PICO_STREAM_RENDER
    free(app.asset_cache_memory);
    app.asset_cache_memory = NULL;
#endif
    if (capacity) {
        arena = malloc(capacity);
        if (!arena || !pico_assets_bind_cache(&app.assets, &cache, arena, capacity))
            return 2;
    }
    app.assist = 0;
    for (i = 0; i < initial; i++)
        host_tick(&app);
    if (!host_render(&app))
        return 2;
    snprintf(path, sizeof(path), "%s-first.ppm", argv[8]);
    if (!host_ppm(&app, path))
        return 2;
    printf("pack=%s size=%ux%u root=%u initial=%u cache_KiB=%zu first_hash=%08x first_draws=%u\n",
           argv[1], width, height, root, initial, capacity / 1024, host_frame_hash(&app),
           app.draw_count);
    for (i = 0; i < frames; i++) {
        double start = host_now();
        host_tick(&app);
        ticks += host_now() - start;
        start = host_now();
        if (!host_render(&app))
            return 2;
        times[i] = (host_now() - start) * 1000;
        total += times[i];
    }
    qsort(times, frames, sizeof(*times), compare);
    printf("frames=%u render_mean_ms=%.4f median_ms=%.4f p95_ms=%.4f max_ms=%.4f tick_mean_us=%.4f "
           "last_hash=%08x last_draws=%u peak_draws=%u cache_used=%zu cache_count=%u errors=%u\n",
           frames, total / frames, times[frames / 2], times[(frames - 1) * 95 / 100],
           times[frames - 1], ticks * 1e6 / frames, host_frame_hash(&app), app.draw_count,
           app.peak_draws, app.assets.cache ? cache.used : 0, app.assets.cache ? cache.count : 0,
           app.game.errors);
    snprintf(path, sizeof(path), "%s-last.ppm", argv[8]);
    if (!host_ppm(&app, path))
        return 2;
#ifndef PICO_STREAM_RENDER
    if (profiling && app.assets.version >= 2) {
        double start = host_now();
        if (!pico_raster_framebuffer32(&app.assets, NULL, 0, app.view_width, app.view_height, 0,
                                       app.pixels, app.width))
            return 2;
        printf("leaf_clear_ms=%.4f (included in each isolated leaf measurement)\n",
               (host_now() - start) * 1000);
        for (i = 0; i < app.draw_count; i++) {
            PicoDraw *d = app.draws + i;
            double start = host_now();
            if (!pico_raster_framebuffer32(&app.assets, d, 1, app.view_width, app.view_height, 0,
                                           app.pixels, app.width))
                return 2;
            printf("leaf=%u symbol=%u ratio=%u b=%d c=%d render_ms=%.4f cache_used=%zu "
                   "cache_count=%u\n",
                   i, d->symbol, d->ratio, d->b, d->c, (host_now() - start) * 1000,
                   app.assets.cache ? cache.used : 0, app.assets.cache ? cache.count : 0);
        }
    }
#else
    if (profiling)
        fputs("Per-leaf profiling is unavailable in the row-streaming build.\n", stderr);
#endif
    host_close(&app);
    free(arena);
    return 0;
}
