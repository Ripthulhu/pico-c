#ifndef PICO_AUDIO_H
#define PICO_AUDIO_H

#include "pico.h"

#ifndef PICO_AUDIO_MAX_VOICES
#define PICO_AUDIO_MAX_VOICES 8
#endif
#if PICO_AUDIO_MAX_VOICES < 1 || PICO_AUDIO_MAX_VOICES > 32
#error PICO_AUDIO_MAX_VOICES must be between 1 and 32
#endif

/* All state is caller-owned. Bank bytes and cue envelopes must remain valid
 * until their voices stop. Render and cue delivery must be serialised by the
 * adapter; no locks, allocation, output queue or device calls live here. */
typedef struct PicoAudioVoice {
    const uint8_t *payload;
    const PicoSoundEnvelope *envelope;
    uint32_t frame_count, frame, phase, sample_rate, decoded_frame;
    int32_t predictor[2];
    uint16_t symbol, plays_left, envelope_count, envelope_index;
    uint8_t active, codec, channels, step_index[2];
} PicoAudioVoice;

typedef struct PicoAudio {
    const uint8_t *bank;
    size_t bank_size;
    PicoAudioVoice voices[PICO_AUDIO_MAX_VOICES];
    uint64_t rendered_frames;
    uint32_t sample_rate, dropped_events, missing_sounds, invalid_events, clipped_samples;
    uint16_t sound_count, active_voices, peak_voices;
    uint8_t channels;
} PicoAudio;

/* Accepts unaligned PCTS v1 data. Returns zero and clears state on invalid data.
 * Output rates are 11025, 22050 or 44100 Hz, with one or two channels. */
int pico_audio_init(PicoAudio *audio, const void *bank, size_t bytes, unsigned sample_rate,
                    unsigned channels);
/* loops is the total play count, with zero treated as one. A full voice pool
 * drops the new cue, preserving music already playing. stop stops every voice
 * with that symbol. no_multiple suppresses a cue while its symbol is active. */
void pico_audio_event(PicoAudio *audio, const PicoSoundEvent *event);
void pico_audio_stop_all(PicoAudio *audio);
/* Signed PCM16, interleaved for stereo. Rates use nearest-neighbour resampling;
 * downsampling has no low-pass filter. Gains interpolate at 44100 Hz positions.
 * Mono source envelopes average left/right gain as specified by SWF. Pause by
 * withholding calls: rendering advances playback even when the result is zero. */
void pico_audio_render(PicoAudio *audio, int16_t *output, size_t frames);

#endif
