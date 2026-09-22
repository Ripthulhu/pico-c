#ifndef PICO_HOST_AUDIO_H
#define PICO_HOST_AUDIO_H
#include "host_common.h"
#include "pico_audio.h"

#ifdef PICO_STREAM_RENDER
#define PICO_DEFAULT_SOUNDS "assets/pico_sound_micro.pcts"
#define PICO_DEFAULT_AUDIO_RATE 11025
#define PICO_DEFAULT_AUDIO_CHANNELS 1
#else
#define PICO_DEFAULT_SOUNDS "assets/pico_sound.pcts"
#define PICO_DEFAULT_AUDIO_RATE 44100
#define PICO_DEFAULT_AUDIO_CHANNELS 2
#endif
#define HOST_AUDIO_CHUNK 256

typedef struct HostAudio {
    PicoAudio mixer;
    const void *bytes;
    size_t size;
    FILE *wav;
    uint64_t wav_frames;
    uint64_t source_identity[2];
    unsigned tick_fraction;
    int failed;
} HostAudio;

int host_audio_open(HostAudio *audio, const char *path, unsigned rate, unsigned channels);
int host_audio_close(HostAudio *audio);
HostSoundSink host_audio_sink(HostAudio *audio);
int host_audio_wav_open(HostAudio *audio, const char *path, const char *art_path);
int host_audio_wav_tick(HostAudio *audio);
void host_audio_report(const HostAudio *audio, FILE *out);
#endif
