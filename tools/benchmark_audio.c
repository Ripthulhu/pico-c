#include "pico_audio.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Link the old mixer separately, renaming its four public functions to
 * baseline_audio_*. Keeping that source outside this tool avoids accidentally
 * checking a changed implementation against a copy of itself. */
int baseline_audio_init(PicoAudio *, const void *, size_t, unsigned, unsigned);
void baseline_audio_event(PicoAudio *, const PicoSoundEvent *);
void baseline_audio_stop_all(PicoAudio *);
void baseline_audio_render(PicoAudio *, int16_t *, size_t);

typedef void (*Render)(PicoAudio *, int16_t *, size_t);

static void put16(uint8_t *p, unsigned value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, value);
    put16(p + 2, value >> 16);
}

static size_t fixture(uint8_t *bank, unsigned codec, unsigned channels, unsigned rate,
                      unsigned frames) {
    unsigned bytes = codec ? ((frames + 255) / 256) * channels * 132 : frames * channels * 2;
    unsigned i;
    memset(bank, 0, 48);
    memcpy(bank, "PCTS", 4);
    put16(bank + 4, 1);
    put16(bank + 6, 1);
    put32(bank + 8, 16);
    put32(bank + 12, 48 + bytes);
    put16(bank + 16, 7);
    bank[18] = (uint8_t)codec;
    bank[19] = (uint8_t)channels;
    put32(bank + 20, rate);
    put32(bank + 24, frames);
    put32(bank + 28, 48);
    put32(bank + 32, bytes);
    put32(bank + 36, codec ? 256 : 0);
    for (i = 0; i < bytes; ++i)
        bank[48 + i] = (uint8_t)(i * 137u + (i >> 3) * 67u);
    if (codec) {
        for (i = 0; i < bytes; i += 132) {
            bank[48 + i + 2] = (uint8_t)((i / 132) * 19u % 89u);
            bank[48 + i + 3] = 0;
        }
    }
    return 48 + bytes;
}

static PicoSoundEvent cue(unsigned loops, const PicoSoundEnvelope *envelope, unsigned count) {
    PicoSoundEvent event;
    memset(&event, 0, sizeof(event));
    event.symbol = 7;
    event.loops = (uint16_t)loops;
    event.envelope = envelope;
    event.envelope_count = (uint16_t)count;
    return event;
}

static void same_state(const PicoAudio *before, const PicoAudio *after) {
    /* Both APIs zero their state, including padding; both use the same bank
     * and envelope addresses, so this also checks every counter and decoder. */
    if (memcmp(before, after, sizeof(*before))) {
        fputs("audio state differs from baseline\n", stderr);
        exit(1);
    }
}

static void event_pair(PicoAudio *before, PicoAudio *after, const PicoSoundEvent *event) {
    baseline_audio_event(before, event);
    pico_audio_event(after, event);
    same_state(before, after);
}

static void render_pair(PicoAudio *before, PicoAudio *after, size_t frames) {
    int16_t a[514], b[514];
    memset(a, 0x55, sizeof(a));
    memset(b, 0x55, sizeof(b));
    assert(frames <= 257);
    baseline_audio_render(before, a, frames);
    pico_audio_render(after, b, frames);
    if (memcmp(a, b, sizeof(a))) {
        fputs("audio output differs from baseline\n", stderr);
        exit(1);
    }
    same_state(before, after);
}

static unsigned verify(void) {
    static const unsigned rates[] = {11025, 22050, 44100};
    static const unsigned lengths[] = {1, 2, 3, 17, 255, 256, 257, 513};
    static const unsigned loops[] = {0, 2, 5};
    static const PicoSoundEnvelope envelope[] = {
        {0, 0, 32768}, {0, 10000, 25000}, {6, 32768, 0}, {1200, 17000, 22000}};
    uint8_t bank[4096];
    unsigned codec, source_channels, output_channels, source_rate, output_rate, length, loop,
        envelopes, cases = 0;
    for (codec = 0; codec < 2; ++codec)
        for (source_channels = 1; source_channels <= 2; ++source_channels)
            for (output_channels = 1; output_channels <= 2; ++output_channels)
                for (source_rate = 0; source_rate < 3; ++source_rate)
                    for (output_rate = 0; output_rate < 3; ++output_rate)
                        for (length = 0; length < 8; ++length)
                            for (loop = 0; loop < 3; ++loop)
                                for (envelopes = 0; envelopes < 2; ++envelopes) {
                                    PicoAudio before, after;
                                    PicoSoundEvent event =
                                        cue(loops[loop], envelopes ? envelope : NULL,
                                            envelopes ? 4 : 0);
                                    size_t bytes = fixture(bank, codec, source_channels,
                                                           rates[source_rate], lengths[length]);
                                    unsigned i = 0;
                                    assert(baseline_audio_init(
                                        &before, bank, bytes, rates[output_rate], output_channels));
                                    assert(pico_audio_init(&after, bank, bytes, rates[output_rate],
                                                           output_channels));
                                    render_pair(&before, &after, 257);
                                    event_pair(&before, &after, &event);
                                    do {
                                        render_pair(&before, &after, i++ % 97u + 1u);
                                    } while (before.active_voices);
                                    render_pair(&before, &after, 0);
                                    render_pair(&before, &after, 257);
                                    /* Exercise pool overflow, suppression, stop, and error
                                     * counters after voices have naturally finished. */
                                    for (i = 0; i <= PICO_AUDIO_MAX_VOICES; ++i)
                                        event_pair(&before, &after, &event);
                                    event.no_multiple = 1;
                                    event_pair(&before, &after, &event);
                                    render_pair(&before, &after, 3);
                                    event.stop = 1;
                                    event_pair(&before, &after, &event);
                                    render_pair(&before, &after, 257);
                                    event.stop = event.no_multiple = 0;
                                    event.symbol = 99;
                                    event_pair(&before, &after, &event);
                                    event.symbol = 7;
                                    event.envelope = NULL;
                                    event.envelope_count = 1;
                                    event_pair(&before, &after, &event);
                                    baseline_audio_stop_all(&before);
                                    pico_audio_stop_all(&after);
                                    same_state(&before, &after);
                                    ++cases;
                                }
    return cases;
}

static double time_render(Render render, PicoAudio initial, unsigned calls) {
    int16_t output[512];
    unsigned i;
    clock_t start = clock();
    for (i = 0; i < calls; ++i)
        render(&initial, output, 256);
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

static void benchmark(unsigned calls) {
    static const struct {
        const char *name;
        unsigned codec, channels, rate, output_rate, voices;
    } cases[] = {{"silence", 0, 2, 44100, 44100, 0},
                 {"pcm-unity", 0, 2, 44100, 44100, 1},
                 {"pcm-up4", 0, 1, 11025, 44100, 1},
                 {"pcm-down4", 0, 2, 44100, 11025, 1},
                 {"ima-unity", 1, 1, 11025, 11025, 1},
                 {"ima-up2", 1, 1, 11025, 22050, 1},
                 {"ima-unity-pool", 1, 1, 11025, 11025, PICO_AUDIO_MAX_VOICES},
                 {"ima-up4-pool", 1, 1, 11025, 44100, PICO_AUDIO_MAX_VOICES}};
    uint8_t bank[4096];
    unsigned i, voice;
    printf("benchmark: %u calls of 256 output frames, CPU time, no envelope\n", calls);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        PicoAudio initial;
        PicoSoundEvent event = cue(65535, NULL, 0);
        size_t bytes = fixture(bank, cases[i].codec, cases[i].channels, cases[i].rate, 513);
        double before, after;
        assert(pico_audio_init(&initial, bank, bytes, cases[i].output_rate, 2));
        for (voice = 0; voice < cases[i].voices; ++voice)
            pico_audio_event(&initial, &event);
        before = time_render(baseline_audio_render, initial, calls);
        after = time_render(pico_audio_render, initial, calls);
        printf("%s: before=%.6f after=%.6f seconds saving=%.1f%%\n", cases[i].name, before, after,
               before ? (before - after) / before * 100 : 0);
    }
}

int main(int argc, char **argv) {
    unsigned cases = verify();
    printf("audio equivalence: %u cases, exact samples and complete state (%d voices)\n", cases,
           PICO_AUDIO_MAX_VOICES);
    if (argc == 2) {
        unsigned calls = (unsigned)strtoul(argv[1], NULL, 10);
        /* Even the 4x downsample case must remain active for the whole run. */
        if (calls < 1 || calls > 30000)
            return 2;
        benchmark(calls);
    } else if (argc != 1)
        return 2;
    return 0;
}
