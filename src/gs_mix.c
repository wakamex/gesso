#include "gs_mix.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#define MAX_SOURCES 32
#define CHUNK 512
#define TAP 16384  // frames of recent output kept for gs_mix_recent (a power of two)

static struct {
    SDL_AudioStream *stream;
    int rate;
    float volume;
    int n;
    uint64_t busy_ns, rendered;  // for gs_mix_load
    struct { gs_mix_fn *fn; void *user; } src[MAX_SOURCES];
    uint64_t tapped;  // frames written to tap in all
    int device_frames;  // the playback device's buffer
} mix = { .rate = 48000, .volume = 1 };
// Apart from mix, which has initial values and so is stored in the program file: zeros cost nothing.
static float tap[TAP * 2];

void gs_mix_render(float *lr, int frames) {
    uint64_t start = SDL_GetTicksNS();
    memset(lr, 0, sizeof(float) * 2 * (size_t)frames);
    for (int i = 0; i < mix.n; i++) mix.src[i].fn(mix.src[i].user, lr, frames);
    for (int i = 0; i < frames; i++, mix.tapped++) {  // before the volume, so scopes keep their size
        size_t k = (size_t)(mix.tapped & (TAP - 1)) * 2;
        tap[k] = lr[2 * i], tap[k + 1] = lr[2 * i + 1];
    }
    for (int i = 0; i < 2 * frames; i++) {
        float x = lr[i] * mix.volume;
        lr[i] = x > 1 || x < -1 ? tanhf(x) : x;  // clean below full scale, soft above
    }
    mix.busy_ns += SDL_GetTicksNS() - start;
    mix.rendered += (uint64_t)frames;
}

double gs_mix_load(void) {
    gs_mix_lock();
    double load = mix.rendered ? mix.busy_ns / 1e9 / ((double)mix.rendered / mix.rate) : -1;
    mix.busy_ns = mix.rendered = 0;
    gs_mix_unlock();
    return load;
}

static void SDLCALL feed(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)user, (void)total;
    float buf[CHUNK * 2];
    for (int frames = additional / (int)(2 * sizeof(float)); frames > 0; frames -= CHUNK) {
        int n = frames < CHUNK ? frames : CHUNK;
        gs_mix_render(buf, n);
        SDL_PutAudioStreamData(stream, buf, n * (int)(2 * sizeof(float)));
    }
}

bool gs_mix_open(int rate) {
    mix.rate = rate;
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, rate };
    mix.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!mix.stream) return false;
    SDL_AudioSpec device;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(mix.stream), &device, &mix.device_frames)) mix.device_frames = 0;
    else if (device.freq > 0 && device.freq != rate) mix.device_frames = (int)((int64_t)mix.device_frames * rate / device.freq);
    return SDL_ResumeAudioStreamDevice(mix.stream);
}

int gs_mix_recent(float *lr, int frames) {
    if (frames > TAP / 2) frames = TAP / 2;
    gs_mix_lock();
    // Mixed but still waiting in the stream for the device: not heard yet, so skipped.
    uint64_t waiting = mix.stream ? (uint64_t)SDL_GetAudioStreamQueued(mix.stream) / (2 * sizeof(float)) : 0;
    uint64_t end = mix.tapped > waiting ? mix.tapped - waiting : 0, start = end > (uint64_t)frames ? end - (uint64_t)frames : 0;
    int n = (int)(end - start);
    for (int i = 0; i < n; i++) {
        size_t k = (size_t)((start + (uint64_t)i) & (TAP - 1)) * 2;
        lr[2 * i] = tap[k], lr[2 * i + 1] = tap[k + 1];
    }
    gs_mix_unlock();
    return n;
}

void gs_mix_close(void) {
    if (mix.stream) SDL_DestroyAudioStream(mix.stream);
    mix.stream = NULL;
}

int gs_mix_rate(void) { return mix.rate; }

int gs_mix_latency_frames(void) {
    if (!mix.stream) return 0;
    return SDL_GetAudioStreamQueued(mix.stream) / (int)(2 * sizeof(float)) + mix.device_frames;
}
void gs_mix_lock(void) { if (mix.stream) SDL_LockAudioStream(mix.stream); }
void gs_mix_unlock(void) { if (mix.stream) SDL_UnlockAudioStream(mix.stream); }

void gs_mix_set_volume(float v) {
    gs_mix_lock();
    mix.volume = v;
    gs_mix_unlock();
}

void gs_mix_add(gs_mix_fn *fn, void *user) {
    gs_mix_lock();
    if (mix.n < MAX_SOURCES) mix.src[mix.n].fn = fn, mix.src[mix.n++].user = user;
    gs_mix_unlock();
}

void gs_mix_remove(gs_mix_fn *fn, void *user) {
    gs_mix_lock();
    for (int i = 0; i < mix.n; i++)
        if (mix.src[i].fn == fn && mix.src[i].user == user) mix.src[i] = mix.src[--mix.n];
    gs_mix_unlock();
}
