#include "host_audio.h"
#include <assert.h>
#include <stdio.h>

static HostApp app;
static HostAudio audio;
static unsigned sample_fraction;

static unsigned voices(unsigned symbol) {
    unsigned i, count = 0;
    for (i = 0; i < PICO_AUDIO_MAX_VOICES; ++i)
        if (audio.mixer.voices[i].active && audio.mixer.voices[i].symbol == symbol)
            ++count;
    return count;
}

static void clean(void) {
    assert(!app.game.errors && !app.game.missing_labels && !app.draw_overflows);
    assert(!audio.mixer.missing_sounds && !audio.mixer.invalid_events);
}

static void ticks(unsigned count) {
    while (count--) {
        int16_t samples[460];
        unsigned frames;
        host_tick(&app);
        sample_fraction += 11025;
        frames = sample_fraction / 24;
        sample_fraction %= 24;
        pico_audio_render(&audio.mixer, samples, frames);
        clean();
    }
}

static void click(unsigned symbol) {
    unsigned x, y;
    assert(!app.game.pointer_down);
    for (y = 1; y < 350; y += 2) {
        for (x = 1; x < 550; x += 2) {
            uint16_t n = pico_hit_test(&app.game, (int32_t)x * 65536, (int32_t)y * 65536);
            if (n != PICO_NONE && app.game.instances[n].symbol == symbol) {
                pico_pointer(&app.game, (int32_t)x * 65536, (int32_t)y * 65536, 1);
                pico_pointer(&app.game, (int32_t)x * 65536, (int32_t)y * 65536, 0);
                clean();
                return;
            }
        }
    }
    fprintf(stderr, "Missing music-route button %u at root %u, room %u\n", symbol,
            pico_frame(&app.game, "/"), pico_frame(&app.game, "/room"));
    assert(0);
}

static void hallway_music(void) {
    assert(pico_frame(&app.game, "/") == 9);
    assert(voices(316) == 0);
    assert(voices(321) == 0);
    assert(voices(242) == 0);
    assert(voices(215) == 1);
}

static void hanzou_exit(void) {
    unsigned revisit;
    /* Diagnostic entries skip dialogue and the already-covered fight, but
     * include its authored music reset, battle cue and victory transition. */
    pico_goto(&app.game, "/", 15, 0);
    ticks(4);
    pico_goto(&app.game, "/room", 60, 1);
    ticks(4);
    assert(pico_frame(&app.game, "/room") == 64);
    assert(voices(321) == 1 && voices(242) == 0);
    pico_goto(&app.game, "/room", 250, 1);
    assert(voices(321) == 1);
    ticks(3);
    assert(pico_frame(&app.game, "/room") == 253);
    assert(voices(321) == 0);
    ticks(41);
    assert(pico_frame(&app.game, "/room") == 294);
    assert(voices(316) == 1 && voices(215) == 0);
    click(751);
    ticks(24);
    assert(pico_frame(&app.game, "/action_room15") == 5);
    click(746);
    ticks(24);
    hallway_music();
    ticks(240);
    hallway_music();

    /* Completed-room revisits must not accumulate a new hallway loop on top
     * of the old one. Only the intervening corridor route is skipped. */
    for (revisit = 0; revisit < 2; ++revisit) {
        pico_goto(&app.game, "/", 15, 0);
        pico_play(&app.game, "/action_room15");
        ticks(24);
        assert(pico_frame(&app.game, "/room") == 295);
        click(746);
        ticks(24);
        hallway_music();
    }
    puts("Music: Hanzou battle stops, reward exits to one hallway loop, revisits stay clean.");
}

static void layered_classroom(void) {
    /* This room intentionally layers two authored loops. A blanket rule that
     * every music start replaces the previous voice would break it. */
    pico_goto(&app.game, "/", 16, 0);
    ticks(24);
    click(756);
    ticks(24);
    assert(pico_frame(&app.game, "/") == 17);
    assert(voices(242) == 1 && voices(759) == 1);
    assert(voices(215) == 0 && voices(316) == 0 && voices(321) == 0);
    ticks(240);
    assert(voices(242) == 1 && voices(759) == 1);
    puts("Music: the classroom retains its authored two-loop mix.");
}

int main(int argc, char **argv) {
    HostSoundSink sink;
    assert(argc == 3);
    assert(host_audio_open(&audio, argv[2], 11025, 1));
    sink = host_audio_sink(&audio);
    assert(host_open_with_sound(&app, argv[1], 96, 64, 2, 0, &sink));
    hanzou_exit();
    layered_classroom();
    assert(!audio.mixer.dropped_events);
    clean();
    host_close(&app);
    assert(host_audio_close(&audio));
    return 0;
}
