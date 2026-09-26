#include "gs_mix.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#define MAX_SOURCES 32
#define CHUNK 512

static struct {
    SDL_AudioStream *stream;
    int rate;
    float volume;
    int n;
    struct { gs_mix_fn *fn; void *user; } src[MAX_SOURCES];
} mix = { .rate = 48000, .volume = 1 };

void gs_mix_render(float *lr, int frames) {
    memset(lr, 0, sizeof(float) * 2 * (size_t)frames);
    for (int i = 0; i < mix.n; i++) mix.src[i].fn(mix.src[i].user, lr, frames);
    for (int i = 0; i < 2 * frames; i++) {
        float x = lr[i] * mix.volume;
        lr[i] = x > 1 || x < -1 ? tanhf(x) : x;  // clean below full scale, soft above
    }
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
    return SDL_ResumeAudioStreamDevice(mix.stream);
}

void gs_mix_close(void) {
    if (mix.stream) SDL_DestroyAudioStream(mix.stream);
    mix.stream = NULL;
}

int gs_mix_rate(void) { return mix.rate; }
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
