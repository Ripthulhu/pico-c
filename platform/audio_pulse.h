#ifndef PICO_AUDIO_PULSE_H
#define PICO_AUDIO_PULSE_H
#include "host_audio.h"

typedef struct HostAudioLive HostAudioLive;
HostAudioLive *host_audio_live_open(const char *path);
HostSoundSink host_audio_live_sink(HostAudioLive *live);
int host_audio_live_start(HostAudioLive *live);
void host_audio_live_pause(HostAudioLive *live, int paused);
void host_audio_live_close(HostAudioLive *live);
#endif
