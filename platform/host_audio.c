#define _POSIX_C_SOURCE 200809L
#include "host_audio.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static int file_identity(int fd, uint64_t identity[2]) {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle((HANDLE)_get_osfhandle(fd), &info))
        return 0;
    identity[0] = info.dwVolumeSerialNumber;
    identity[1] = (uint64_t)info.nFileIndexHigh << 32 | info.nFileIndexLow;
#else
    struct stat st;
    if (fstat(fd, &st))
        return 0;
    identity[0] = (uint64_t)st.st_dev;
    identity[1] = (uint64_t)st.st_ino;
#endif
    return 1;
}

static int matching_identity(const uint64_t first[2], const uint64_t second[2]) {
    return first[0] == second[0] && first[1] == second[1];
}

static void close_descriptor(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

static void audio_event(void *user, const PicoSoundEvent *event) {
    pico_audio_event(&((HostAudio *)user)->mixer, event);
}

static void audio_stop(void *user) {
    pico_audio_stop_all(&((HostAudio *)user)->mixer);
}

HostSoundSink host_audio_sink(HostAudio *audio) {
    HostSoundSink sink = {audio, audio_event, audio_stop};
    return sink;
}

int host_audio_open(HostAudio *audio, const char *path, unsigned rate, unsigned channels) {
    memset(audio, 0, sizeof(*audio));
#ifdef _WIN32
    {
        FILE *file = fopen(path, "rb");
        long length;
        void *bytes;
        if (!file)
            return 0;
        if (!file_identity(_fileno(file), audio->source_identity) || fseek(file, 0, SEEK_END) ||
            (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) {
            fclose(file);
            return 0;
        }
        bytes = malloc((size_t)length);
        if (!bytes) {
            fclose(file);
            return 0;
        }
        if (fread(bytes, 1, (size_t)length, file) != (size_t)length) {
            free(bytes);
            fclose(file);
            return 0;
        }
        fclose(file);
        audio->bytes = bytes;
        audio->size = (size_t)length;
    }
#else
    {
        struct stat st;
        void *bytes;
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return 0;
        if (fstat(fd, &st) || st.st_size <= 0 || (uint64_t)st.st_size > SIZE_MAX) {
            close(fd);
            return 0;
        }
        audio->source_identity[0] = (uint64_t)st.st_dev;
        audio->source_identity[1] = (uint64_t)st.st_ino;
        bytes = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (bytes == MAP_FAILED)
            return 0;
        audio->bytes = bytes;
        audio->size = (size_t)st.st_size;
    }
#endif
    if (!pico_audio_init(&audio->mixer, audio->bytes, audio->size, rate, channels)) {
        host_audio_close(audio);
        return 0;
    }
    return 1;
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value) {
    put16(p, (uint16_t)value);
    put16(p + 2, (uint16_t)(value >> 16));
}

static int wav_header(HostAudio *audio) {
    uint8_t header[44] = {0};
    unsigned channels = audio->mixer.channels;
    uint64_t bytes = audio->wav_frames * channels * 2;
    if (bytes > UINT32_MAX - 36u)
        return 0;
    memcpy(header, "RIFF", 4);
    put32(header + 4, 36u + (uint32_t)bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);
    put16(header + 20, 1);
    put16(header + 22, (uint16_t)channels);
    put32(header + 24, audio->mixer.sample_rate);
    put32(header + 28, audio->mixer.sample_rate * channels * 2);
    put16(header + 32, (uint16_t)(channels * 2));
    put16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put32(header + 40, (uint32_t)bytes);
    return !fseek(audio->wav, 0, SEEK_SET) && fwrite(header, 1, 44, audio->wav) == 44;
}

int host_audio_wav_open(HostAudio *audio, const char *path, const char *art_path) {
    uint64_t output_identity[2], art_identity[2];
    FILE *art;
    int fd, identified;
    if (!audio->bytes || audio->wav)
        return 0;
    art = fopen(art_path, "rb");
    if (!art)
        return 0;
#ifdef _WIN32
    identified = file_identity(_fileno(art), art_identity);
#else
    identified = file_identity(fileno(art), art_identity);
#endif
    fclose(art);
    if (!identified)
        return 0;
    /* Open without truncation, then inspect that same descriptor. Names alone
     * miss hard links and can change between the check and the open. */
#ifdef _WIN32
    fd = _open(path, _O_WRONLY | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    fd = open(path, O_WRONLY | O_CREAT, 0666);
#endif
    if (fd < 0)
        return 0;
    if (!file_identity(fd, output_identity) ||
        matching_identity(output_identity, audio->source_identity) ||
        matching_identity(output_identity, art_identity)) {
        fprintf(stderr, "WAV output cannot overwrite an input file: %s\n", path);
        close_descriptor(fd);
        return 0;
    }
#ifdef _WIN32
    if (_chsize_s(fd, 0)) {
#else
    if (ftruncate(fd, 0)) {
#endif
        close_descriptor(fd);
        return 0;
    }
#ifdef _WIN32
    audio->wav = _fdopen(fd, "wb");
#else
    audio->wav = fdopen(fd, "wb");
#endif
    if (!audio->wav) {
        close_descriptor(fd);
        return 0;
    }
    if (!wav_header(audio)) {
        fclose(audio->wav);
        audio->wav = NULL;
        return 0;
    }
    return 1;
}

int host_audio_wav_tick(HostAudio *audio) {
    int16_t samples[HOST_AUDIO_CHUNK * 2];
    uint8_t bytes[HOST_AUDIO_CHUNK * 4];
    unsigned frames, channels = audio->mixer.channels;
    if (!audio->wav || audio->failed)
        return 0;
    audio->tick_fraction += audio->mixer.sample_rate;
    frames = audio->tick_fraction / 24;
    audio->tick_fraction %= 24;
    if ((audio->wav_frames + frames) * channels * 2 > UINT32_MAX - 36u) {
        audio->failed = 1;
        return 0;
    }
    while (frames) {
        unsigned i, count = frames < HOST_AUDIO_CHUNK ? frames : HOST_AUDIO_CHUNK;
        pico_audio_render(&audio->mixer, samples, count);
        for (i = 0; i < count * channels; i++)
            put16(bytes + i * 2, (uint16_t)samples[i]);
        if (fwrite(bytes, channels * 2, count, audio->wav) != count) {
            audio->failed = 1;
            return 0;
        }
        audio->wav_frames += count;
        frames -= count;
    }
    return 1;
}

void host_audio_report(const HostAudio *audio, FILE *out) {
    fprintf(out, "Audio: %u Hz, %u channels, %zu bytes of mixer state, %zu bytes of sound ROM.\n",
            audio->mixer.sample_rate, audio->mixer.channels, sizeof(PicoAudio), audio->size);
    fprintf(out, "Audio: %u peak voices, %u dropped cues, %u missing sounds, %u clipped samples.\n",
            audio->mixer.peak_voices, audio->mixer.dropped_events, audio->mixer.missing_sounds,
            audio->mixer.clipped_samples);
}

int host_audio_close(HostAudio *audio) {
    int ok = !audio->failed;
    if (audio->wav) {
        if (!wav_header(audio))
            ok = 0;
        if (fclose(audio->wav))
            ok = 0;
        audio->wav = NULL;
    }
    if (audio->bytes) {
#ifdef _WIN32
        free((void *)audio->bytes);
#else
        munmap((void *)audio->bytes, audio->size);
#endif
    }
    audio->bytes = NULL;
    memset(&audio->mixer, 0, sizeof(audio->mixer));
    return ok;
}
