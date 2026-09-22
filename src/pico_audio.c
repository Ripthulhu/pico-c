#include "pico_audio.h"

#include <string.h>

static const int16_t ima_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t ima_indices[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t signed16(const uint8_t *p) {
    uint16_t value = read16(p);
    return value < 32768 ? (int32_t)value : (int32_t)value - 65536;
}

static int valid_rate(uint32_t rate) {
    return rate == 11025 || rate == 22050 || rate == 44100;
}

static int validate_bank(const uint8_t *bank, size_t bytes) {
    uint32_t directory_end, i;
    uint16_t count, previous = 0;
    if (!bank || bytes < 16 || bytes > UINT32_MAX || memcmp(bank, "PCTS", 4) ||
        read16(bank + 4) != 1 || read32(bank + 8) != 16 || read32(bank + 12) != bytes)
        return 0;
    count = read16(bank + 6);
    directory_end = 16u + (uint32_t)count * 32u;
    if (directory_end > bytes)
        return 0;
    for (i = 0; i < count; ++i) {
        const uint8_t *record = bank + 16u + i * 32u;
        uint16_t symbol = read16(record);
        uint8_t codec = record[2], channels = record[3];
        uint32_t frames = read32(record + 8), offset = read32(record + 12);
        uint32_t length = read32(record + 16), block_frames = read32(record + 20);
        uint64_t required;
        if ((i && symbol <= previous) || codec > 1 || (channels != 1 && channels != 2) ||
            !valid_rate(read32(record + 4)) || !frames || read32(record + 24) ||
            read32(record + 28) || offset < directory_end || offset > bytes ||
            length > bytes - offset)
            return 0;
        previous = symbol;
        if (codec == 0) {
            required = (uint64_t)frames * channels * 2u;
            if (block_frames)
                return 0;
        } else {
            uint32_t block, blocks = (uint32_t)(((uint64_t)frames + 255u) / 256u);
            required = (uint64_t)blocks * channels * 132u;
            if (block_frames != 256 || required != length)
                return 0;
            for (block = 0; block < blocks; ++block) {
                unsigned channel;
                for (channel = 0; channel < channels; ++channel) {
                    const uint8_t *header =
                        bank + offset + ((size_t)block * channels + channel) * 132u;
                    if (header[2] > 88 || header[3])
                        return 0;
                }
            }
        }
        if (required != length)
            return 0;
    }
    return 1;
}

int pico_audio_init(PicoAudio *audio, const void *bank, size_t bytes, unsigned sample_rate,
                    unsigned channels) {
    if (!audio)
        return 0;
    memset(audio, 0, sizeof(*audio));
    if (!valid_rate(sample_rate) || (channels != 1 && channels != 2) ||
        !validate_bank((const uint8_t *)bank, bytes))
        return 0;
    audio->bank = (const uint8_t *)bank;
    audio->bank_size = bytes;
    audio->sample_rate = sample_rate;
    audio->channels = (uint8_t)channels;
    audio->sound_count = read16(audio->bank + 6);
    return 1;
}

static const uint8_t *find_sound(const PicoAudio *audio, uint16_t symbol) {
    unsigned low = 0, high = audio->sound_count;
    while (low < high) {
        unsigned mid = low + (high - low) / 2;
        const uint8_t *record = audio->bank + 16u + mid * 32u;
        uint16_t found = read16(record);
        if (found == symbol)
            return record;
        if (found < symbol)
            low = mid + 1;
        else
            high = mid;
    }
    return NULL;
}

static void stop_voice(PicoAudio *audio, PicoAudioVoice *voice) {
    voice->active = 0;
    --audio->active_voices;
}

void pico_audio_stop_all(PicoAudio *audio) {
    unsigned i;
    if (!audio)
        return;
    for (i = 0; i < PICO_AUDIO_MAX_VOICES; ++i)
        audio->voices[i].active = 0;
    audio->active_voices = 0;
}

void pico_audio_event(PicoAudio *audio, const PicoSoundEvent *event) {
    unsigned i;
    PicoAudioVoice *free_voice = NULL;
    const uint8_t *record;
    if (!audio || !audio->bank || !event)
        return;
    for (i = 0; i < PICO_AUDIO_MAX_VOICES; ++i) {
        PicoAudioVoice *voice = &audio->voices[i];
        if (voice->active && voice->symbol == event->symbol) {
            if (event->stop)
                stop_voice(audio, voice);
            else if (event->no_multiple)
                return;
        }
        if (!voice->active && !free_voice)
            free_voice = voice;
    }
    if (event->stop)
        return;
    if (event->envelope_count && !event->envelope) {
        ++audio->invalid_events;
        return;
    }
    for (i = 0; i < event->envelope_count; ++i) {
        const PicoSoundEnvelope *point = event->envelope + i;
        if (point->left > 32768 || point->right > 32768 ||
            (i && point->position < event->envelope[i - 1].position)) {
            ++audio->invalid_events;
            return;
        }
    }
    record = find_sound(audio, event->symbol);
    if (!record) {
        ++audio->missing_sounds;
        return;
    }
    if (!free_voice) {
        ++audio->dropped_events;
        return;
    }
    memset(free_voice, 0, sizeof(*free_voice));
    free_voice->payload = audio->bank + read32(record + 12);
    free_voice->envelope = event->envelope;
    free_voice->envelope_count = event->envelope_count;
    free_voice->symbol = event->symbol;
    free_voice->plays_left = event->loops ? event->loops : 1;
    free_voice->codec = record[2];
    free_voice->channels = record[3];
    free_voice->sample_rate = read32(record + 4);
    free_voice->frame_count = read32(record + 8);
    free_voice->decoded_frame = UINT32_MAX;
    free_voice->active = 1;
    ++audio->active_voices;
    if (audio->active_voices > audio->peak_voices)
        audio->peak_voices = audio->active_voices;
}

static int32_t decode_nibble(PicoAudioVoice *voice, unsigned channel, unsigned code) {
    int step = ima_steps[voice->step_index[channel]];
    int delta = (step >> 3) + ((code & 1) ? step >> 2 : 0) + ((code & 2) ? step >> 1 : 0) +
                ((code & 4) ? step : 0);
    int index = voice->step_index[channel] + ima_indices[code & 7];
    int32_t sample = voice->predictor[channel] + ((code & 8) ? -delta : delta);
    if (sample < -32768)
        sample = -32768;
    if (sample > 32767)
        sample = 32767;
    if (index < 0)
        index = 0;
    if (index > 88)
        index = 88;
    voice->step_index[channel] = (uint8_t)index;
    voice->predictor[channel] = sample;
    return sample;
}

static void sample_voice(PicoAudioVoice *voice, int32_t samples[2]) {
    unsigned channel;
    if (!voice->codec) {
        const uint8_t *source = voice->payload + (size_t)voice->frame * voice->channels * 2u;
        for (channel = 0; channel < voice->channels; ++channel)
            samples[channel] = signed16(source + channel * 2u);
    } else {
        uint32_t block_frame = voice->frame & ~255u;
        const uint8_t *block =
            voice->payload + (size_t)(voice->frame / 256u) * voice->channels * 132u;
        if (voice->decoded_frame == UINT32_MAX || voice->decoded_frame > voice->frame ||
            (voice->decoded_frame & ~255u) != block_frame) {
            for (channel = 0; channel < voice->channels; ++channel) {
                voice->predictor[channel] = signed16(block + channel * 132u);
                voice->step_index[channel] = block[channel * 132u + 2u];
            }
            voice->decoded_frame = block_frame;
        }
        while (voice->decoded_frame < voice->frame) {
            unsigned code_index = voice->decoded_frame - block_frame;
            for (channel = 0; channel < voice->channels; ++channel) {
                unsigned code = block[channel * 132u + 4u + code_index / 2u];
                code = (code >> ((code_index & 1u) * 4u)) & 15u;
                decode_nibble(voice, channel, code);
            }
            ++voice->decoded_frame;
        }
        for (channel = 0; channel < voice->channels; ++channel)
            samples[channel] = voice->predictor[channel];
    }
    if (voice->channels == 1)
        samples[1] = samples[0];
}

static void envelope_gain(PicoAudioVoice *voice, uint32_t output_rate, int32_t gain[2]) {
    const PicoSoundEnvelope *first, *next;
    uint64_t position;
    gain[0] = gain[1] = 32768;
    if (!voice->envelope_count)
        return;
    first = voice->envelope + voice->envelope_index;
    gain[0] = first->left;
    gain[1] = first->right;
    if (voice->envelope_index + 1u == voice->envelope_count)
        return;
    position = (uint64_t)voice->frame * (44100u / voice->sample_rate) +
               voice->phase * (44100u / voice->sample_rate) / output_rate;
    while (voice->envelope_index + 1u < voice->envelope_count &&
           voice->envelope[voice->envelope_index + 1u].position <= position)
        ++voice->envelope_index;
    first = voice->envelope + voice->envelope_index;
    gain[0] = first->left;
    gain[1] = first->right;
    if (voice->envelope_index + 1u < voice->envelope_count && position > first->position) {
        next = first + 1;
        gain[0] +=
            (int32_t)((int64_t)((int32_t)next->left - first->left) *
                      (int64_t)(position - first->position) / (next->position - first->position));
        gain[1] +=
            (int32_t)((int64_t)((int32_t)next->right - first->right) *
                      (int64_t)(position - first->position) / (next->position - first->position));
    }
}

static void advance_voice(PicoAudio *audio, PicoAudioVoice *voice) {
    uint32_t advance;
    voice->phase += voice->sample_rate;
    advance = voice->phase / audio->sample_rate;
    voice->phase %= audio->sample_rate;
    while (advance) {
        uint32_t remaining = voice->frame_count - voice->frame;
        if (advance < remaining) {
            voice->frame += advance;
            return;
        }
        advance -= remaining;
        if (--voice->plays_left == 0) {
            stop_voice(audio, voice);
            return;
        }
        voice->frame = 0;
        voice->decoded_frame = UINT32_MAX;
        voice->envelope_index = 0;
    }
}

static int16_t clip_sample(PicoAudio *audio, int32_t value) {
    if (value < -32768) {
        ++audio->clipped_samples;
        return -32768;
    }
    if (value > 32767) {
        ++audio->clipped_samples;
        return 32767;
    }
    return (int16_t)value;
}

void pico_audio_render(PicoAudio *audio, int16_t *output, size_t frames) {
    size_t frame;
    if (!audio || !audio->bank || !output)
        return;
    if (!audio->active_voices) {
        memset(output, 0, frames * audio->channels * sizeof(*output));
        audio->rendered_frames += frames;
        return;
    }
    for (frame = 0; frame < frames; ++frame) {
        int32_t mixed[2] = {0, 0};
        unsigned i;
        for (i = 0; i < PICO_AUDIO_MAX_VOICES; ++i) {
            PicoAudioVoice *voice = &audio->voices[i];
            int32_t samples[2], gain[2];
            if (!voice->active)
                continue;
            sample_voice(voice, samples);
            if (voice->envelope_count) {
                envelope_gain(voice, audio->sample_rate, gain);
                if (voice->channels == 1)
                    gain[0] = gain[1] = (gain[0] + gain[1]) / 2;
                mixed[0] += samples[0] * gain[0] / 32768;
                mixed[1] += samples[1] * gain[1] / 32768;
            } else {
                mixed[0] += samples[0];
                mixed[1] += samples[1];
            }
            advance_voice(audio, voice);
        }
        if (audio->channels == 1) {
            *output++ = clip_sample(audio, (mixed[0] + mixed[1]) / 2);
        } else {
            *output++ = clip_sample(audio, mixed[0]);
            *output++ = clip_sample(audio, mixed[1]);
        }
    }
    audio->rendered_frames += frames;
}
