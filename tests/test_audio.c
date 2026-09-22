#include "pico_audio.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct FixtureSound {
    uint16_t symbol;
    uint8_t codec, channels;
    uint32_t rate, frames, bytes;
    const uint8_t *payload;
} FixtureSound;

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static size_t make_bank(uint8_t *bank, const FixtureSound *sounds, unsigned count) {
    unsigned i;
    uint32_t offset = 16u + count * 32u;
    memset(bank, 0, offset);
    memcpy(bank, "PCTS", 4);
    put16(bank + 4, 1);
    put16(bank + 6, (uint16_t)count);
    put32(bank + 8, 16);
    for (i = 0; i < count; ++i) {
        uint8_t *record = bank + 16u + i * 32u;
        put16(record, sounds[i].symbol);
        record[2] = sounds[i].codec;
        record[3] = sounds[i].channels;
        put32(record + 4, sounds[i].rate);
        put32(record + 8, sounds[i].frames);
        put32(record + 12, offset);
        put32(record + 16, sounds[i].bytes);
        put32(record + 20, sounds[i].codec ? 256 : 0);
        memcpy(bank + offset, sounds[i].payload, sounds[i].bytes);
        offset += sounds[i].bytes;
    }
    put32(bank + 12, offset);
    return offset;
}

static PicoSoundEvent cue(uint16_t symbol, uint16_t loops) {
    PicoSoundEvent event;
    memset(&event, 0, sizeof(event));
    event.symbol = symbol;
    event.loops = loops;
    return event;
}

static void pcm_bytes(uint8_t *bytes, const int16_t *samples, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i)
        put16(bytes + i * 2, (uint16_t)samples[i]);
}

static void assert_samples(const int16_t *got, const int16_t *want, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (got[i] != want[i])
            fprintf(stderr, "audio sample %lu: got %d, expected %d\n", (unsigned long)i, got[i],
                    want[i]);
        assert(got[i] == want[i]);
    }
}

static void test_pcm(void) {
    const int16_t source[] = {1000, -1000, 32767, -32768};
    const int16_t expected[] = {1000, -1000, 32767, -32768, 0, 0};
    uint8_t data[8], storage[128];
    FixtureSound sound = {7, 0, 1, 44100, 4, sizeof(data), data};
    PicoSoundEvent event = cue(7, 0);
    PicoAudio audio;
    int16_t output[6];
    size_t bytes;
    pcm_bytes(data, source, 4);
    /* Both the directory and samples start at deliberately unaligned addresses. */
    bytes = make_bank(storage + 1, &sound, 1);
    assert(pico_audio_init(&audio, storage + 1, bytes, 44100, 1));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 6);
    assert_samples(output, expected, 6);
    assert(audio.rendered_frames == 6 && audio.peak_voices == 1 && audio.active_voices == 0);
    assert(audio.clipped_samples == 0);
    assert(pico_audio_init(&audio, storage + 1, bytes, 44100, 2));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 3);
    assert(output[0] == 1000 && output[1] == 1000 && output[4] == 32767 && output[5] == 32767);
}

static void test_rates_and_loops(void) {
    const int16_t source[] = {100, 200, 300, 400};
    const int16_t up[] = {100, 100, 100, 100, 200, 200, 200, 200, 300,
                          300, 300, 300, 400, 400, 400, 400, 0};
    const int16_t down[] = {100, 300, 100, 300, 100, 300, 0};
    const int16_t short_loop[] = {100, 100, 0};
    const unsigned rates[] = {11025, 22050, 44100};
    uint8_t data[8], bank[128];
    FixtureSound sound = {1, 0, 1, 11025, 4, sizeof(data), data};
    PicoSoundEvent event = cue(1, 1);
    PicoAudio audio;
    int16_t output[17];
    size_t bytes;
    unsigned s, d;
    pcm_bytes(data, source, 4);
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&audio, bank, bytes, 44100, 1));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 17);
    assert_samples(output, up, 17);
    sound.rate = 22050;
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&audio, bank, bytes, 11025, 1));
    event.loops = 3;
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 7);
    assert_samples(output, down, 7);
    /* One output step can cross several plays of a short source. */
    sound.rate = 44100;
    sound.frames = 1;
    sound.bytes = 2;
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&audio, bank, bytes, 11025, 1));
    event.loops = 5;
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 3);
    assert_samples(output, short_loop, 3);
    assert(audio.active_voices == 0);
    sound.frames = 4;
    sound.bytes = 8;
    for (s = 0; s < 3; ++s) {
        sound.rate = rates[s];
        bytes = make_bank(bank, &sound, 1);
        for (d = 0; d < 3; ++d) {
            size_t i, duration = 4u * rates[d] / rates[s];
            event.loops = 1;
            assert(pico_audio_init(&audio, bank, bytes, rates[d], 1));
            pico_audio_event(&audio, &event);
            pico_audio_render(&audio, output, duration + 1);
            for (i = 0; i < duration; ++i)
                assert(output[i] == source[i * rates[s] / rates[d]]);
            assert(output[duration] == 0 && audio.active_voices == 0);
        }
    }
}

static void test_rate_phase(void) {
    const int16_t source[] = {123, -456, 789};
    const unsigned rates[] = {11025, 22050, 44100};
    uint8_t data[6], bank[128];
    FixtureSound sound = {1, 0, 1, 11025, 3, sizeof(data), data};
    PicoSoundEvent event = cue(1, 5);
    PicoAudio audio;
    unsigned s, d, frame;
    int16_t output[3];
    pcm_bytes(data, source, 3);
    for (s = 0; s < 3; ++s) {
        size_t bytes;
        sound.rate = rates[s];
        bytes = make_bank(bank, &sound, 1);
        for (d = 0; d < 3; ++d) {
            unsigned duration = (15u * rates[d] + rates[s] - 1u) / rates[s];
            assert(pico_audio_init(&audio, bank, bytes, rates[d], 1));
            pico_audio_event(&audio, &event);
            /* Absolute elapsed samples give an independent clock. An odd source
             * length also makes a downsample step cross a loop boundary. */
            for (frame = 0; frame < duration; ++frame) {
                unsigned elapsed = (frame + 1u) * rates[s];
                unsigned position = elapsed / rates[d];
                pico_audio_render(&audio, output, 1);
                assert(output[0] == source[(frame * rates[s] / rates[d]) % 3u]);
                assert(audio.voices[0].phase == elapsed % rates[d]);
                if (position < 15) {
                    assert(audio.active_voices == 1);
                    assert(audio.voices[0].frame == position % 3u);
                    assert(audio.voices[0].plays_left == 5u - position / 3u);
                } else {
                    assert(audio.active_voices == 0 && audio.voices[0].plays_left == 0);
                }
            }
            output[0] = output[1] = output[2] = 123;
            pico_audio_render(&audio, output, 3);
            assert(output[0] == 0 && output[1] == 0 && output[2] == 0);
            assert(audio.rendered_frames == duration + 3u && audio.clipped_samples == 0);
        }
    }
}

static void test_cues_and_pool(void) {
    const int16_t sample[] = {2000, 2000};
    uint8_t data[4], bank[128];
    FixtureSound sounds[2] = {{3, 0, 1, 11025, 2, 4, data}, {9, 0, 1, 11025, 2, 4, data}};
    PicoSoundEvent event = cue(3, 2);
    PicoAudio audio;
    int16_t output[8];
    size_t bytes;
    unsigned i;
    pcm_bytes(data, sample, 2);
    bytes = make_bank(bank, sounds, 2);
    assert(pico_audio_init(&audio, bank, bytes, 11025, 1));
    pico_audio_event(&audio, &event);
    event.no_multiple = 1;
    pico_audio_event(&audio, &event);
    assert(audio.active_voices == 1 && audio.dropped_events == 0);
    event.no_multiple = 0;
    for (i = 1; i < PICO_AUDIO_MAX_VOICES; ++i)
        pico_audio_event(&audio, &event);
    event.symbol = 9;
    pico_audio_event(&audio, &event);
    assert(audio.dropped_events == 1 && audio.active_voices == PICO_AUDIO_MAX_VOICES);
    for (i = 0; i < PICO_AUDIO_MAX_VOICES; ++i)
        assert(audio.voices[i].symbol == 3 && audio.voices[i].frame == 0);
    event.symbol = 3;
    event.stop = 1;
    pico_audio_event(&audio, &event);
    assert(audio.active_voices == 0);
    event.symbol = 9;
    event.stop = 0;
    pico_audio_event(&audio, &event);
    event.symbol = 3;
    event.stop = 1;
    pico_audio_event(&audio, &event);
    assert(audio.active_voices == 1);
    pico_audio_render(&audio, output, 1);
    /* No render call while paused means that every voice retains its position. */
    assert(audio.voices[0].frame == 1 && audio.rendered_frames == 1);
    pico_audio_render(&audio, output, 3);
    assert(output[0] == 2000 && output[1] == 2000 && output[2] == 2000);
    assert(audio.active_voices == 0);
    event.stop = 0;
    event.symbol = 99;
    pico_audio_event(&audio, &event);
    assert(audio.missing_sounds == 1);
    event.symbol = 3;
    pico_audio_event(&audio, &event);
    pico_audio_stop_all(&audio);
    pico_audio_render(&audio, output, 8);
    for (i = 0; i < 8; ++i)
        assert(output[i] == 0);
    assert(audio.active_voices == 0 && audio.peak_voices == PICO_AUDIO_MAX_VOICES);
}

static void test_envelopes(void) {
    const int16_t stereo[] = {12000, -8000, 12000, -8000, 12000, -8000, 12000, -8000,
                              12000, -8000, 12000, -8000, 12000, -8000, 12000, -8000};
    const int16_t expected[] = {0,     -8000, 3000,  -6000, 6000,  -4000, 9000,  -2000,
                                12000, 0,     12000, 0,     12000, 0,     12000, 0};
    const int16_t downmixed[] = {-4000, -1500, 1000, 3500, 6000, 6000, 6000, 6000};
    const int16_t mono[] = {12000, 12000};
    const int16_t mono_expected[] = {6000, 6000, 6000, 6000, 6000, 6000, 6000, 6000,
                                     6000, 6000, 6000, 6000, 6000, 6000, 6000, 6000};
    const int16_t ramp_expected[] = {0, 3000, 6000, 9000, 12000, 12000, 12000, 12000};
    PicoSoundEnvelope envelope[] = {{0, 0, 32768}, {4, 32768, 0}};
    PicoSoundEnvelope ramp[] = {{0, 0, 0}, {4, 32768, 32768}};
    PicoSoundEnvelope step[] = {{0, 0, 0}, {0, 16384, 16384}, {4, 32768, 32768}};
    uint8_t data[32], bank[128];
    FixtureSound sound = {2, 0, 2, 44100, 8, sizeof(data), data};
    PicoSoundEvent event = cue(2, 2);
    PicoAudio audio;
    int16_t output[32];
    size_t bytes;
    pcm_bytes(data, stereo, 16);
    bytes = make_bank(bank, &sound, 1);
    event.envelope = envelope;
    event.envelope_count = 2;
    assert(pico_audio_init(&audio, bank, bytes, 44100, 2));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 16);
    assert_samples(output, expected, 16);
    assert_samples(output + 16, expected, 16);
    assert(pico_audio_init(&audio, bank, bytes, 44100, 1));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 8);
    assert_samples(output, downmixed, 8);
    sound.channels = 1;
    sound.frames = 2;
    sound.rate = 11025;
    sound.bytes = 4;
    pcm_bytes(data, mono, 2);
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&audio, bank, bytes, 44100, 2));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 8);
    assert_samples(output, mono_expected, 16);
    assert(pico_audio_init(&audio, bank, bytes, 44100, 1));
    event.envelope = ramp;
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 8);
    assert_samples(output, ramp_expected, 8);
    assert(pico_audio_init(&audio, bank, bytes, 44100, 1));
    event.envelope = step;
    event.envelope_count = 3;
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 2);
    assert(output[0] == 6000 && output[1] == 7500);
    pico_audio_stop_all(&audio);
    event.envelope = NULL;
    pico_audio_event(&audio, &event);
    assert(audio.invalid_events == 1 && audio.active_voices == 0);
    step[1].left = 32769;
    event.envelope = step;
    pico_audio_event(&audio, &event);
    assert(audio.invalid_events == 2);
    step[1].left = 16384;
    step[0].position = 5;
    pico_audio_event(&audio, &event);
    assert(audio.invalid_events == 3);
}

static void test_clipping(void) {
    const int16_t samples[] = {32767, -32768, 2000, -2000};
    uint8_t data[8], bank[128];
    FixtureSound sound = {1, 0, 2, 44100, 2, sizeof(data), data};
    PicoSoundEvent event = cue(1, 1);
    PicoAudio audio;
    int16_t output[4];
    size_t bytes;
    pcm_bytes(data, samples, 4);
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&audio, bank, bytes, 44100, 2));
    pico_audio_event(&audio, &event);
#if PICO_AUDIO_MAX_VOICES > 1
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 2);
    assert(output[0] == 32767 && output[1] == -32768 && output[2] == 4000 && output[3] == -4000);
    assert(audio.clipped_samples == 2);
#else
    pico_audio_render(&audio, output, 2);
    assert_samples(output, samples, 4);
    assert(audio.clipped_samples == 0);
#endif
    assert(pico_audio_init(&audio, bank, bytes, 44100, 2));
    {
        unsigned i;
        for (i = 0; i < PICO_AUDIO_MAX_VOICES; ++i)
            pico_audio_event(&audio, &event);
    }
    pico_audio_render(&audio, output, 2);
    assert(output[0] == 32767 && output[1] == -32768);
    assert(audio.active_voices == 0);
}

static void test_ima(void) {
    /* Independent golden vector: predictor 0/index0, nibbles 0,1,2,3,4,5,6,7,
     * 8,9,A,B,C,D,E,F. These exercise changing indices and both signs. */
    const int16_t expected[] = {0,  0,  1,  4,  8,   15,  27,   47,  88,
                                82, 66, 41, 10, -28, -84, -181, -380};
    uint8_t data[528], bank[640];
    FixtureSound sound = {42, 1, 1, 22050, 17, 132, data};
    PicoSoundEvent event = cue(42, 2);
    PicoAudio audio;
    int16_t output[520];
    size_t bytes;
    unsigned i;
    memset(data, 0, sizeof(data));
    for (i = 0; i < 8; ++i)
        data[4 + i] = (uint8_t)((i * 2u) | ((i * 2u + 1u) << 4));
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&audio, bank, bytes, 22050, 1));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 35);
    assert_samples(output, expected, 17);
    assert_samples(output + 17, expected, 17);
    assert(output[34] == 0 && audio.active_voices == 0);
    assert(pico_audio_init(&audio, bank, bytes, 11025, 1));
    event.loops = 1;
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 10);
    for (i = 0; i < 9; ++i)
        assert(output[i] == expected[i * 2]);
    assert(output[9] == 0);
    /* Distinct predictors prove planar stereo ordering and per-block reset. */
    memset(data, 0, sizeof(data));
    put16(data, 1000);
    put16(data + 132, (uint16_t)-1000);
    put16(data + 264, 3000);
    put16(data + 396, (uint16_t)-3000);
    sound.channels = 2;
    sound.frames = 258;
    sound.bytes = sizeof(data);
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&audio, bank, bytes, 22050, 2));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 260);
    for (i = 0; i < 256; ++i)
        assert(output[i * 2] == 1000 && output[i * 2 + 1] == -1000);
    assert(output[512] == 3000 && output[513] == -3000);
    assert(output[514] == 3000 && output[515] == -3000);
    assert(output[516] == 0 && output[517] == 0);
    /* Predictor saturation at the highest legal step index. */
    memset(data, 0x77, 132);
    put16(data, 32000);
    data[2] = 88;
    data[3] = 0;
    data[4] = 0xf7;
    sound.channels = 1;
    sound.frames = 3;
    sound.bytes = 132;
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&audio, bank, bytes, 22050, 1));
    pico_audio_event(&audio, &event);
    pico_audio_render(&audio, output, 3);
    assert(output[0] == 32000 && output[1] == 32767 && output[2] == -28669);
}

static void test_render_chunks(void) {
    uint8_t data[264], bank[384];
    FixtureSound sound = {5, 1, 1, 22050, 300, sizeof(data), data};
    PicoSoundEnvelope envelope[] = {{0, 0, 0}, {300, 32768, 32768}, {600, 0, 0}};
    PicoSoundEvent event = cue(5, 2);
    PicoAudio whole, chunks;
    int16_t expected[2404], output[2404];
    size_t bytes, position = 0;
    unsigned i;
    memset(data, 0, sizeof(data));
    for (i = 4; i < 132; ++i)
        data[i] = (uint8_t)(i * 19u);
    put16(data, 1234);
    data[2] = 42;
    put16(data + 132, (uint16_t)-3000);
    bytes = make_bank(bank, &sound, 1);
    assert(pico_audio_init(&whole, bank, bytes, 44100, 2));
    assert(pico_audio_init(&chunks, bank, bytes, 44100, 2));
    event.envelope = envelope;
    event.envelope_count = 3;
    pico_audio_event(&whole, &event);
    pico_audio_event(&chunks, &event);
    pico_audio_render(&whole, expected, 1202);
    while (position < 1202) {
        size_t count = position % 37u + 1;
        if (count > 1202 - position)
            count = 1202 - position;
        pico_audio_render(&chunks, output + position * 2, count);
        position += count;
    }
    assert_samples(output, expected, 2404);
    assert(whole.rendered_frames == chunks.rendered_frames);
    assert(whole.active_voices == 0 && chunks.active_voices == 0);
}

static void test_bad_banks(void) {
    const uint8_t payload[264] = {0};
    uint8_t good[384], bad[384];
    FixtureSound sounds[2] = {{2, 1, 1, 11025, 257, 264, payload}, {4, 0, 1, 11025, 1, 2, payload}};
    PicoAudio audio;
    size_t bytes = make_bank(good, sounds, 2), i;
    const struct {
        unsigned offset;
        uint32_t value;
        unsigned width;
    } corruptions[] = {{0, 'X', 1},
                       {4, 2, 2},
                       {6, 65535, 2},
                       {8, UINT32_MAX, 4},
                       {12, UINT32_MAX, 4},
                       {18, 2, 1},
                       {19, 0, 1},
                       {20, 48000, 4},
                       {24, 0, 4},
                       {24, UINT32_MAX, 4},
                       {28, 0, 4},
                       {28, UINT32_MAX, 4},
                       {32, UINT32_MAX, 4},
                       {32, 131, 4},
                       {36, 128, 4},
                       {40, 1, 4},
                       {44, 1, 4},
                       {48, 2, 2},
                       {48, 1, 2},
                       {68, 256, 4},
                       {80 + 2, 89, 1},
                       {80 + 3, 1, 1},
                       {80 + 132 + 2, 255, 1}};
    assert(pico_audio_init(&audio, good, bytes, 11025, 1));
    for (i = 0; i < bytes; ++i) {
        assert(!pico_audio_init(&audio, good, i, 11025, 1));
        memcpy(bad, good, bytes);
        put32(bad + 12, (uint32_t)i);
        assert(!pico_audio_init(&audio, bad, i, 11025, 1));
    }
    for (i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); ++i) {
        memcpy(bad, good, bytes);
        if (corruptions[i].width == 4)
            put32(bad + corruptions[i].offset, corruptions[i].value);
        else if (corruptions[i].width == 2)
            put16(bad + corruptions[i].offset, (uint16_t)corruptions[i].value);
        else
            bad[corruptions[i].offset] = (uint8_t)corruptions[i].value;
        assert(!pico_audio_init(&audio, bad, bytes, 11025, 1));
        assert(audio.bank == NULL && audio.sample_rate == 0 && audio.active_voices == 0);
    }
    assert(!pico_audio_init(&audio, good, bytes, 48000, 1));
    assert(!pico_audio_init(&audio, good, bytes, 11025, 0));
    assert(!pico_audio_init(&audio, good, bytes, 11025, 3));
    assert(!pico_audio_init(&audio, NULL, bytes, 11025, 1));
    assert(!pico_audio_init(NULL, good, bytes, 11025, 1));
    /* Shared read-only payloads let separately named sounds deduplicate. */
    sounds[0].codec = 0;
    sounds[0].frames = 1;
    sounds[0].bytes = 2;
    bytes = make_bank(good, sounds, 2);
    put32(good + 48 + 12, 80);
    assert(pico_audio_init(&audio, good, bytes, 11025, 1));
}

int main(void) {
    test_pcm();
    test_rates_and_loops();
    test_rate_phase();
    test_cues_and_pool();
    test_envelopes();
    test_clipping();
    test_ima();
    test_render_chunks();
    test_bad_banks();
    printf("audio: PCM, ADPCM, mixing, envelopes, loops and malformed banks passed (%lu bytes, %d "
           "voices)\n",
           (unsigned long)sizeof(PicoAudio), PICO_AUDIO_MAX_VOICES);
    return 0;
}
