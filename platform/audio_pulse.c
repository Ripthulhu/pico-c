#define _POSIX_C_SOURCE 200809L
#include "audio_pulse.h"
#include <pulse/pulseaudio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct HostAudioLive {
    HostAudio audio;
    pa_threaded_mainloop *loop;
    pa_context *context;
    pa_stream *stream;
    int loop_started, ready, started, paused, error;
};

static void live_event(void *user, const PicoSoundEvent *event) {
    HostAudioLive *live = (HostAudioLive *)user;
    pa_threaded_mainloop_lock(live->loop);
    pico_audio_event(&live->audio.mixer, event);
    pa_threaded_mainloop_unlock(live->loop);
}

static void live_stop(void *user) {
    HostAudioLive *live = (HostAudioLive *)user;
    pa_threaded_mainloop_lock(live->loop);
    pico_audio_stop_all(&live->audio.mixer);
    pa_threaded_mainloop_unlock(live->loop);
}

HostSoundSink host_audio_live_sink(HostAudioLive *live) {
    HostSoundSink sink = {live, live_event, live_stop};
    return sink;
}

static void write_samples(pa_stream *stream, size_t bytes, void *user) {
    HostAudioLive *live = (HostAudioLive *)user;
    int16_t samples[HOST_AUDIO_CHUNK * 2];
    unsigned frame_bytes = live->audio.mixer.channels * sizeof(int16_t);
    size_t frames = bytes / frame_bytes;
    /* The callback holds the mainloop lock. Bound work even after an underrun. */
    if (frames > live->audio.mixer.sample_rate / 5)
        frames = live->audio.mixer.sample_rate / 5;
    while (frames && !live->error) {
        size_t count = frames < HOST_AUDIO_CHUNK ? frames : HOST_AUDIO_CHUNK;
        if (!live->started || live->paused)
            memset(samples, 0, count * frame_bytes);
        else
            pico_audio_render(&live->audio.mixer, samples, count);
        if (pa_stream_write(stream, samples, count * frame_bytes, NULL, 0, PA_SEEK_RELATIVE) < 0) {
            live->error = pa_context_errno(live->context);
            break;
        }
        frames -= count;
    }
}

static void stream_state(pa_stream *stream, void *user) {
    HostAudioLive *live = (HostAudioLive *)user;
    pa_stream_state_t state = pa_stream_get_state(stream);
    if (state == PA_STREAM_READY)
        live->ready = 1;
    else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
        live->error = pa_context_errno(live->context) ? pa_context_errno(live->context) : PA_ERR_CONNECTIONTERMINATED;
}

static void context_state(pa_context *context, void *user) {
    HostAudioLive *live = (HostAudioLive *)user;
    pa_context_state_t state = pa_context_get_state(context);
    if (state == PA_CONTEXT_READY) {
        pa_sample_spec spec = {PA_SAMPLE_S16NE, PICO_DEFAULT_AUDIO_RATE,
                               PICO_DEFAULT_AUDIO_CHANNELS};
        pa_buffer_attr buffers;
        memset(&buffers, 0xff, sizeof(buffers));
        buffers.maxlength = spec.rate * spec.channels * 2 / 5;
        buffers.tlength = spec.rate * spec.channels * 2 / 20;
        live->stream = pa_stream_new(context, "Game audio", &spec, NULL);
        if (!live->stream) {
            live->error = pa_context_errno(context);
            return;
        }
        pa_stream_set_state_callback(live->stream, stream_state, live);
        pa_stream_set_write_callback(live->stream, write_samples, live);
        if (pa_stream_connect_playback(live->stream, NULL, &buffers, PA_STREAM_ADJUST_LATENCY,
                                       NULL, NULL) < 0)
            live->error = pa_context_errno(context);
    } else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
        live->error = pa_context_errno(context) ? pa_context_errno(context) : PA_ERR_CONNECTIONTERMINATED;
}

HostAudioLive *host_audio_live_open(const char *path) {
    HostAudioLive *live = (HostAudioLive *)calloc(1, sizeof(*live));
    double deadline;
    int ready = 0, error = 0;
    if (!live)
        return NULL;
    if (!host_audio_open(&live->audio, path, PICO_DEFAULT_AUDIO_RATE, PICO_DEFAULT_AUDIO_CHANNELS)) {
        fprintf(stderr, "Cannot open sound pack: %s\n", path);
        free(live);
        return NULL;
    }
    live->loop = pa_threaded_mainloop_new();
    if (!live->loop)
        goto failed;
    live->context = pa_context_new(pa_threaded_mainloop_get_api(live->loop), "Pico's School");
    if (!live->context)
        goto failed;
    pa_context_set_state_callback(live->context, context_state, live);
    if (pa_context_connect(live->context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
        goto failed;
    if (pa_threaded_mainloop_start(live->loop) < 0)
        goto failed;
    live->loop_started = 1;
    deadline = host_now() + 2.0;
    do {
        struct timespec pause = {0, 20000000};
        pa_threaded_mainloop_lock(live->loop);
        ready = live->ready;
        error = live->error;
        pa_threaded_mainloop_unlock(live->loop);
        if (ready || error)
            break;
        nanosleep(&pause, NULL);
    } while (host_now() < deadline);
    if (ready && !error)
        return live;
    if (!error)
        error = PA_ERR_TIMEOUT;
failed:
    fprintf(stderr, "Audio output unavailable: %s\n", pa_strerror(error ? error : PA_ERR_CONNECTIONREFUSED));
    host_audio_live_close(live);
    return NULL;
}

int host_audio_live_start(HostAudioLive *live) {
    int ready;
    pa_threaded_mainloop_lock(live->loop);
    ready = live->ready && !live->error;
    if (ready)
        live->started = 1;
    pa_threaded_mainloop_unlock(live->loop);
    return ready;
}

void host_audio_live_pause(HostAudioLive *live, int paused) {
    if (!live)
        return;
    pa_threaded_mainloop_lock(live->loop);
    live->paused = paused;
    pa_threaded_mainloop_unlock(live->loop);
}

void host_audio_live_close(HostAudioLive *live) {
    if (!live)
        return;
    /* Stop wakes the local event loop. It doesn't wait for the server to drain. */
    if (live->loop_started)
        pa_threaded_mainloop_stop(live->loop);
    if (live->error)
        fprintf(stderr, "Audio output stopped: %s\n", pa_strerror(live->error));
    if (live->stream) {
        pa_stream_set_state_callback(live->stream, NULL, NULL);
        pa_stream_set_write_callback(live->stream, NULL, NULL);
        pa_stream_disconnect(live->stream);
        pa_stream_unref(live->stream);
    }
    if (live->context) {
        pa_context_set_state_callback(live->context, NULL, NULL);
        pa_context_disconnect(live->context);
        pa_context_unref(live->context);
    }
    if (live->loop)
        pa_threaded_mainloop_free(live->loop);
    host_audio_report(&live->audio, stderr);
    host_audio_close(&live->audio);
    free(live);
}
