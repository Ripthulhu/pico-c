#include "host_common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SoundLog {
    PicoSoundEvent events[128];
    unsigned count, stops;
} SoundLog;

static HostApp file_app, memory_app;

static void sound_event(void *user, const PicoSoundEvent *event) {
    SoundLog *log = (SoundLog *)user;
    assert(log->count < sizeof(log->events) / sizeof(log->events[0]));
    log->events[log->count++] = *event;
}

static void stop_sounds(void *user) {
    ((SoundLog *)user)->stops++;
}

static uint32_t bytes_hash(const unsigned char *bytes, size_t size) {
    uint32_t hash = 2166136261u;
    size_t i;
    for (i = 0; i < size; i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static void same_game_and_frame(const SoundLog *file_sound, const SoundLog *memory_sound) {
    Pico expected = file_app.game;
    unsigned i;
    /* Only the callback owner differs between the two instances. */
    expected.host.user = &memory_app;
    assert(!memcmp(&expected, &memory_app.game, sizeof(expected)));
    assert(!file_app.game.errors && !memory_app.game.errors);
    assert(host_render(&file_app) && host_render(&memory_app));
    assert(host_frame_hash(&file_app) == host_frame_hash(&memory_app));
    assert(!file_app.draw_overflows && !memory_app.draw_overflows);
    assert(file_sound->count == memory_sound->count);
    assert(file_sound->stops == memory_sound->stops);
    for (i = 0; i < file_sound->count; i++) {
        const PicoSoundEvent *a = &file_sound->events[i], *b = &memory_sound->events[i];
        assert(a->symbol == b->symbol && a->loops == b->loops);
        assert(a->stop == b->stop && a->no_multiple == b->no_multiple);
        assert(a->envelope_count == b->envelope_count && a->envelope == b->envelope);
    }
    assert(file_app.sound_events == file_sound->count);
    assert(memory_app.sound_events == memory_sound->count);
    assert(!file_app.silent_sound_events && !memory_app.silent_sound_events);
}

static void check_open(const char *path, unsigned char *bytes, size_t size, unsigned frame) {
    SoundLog file_sound = {0}, memory_sound = {0};
    HostSoundSink file_sink = {&file_sound, sound_event, stop_sounds};
    HostSoundSink memory_sink = {&memory_sound, sound_event, stop_sounds};
    unsigned tick;
    uint32_t hash = bytes_hash(bytes, size);
    assert(host_open_with_sound(&file_app, path, 96, 64, frame, 0, &file_sink));
    assert(host_open_memory(&memory_app, bytes, size, 96, 64, frame, 0, &memory_sink));
    assert(file_app.owns_asset_bytes && !memory_app.owns_asset_bytes);
    assert(memory_app.asset_bytes == bytes && memory_app.asset_size == size);
    if (frame == 22)
        assert(memory_sound.count > 0);
    same_game_and_frame(&file_sound, &memory_sound);
    for (tick = 0; tick < 48; tick++) {
        host_tick(&file_app);
        host_tick(&memory_app);
    }
    same_game_and_frame(&file_sound, &memory_sound);
    host_close(&memory_app);
    host_close(&memory_app);
    assert(!memory_app.asset_bytes && !memory_app.pixels);
    /* Reading and writing the caller's allocation also catches an unwanted
     * free when the test runs under an address checker. */
    assert(bytes_hash(bytes, size) == hash);
    bytes[size - 1] ^= 0xff;
    bytes[size - 1] ^= 0xff;
    assert(host_render(&file_app));
    host_close(&file_app);
}

static void reject_borrowed(unsigned char *bytes, size_t size, unsigned width, unsigned height) {
    uint32_t hash = bytes_hash(bytes, size);
    assert(!host_open_memory(&memory_app, bytes, size, width, height, 2, 0, NULL));
    assert(!memory_app.asset_bytes && !memory_app.pixels && !memory_app.owns_asset_bytes);
    host_close(&memory_app);
    assert(bytes_hash(bytes, size) == hash);
    bytes[0] ^= 0xff;
    bytes[0] ^= 0xff;
}

int main(int argc, char **argv) {
    FILE *file;
    long length;
    size_t size;
    unsigned char *bytes;
    assert(argc == 2);
    file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    length = ftell(file);
    assert(length > 32 && !fseek(file, 0, SEEK_SET));
    size = (size_t)length;
    bytes = (unsigned char *)malloc(size);
    assert(bytes && fread(bytes, 1, size, file) == size);
    assert(!fclose(file));

    check_open(argv[1], bytes, size, 2);
    check_open(argv[1], bytes, size, 22);
    reject_borrowed(bytes, size, 0, 64);
    reject_borrowed(bytes, size, 96, PICO_HOST_MAX_HEIGHT + 1u);
    reject_borrowed(bytes, 1, 96, 64);
    bytes[0] = '?';
    reject_borrowed(bytes, size, 96, 64);
    bytes[0] = 'P';
    assert(!host_open_memory(&memory_app, NULL, 0, 96, 64, 2, 0, NULL));
    host_close(&memory_app);
    check_open(argv[1], bytes, size, 2);
    free(bytes);
    puts("Borrowed artwork preserves frames and initial sound cues without taking ownership.");
    return 0;
}
