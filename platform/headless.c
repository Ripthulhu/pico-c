#include "host_common.h"
#ifdef PICO_HAS_AUDIO
#include "host_audio.h"
static HostAudio audio;
#endif
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static HostApp app;

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --assets FILE       Asset ROM pack (default " PICO_DEFAULT_ASSETS ")\n"
            "  --ticks N           Fixed 24 Hz ticks to execute (default 120)\n"
            "  --root N            Start at root timeline frame (default 1: original menu)\n"
            "  --width N --height N  Output dimensions (defaults %ux%u, limit %ux%u)\n"
            "  --mono              Ordered-dither black-and-white output\n"
            "  --assist|--no-assist  Action caption (default on at width <=128)\n"
            "  --read-text --page N  Show readable dialogue overlay after simulation\n"
            "  --render-every N    Render every N ticks (0: only final frame)\n"
            "  --trace FILE        Deterministic input/control event script\n"
            "  --ppm FILE          Write final RGB PPM image\n"
            "  --pbm FILE          Write final 1-bit PBM image (enables --mono)\n"
            "  --metrics FILE      Write metrics JSON (also printed to stdout)\n"
            "  --state FILE        Write live timeline instances as JSON\n"
#ifdef PICO_HAS_AUDIO
            "  --wav FILE          Write game audio as a deterministic PCM WAV\n"
            "  --sounds FILE       Sound ROM pack (default " PICO_DEFAULT_SOUNDS ")\n"
#endif
            "  --assert-root N     Fail if final root frame differs\n"
            "  --fail-on-missing   Fail on unresolved authored script targets/labels\n"
            "Trace lines: TICK move|down|up X Y; TICK tap SYMBOL;\n"
            "             TICK goto PATH FRAME PLAY; TICK play|stop|next PATH\n"
            "Coordinates are always original 550x350 stage pixels.\n",
            program, (unsigned)PICO_DEFAULT_WIDTH, (unsigned)PICO_DEFAULT_HEIGHT,
            (unsigned)PICO_RENDER_MAX_WIDTH, (unsigned)PICO_HOST_MAX_HEIGHT);
}

static int number(const char *text, unsigned *out) {
    char *end;
    unsigned long value;
    if (!text[0] || text[0] == '-')
        return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value > UINT_MAX)
        return 0;
    *out = (unsigned)value;
    return 1;
}

typedef struct Trace {
    FILE *file;
    char line[1024];
    unsigned tick, previous, line_number;
    int ready, error;
} Trace;

static void trace_next(Trace *trace) {
    trace->ready = 0;
    if (!trace->file)
        return;
    while (fgets(trace->line, sizeof(trace->line), trace->file)) {
        char *start = trace->line;
        char *end;
        unsigned long t;
        trace->line_number++;
        while (*start == ' ' || *start == '\t')
            start++;
        if (*start == '#' || *start == '\n' || *start == '\r' || !*start)
            continue;
        errno = 0;
        t = strtoul(start, &end, 10);
        if (errno || end == start || t > UINT_MAX || t < trace->previous) {
            fprintf(stderr, "Invalid or unsorted trace tick on line %u.\n", trace->line_number);
            trace->error = 1;
            return;
        }
        trace->previous = trace->tick = (unsigned)t;
        memmove(trace->line, end, strlen(end) + 1);
        trace->ready = 1;
        return;
    }
    if (ferror(trace->file))
        trace->error = 1;
}

static int trace_apply(Trace *trace, int *pointer_down) {
    char op[32], path[512];
    int x, y, frame, play;
    unsigned symbol, i;
    if (sscanf(trace->line, "%31s", op) != 1)
        return 0;
    if (!strcmp(op, "move") || !strcmp(op, "down") || !strcmp(op, "up")) {
        if (sscanf(trace->line, "%31s %d %d", op, &x, &y) != 3 || x < -32768 || x > 32767 ||
            y < -32768 || y > 32767)
            return 0;
        if (!strcmp(op, "down"))
            *pointer_down = 1;
        if (!strcmp(op, "up"))
            *pointer_down = 0;
        pico_pointer(&app.game, x * 65536, y * 65536, *pointer_down);
        return 1;
    }
    if (!strcmp(op, "tap")) {
        if (sscanf(trace->line, "%31s %u", op, &symbol) != 2)
            return 0;
        for (i = 0; i < PICO_MAX_INSTANCES; i++) {
            PicoInstance *instance = &app.game.instances[i];
            if (instance->alive && instance->symbol == symbol &&
                app.game.data->symbols[symbol].kind == PICO_BUTTON) {
                uint32_t generation = instance->generation;
                pico_button(&app.game, (uint16_t)i, 1);
                if (instance->alive && instance->generation == generation)
                    pico_button(&app.game, (uint16_t)i, 0);
                return 1;
            }
        }
        fprintf(stderr, "No visible button symbol %u at tick %u.\n", symbol, trace->tick);
        return 0;
    }
    if (!strcmp(op, "goto")) {
        if (sscanf(trace->line, "%31s %511s %d %d", op, path, &frame, &play) != 4 || frame < 1 ||
            frame > 65535)
            return 0;
        pico_goto(&app.game, path, (uint16_t)frame, play);
        return 1;
    }
    if (sscanf(trace->line, "%31s %511s", op, path) != 2)
        return 0;
    if (!strcmp(op, "play"))
        pico_play(&app.game, path);
    else if (!strcmp(op, "stop"))
        pico_stop(&app.game, path);
    else if (!strcmp(op, "next"))
        pico_next(&app.game, path);
    else
        return 0;
    return 1;
}

int main(int argc, char **argv) {
    const char *assets = PICO_DEFAULT_ASSETS, *ppm = NULL, *metrics = NULL, *state = NULL;
    const char *pbm = NULL;
    const char *trace_path = NULL;
#ifdef PICO_HAS_AUDIO
    const char *wav = NULL, *sounds = PICO_DEFAULT_SOUNDS;
#endif
    HostSoundSink sound_sink = {0};
    unsigned ticks = 120, root = 1, width = PICO_DEFAULT_WIDTH, height = PICO_DEFAULT_HEIGHT,
             render_every = 1;
    unsigned expected_root = 0, tick;
    int mono = 0, fail_missing = 0, pointer_down = 0, result = 0, arg, assist = -1, reading = 0;
    unsigned reader_page = 0;
    Trace trace;
    memset(&trace, 0, sizeof(trace));
    for (arg = 1; arg < argc; arg++) {
        const char *option = argv[arg];
        unsigned *numeric = NULL;
        if (!strcmp(option, "--help") || !strcmp(option, "-h")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(option, "--mono")) {
            mono = 1;
            continue;
        }
        if (!strcmp(option, "--assist")) {
            assist = 1;
            continue;
        }
        if (!strcmp(option, "--no-assist")) {
            assist = 0;
            continue;
        }
        if (!strcmp(option, "--read-text")) {
            reading = 1;
            continue;
        }
        if (!strcmp(option, "--fail-on-missing")) {
            fail_missing = 1;
            continue;
        }
        if (!strcmp(option, "--ticks"))
            numeric = &ticks;
        else if (!strcmp(option, "--root"))
            numeric = &root;
        else if (!strcmp(option, "--width"))
            numeric = &width;
        else if (!strcmp(option, "--height"))
            numeric = &height;
        else if (!strcmp(option, "--render-every"))
            numeric = &render_every;
        else if (!strcmp(option, "--assert-root"))
            numeric = &expected_root;
        else if (!strcmp(option, "--page"))
            numeric = &reader_page;
        if (++arg == argc) {
            fprintf(stderr, "Missing value for %s.\n", option);
            return 2;
        }
        if (numeric) {
            if (!number(argv[arg], numeric)) {
                fprintf(stderr, "Invalid number for %s.\n", option);
                return 2;
            }
        } else if (!strcmp(option, "--assets"))
            assets = argv[arg];
        else if (!strcmp(option, "--ppm"))
            ppm = argv[arg];
        else if (!strcmp(option, "--pbm")) {
            pbm = argv[arg];
            mono = 1;
        } else if (!strcmp(option, "--metrics"))
            metrics = argv[arg];
        else if (!strcmp(option, "--state"))
            state = argv[arg];
        else if (!strcmp(option, "--trace"))
            trace_path = argv[arg];
#ifdef PICO_HAS_AUDIO
        else if (!strcmp(option, "--wav"))
            wav = argv[arg];
        else if (!strcmp(option, "--sounds"))
            sounds = argv[arg];
#endif
        else {
            fprintf(stderr, "Unknown option %s.\n", option);
            usage(argv[0]);
            return 2;
        }
    }
    if (!root || root > 65535) {
        fprintf(stderr, "Invalid start frame.\n");
        return 2;
    }
    if (trace_path) {
        trace.file = fopen(trace_path, "r");
        if (!trace.file) {
            fprintf(stderr, "Cannot open trace %s.\n", trace_path);
            return 2;
        }
        trace_next(&trace);
    }
#ifdef PICO_HAS_AUDIO
    if (wav) {
        if (!host_audio_open(&audio, sounds, PICO_DEFAULT_AUDIO_RATE,
                             PICO_DEFAULT_AUDIO_CHANNELS) ||
            !host_audio_wav_open(&audio, wav, assets)) {
            fprintf(stderr, "Cannot open sound pack or WAV output.\n");
            result = 2;
            goto done;
        }
        sound_sink = host_audio_sink(&audio);
    }
#endif
    if (!host_open_with_sound(&app, assets, width, height, root, mono, &sound_sink)) {
        result = 2;
        goto done;
    }
    if (assist >= 0) {
        app.assist = assist && height >= 16;
        app.assist_auto = 0;
    }
    for (tick = 0; tick <= ticks; tick++) {
        while (trace.ready && trace.tick == tick) {
            if (!trace_apply(&trace, &pointer_down)) {
                fprintf(stderr, "Invalid trace event on line %u: %s", trace.line_number,
                        trace.line);
                result = 2;
                goto close;
            }
            trace_next(&trace);
        }
        if (trace.error) {
            result = 2;
            goto close;
        }
        if (tick == ticks)
            break;
#ifdef PICO_HAS_AUDIO
        /* Cues at tick N start at N/24 seconds. Advance audio before tick N+1. */
        if (wav && !host_audio_wav_tick(&audio)) {
            fprintf(stderr, "Cannot write WAV output.\n");
            result = 2;
            goto close;
        }
#endif
        host_tick(&app);
        if (render_every && (tick + 1) % render_every == 0 && !host_render(&app)) {
            fprintf(stderr, "Rendering failed.\n");
            result = 2;
            goto close;
        }
    }
    if (trace.ready) {
        fprintf(stderr, "Trace extends beyond --ticks.\n");
        result = 2;
        goto close;
    }
    if (!host_render(&app)) {
        result = 2;
        goto close;
    }
    if (reading) {
        app.reading = 1;
        app.reader_page = reader_page;
        if (!host_render(&app)) {
            result = 2;
            goto close;
        }
    }
    if (ppm && !host_ppm(&app, ppm)) {
        fprintf(stderr, "Cannot write %s.\n", ppm);
        result = 2;
    }
    if (pbm && !host_pbm(&app, pbm)) {
        fprintf(stderr, "Cannot write %s.\n", pbm);
        result = 2;
    }
    host_metrics(&app, stdout);
    if (metrics) {
        FILE *f = fopen(metrics, "w");
        if (!f) {
            fprintf(stderr, "Cannot write %s.\n", metrics);
            result = 2;
        } else {
            host_metrics(&app, f);
            if (fclose(f))
                result = 2;
        }
    }
    if (state) {
        FILE *f = fopen(state, "w");
        if (!f) {
            fprintf(stderr, "Cannot write %s.\n", state);
            result = 2;
        } else {
            host_state(&app, f);
            if (fclose(f))
                result = 2;
        }
    }
    if (app.game.errors || app.draw_overflows ||
        (fail_missing && (app.game.missing_targets || app.game.missing_labels)))
        result = 1;
    if (expected_root && app.game.instances[0].frame != expected_root) {
        fprintf(stderr, "Expected root frame %u; got %u.\n", expected_root,
                app.game.instances[0].frame);
        result = 1;
    }
close:
    host_close(&app);
done:
#ifdef PICO_HAS_AUDIO
    if (audio.bytes)
        host_audio_report(&audio, stderr);
    if (!host_audio_close(&audio))
        result = 2;
#endif
    if (trace.file)
        fclose(trace.file);
    return result;
}
