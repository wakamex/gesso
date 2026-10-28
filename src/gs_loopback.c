#include "gs_loopback.h"

#include <SDL3/SDL.h>
#include <string.h>

#define KEEP_MS 200  // the most kept for a reader that has fallen behind

struct gs_loopback {
    int rate;
    float *ring;  // what was recorded and not yet read
    int cap, start, count;
    SDL_Mutex *lock;
    SDL_Thread *thread;
    SDL_AtomicInt stop;
    char device[256], error[256];
    void *system;  // the backend's own
};

// Recorded samples in, from the recording thread.
static void put(gs_loopback *l, const float *s, int n) {
    SDL_LockMutex(l->lock);
    for (int i = 0; i < n; i++) {
        if (l->count == l->cap) l->start = (l->start + 1) % l->cap, l->count--;  // (the oldest go)
        l->ring[(l->start + l->count++) % l->cap] = s[i];
    }
    SDL_UnlockMutex(l->lock);
}

static void fail(gs_loopback *l, const char *why) {
    SDL_LockMutex(l->lock);
    SDL_strlcpy(l->error, why, sizeof l->error);
    SDL_UnlockMutex(l->lock);
}

int gs_loopback_read(gs_loopback *l, float *out, int frames) {
    SDL_LockMutex(l->lock);
    int keep = l->rate * KEEP_MS / 1000 + frames;
    if (l->count > keep) l->start = (l->start + l->count - keep) % l->cap, l->count = keep;
    int got = SDL_min(frames, l->count);
    for (int i = 0; i < got; i++) out[i] = l->ring[(l->start + i) % l->cap];
    l->start = (l->start + got) % l->cap, l->count -= got;
    SDL_UnlockMutex(l->lock);
    for (int i = got; i < frames; i++) out[i] = 0;
    return got;
}

void gs_loopback_error(gs_loopback *l, char *error, size_t size) {
    SDL_LockMutex(l->lock);
    SDL_strlcpy(error, l->error, size);
    SDL_UnlockMutex(l->lock);
}

#if defined(_WIN32)
// ---- Windows: WASAPI loopback ----
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

static const GUID CLSID_ENUMERATOR = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const GUID IID_ENUMERATOR = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const GUID IID_CLIENT = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const GUID IID_CAPTURE = { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
static const GUID FORMAT_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT

typedef struct {
    IMMDeviceEnumerator *devices;
    IAudioClient *client;
    IAudioCaptureClient *capture;
    SDL_AudioStream *convert;  // the device's mix to mono at the asked rate
    int channels, bytes_per_frame;
    WCHAR id[256];             // the device recorded
} wasapi;

static void close_device(wasapi *w) {
    if (w->client) IAudioClient_Stop(w->client);
    if (w->capture) IAudioCaptureClient_Release(w->capture);
    if (w->client) IAudioClient_Release(w->client);
    SDL_DestroyAudioStream(w->convert);
    w->capture = NULL, w->client = NULL, w->convert = NULL, w->id[0] = 0;
}

// The default output device's id, into `id`; false when there is none.
static bool default_id(wasapi *w, WCHAR *id, size_t size) {
    IMMDevice *d = NULL;
    WCHAR *s = NULL;
    bool ok = SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(w->devices, eRender, eConsole, &d)) && SUCCEEDED(IMMDevice_GetId(d, &s));
    if (ok) wcsncpy(id, s, size - 1), id[size - 1] = 0;
    CoTaskMemFree(s);
    if (d) IMMDevice_Release(d);
    return ok;
}

static bool open_device(gs_loopback *l, wasapi *w, char *error, size_t size) {
    IMMDevice *d = NULL;
    WAVEFORMATEX *mix = NULL;
    HRESULT hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(w->devices, eRender, eConsole, &d);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(d, &IID_CLIENT, CLSCTX_ALL, NULL, (void **)&w->client);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetMixFormat(w->client, &mix);
    if (SUCCEEDED(hr)) hr = IAudioClient_Initialize(w->client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000 /* 200 ms, in 100 ns */, 0, mix, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(w->client, &IID_CAPTURE, (void **)&w->capture);
    SDL_AudioFormat format = 0;
    if (SUCCEEDED(hr)) {
        bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)mix)->SubFormat, &FORMAT_FLOAT));
        format = is_float && mix->wBitsPerSample == 32 ? SDL_AUDIO_F32 : !is_float && mix->wBitsPerSample == 16 ? SDL_AUDIO_S16 : !is_float && mix->wBitsPerSample == 32 ? SDL_AUDIO_S32 : 0;
        if (!format) hr = E_FAIL, SDL_snprintf(error, size, "the output device mixes %d-bit samples, which gs_loopback does not read", mix->wBitsPerSample);
    }
    if (SUCCEEDED(hr)) {
        const SDL_AudioSpec from = { format, mix->nChannels, (int)mix->nSamplesPerSec }, to = { SDL_AUDIO_F32, 1, l->rate };
        w->convert = SDL_CreateAudioStream(&from, &to);
        w->channels = mix->nChannels, w->bytes_per_frame = mix->nBlockAlign;
        hr = w->convert ? IAudioClient_Start(w->client) : E_FAIL;
    }
    if (SUCCEEDED(hr)) default_id(w, w->id, SDL_arraysize(w->id));
    else if (!error[0]) SDL_snprintf(error, size, "the output device could not be recorded (0x%08lx)", (unsigned long)hr);
    CoTaskMemFree(mix);
    if (d) IMMDevice_Release(d);
    if (FAILED(hr)) close_device(w);
    return SUCCEEDED(hr);
}

static int record(void *data) {
    gs_loopback *l = data;
    wasapi *w = l->system;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    float out[1024];
    Uint64 checked = SDL_GetTicks();
    while (!SDL_GetAtomicInt(&l->stop)) {
        // Once a second, whether the default device is still the one recorded; if not, the new one.
        if (SDL_GetTicks() - checked >= 1000 || !w->client) {
            WCHAR now[256];
            checked = SDL_GetTicks();
            if (!w->client || (default_id(w, now, SDL_arraysize(now)) && wcscmp(now, w->id))) {
                char why[256] = "";
                close_device(w);
                if (!open_device(l, w, why, sizeof why)) { fail(l, why); SDL_Delay(1000); continue; }
                fail(l, "");
            }
        }
        UINT32 n = 0;
        HRESULT hr = IAudioCaptureClient_GetNextPacketSize(w->capture, &n);
        while (SUCCEEDED(hr) && n) {
            BYTE *bytes;
            UINT32 frames;
            DWORD flags;
            hr = IAudioCaptureClient_GetBuffer(w->capture, &bytes, &frames, &flags, NULL, NULL);
            if (FAILED(hr)) break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {  // (silence it does not spell out)
                static const Uint8 zero[4096] = { 0 };
                for (size_t left = (size_t)frames * w->bytes_per_frame; left;) {
                    size_t k = SDL_min(left, sizeof zero);
                    SDL_PutAudioStreamData(w->convert, zero, (int)k), left -= k;
                }
            } else SDL_PutAudioStreamData(w->convert, bytes, (int)(frames * w->bytes_per_frame));
            IAudioCaptureClient_ReleaseBuffer(w->capture, frames);
            hr = IAudioCaptureClient_GetNextPacketSize(w->capture, &n);
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) close_device(w);  // (gone: the next pass opens the default)
        for (int got; (got = SDL_GetAudioStreamData(w->convert, out, sizeof out)) > 0;) put(l, out, got / (int)sizeof *out);
        SDL_Delay(10);
    }
    close_device(w);
    CoUninitialize();
    return 0;
}

static bool start_system(gs_loopback *l, char *error, size_t size) {
    if (l->device[0]) return SDL_strlcpy(error, "on Windows only the default output can be recorded", size), false;
    wasapi *w = SDL_calloc(1, sizeof *w);
    l->system = w;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!w || FAILED(CoCreateInstance(&CLSID_ENUMERATOR, NULL, CLSCTX_ALL, &IID_ENUMERATOR, (void **)&w->devices)))
        return SDL_strlcpy(error, "Windows' audio devices could not be listed", size), false;
    if (!open_device(l, w, error, size)) return false;  // (a first try here, for its error; the thread opens its own)
    close_device(w);
    l->thread = SDL_CreateThread(record, "gs-loopback", l);
    return l->thread != NULL;
}

static void stop_system(gs_loopback *l) {
    wasapi *w = l->system;
    if (!w) return;
    if (w->devices) IMMDeviceEnumerator_Release(w->devices);
    SDL_free(w);
}

#elif defined(__linux__)
// ---- Linux: the default sink's monitor, through libpulse-simple ----
typedef struct { int format; unsigned rate; unsigned char channels; } pa_spec;
typedef struct { unsigned maxlength, tlength, prebuf, minreq, fragsize; } pa_attr;

typedef struct {
    SDL_SharedObject *lib;
    void *(*make)(const char *, const char *, int, const char *, const char *, const pa_spec *, const void *, const pa_attr *, int *);
    int (*read)(void *, void *, size_t, int *);
    void (*free)(void *);
    const char *(*strerror)(int);
    void *pa;
} pulse;

static int record(void *data) {
    gs_loopback *l = data;
    pulse *p = l->system;
    float block[1024];
    int frames = SDL_min(l->rate / 100, (int)SDL_arraysize(block));  // (10 ms at a time)
    while (!SDL_GetAtomicInt(&l->stop)) {
        int err = 0;
        if (p->read(p->pa, block, sizeof *block * (size_t)frames, &err) < 0) {
            fail(l, p->strerror(err));
            break;
        }
        put(l, block, frames);
    }
    return 0;
}

static bool start_system(gs_loopback *l, char *error, size_t size) {
    pulse *p = SDL_calloc(1, sizeof *p);
    l->system = p;
    p->lib = p ? SDL_LoadObject("libpulse-simple.so.0") : NULL;
    if (p && p->lib) {
        *(SDL_FunctionPointer *)&p->make = SDL_LoadFunction(p->lib, "pa_simple_new");
        *(SDL_FunctionPointer *)&p->read = SDL_LoadFunction(p->lib, "pa_simple_read");
        *(SDL_FunctionPointer *)&p->free = SDL_LoadFunction(p->lib, "pa_simple_free");
        *(SDL_FunctionPointer *)&p->strerror = SDL_LoadFunction(p->lib, "pa_strerror");
    }
    if (!p || !p->make || !p->read || !p->free || !p->strerror)
        return SDL_strlcpy(error, "no PulseAudio client library (libpulse-simple), which PipeWire and PulseAudio desktops have", size), false;
    // Mono floats at the asked rate, in 10 ms fragments: the server mixes down and resamples.
    const pa_spec spec = { 5 /* PA_SAMPLE_FLOAT32LE */, (unsigned)l->rate, 1 };
    const pa_attr attr = { (unsigned)-1, (unsigned)-1, (unsigned)-1, (unsigned)-1, (unsigned)(l->rate / 100 * sizeof(float)) };
    char monitor[300];
    SDL_snprintf(monitor, sizeof monitor, "%s.monitor", l->device);
    const char *app = SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING);
    int err = 0;
    p->pa = p->make(NULL, app ? app : "gesso", 2 /* PA_STREAM_RECORD */, l->device[0] ? monitor : "@DEFAULT_MONITOR@", "what the speakers play", &spec, NULL, &attr, &err);
    if (!p->pa) return SDL_snprintf(error, size, "the output could not be recorded: %s", p->strerror(err)), false;
    l->thread = SDL_CreateThread(record, "gs-loopback", l);
    return l->thread != NULL;
}

static void stop_system(gs_loopback *l) {
    pulse *p = l->system;
    if (!p) return;
    if (p->pa) p->free(p->pa);
    SDL_UnloadObject(p->lib);
    SDL_free(p);
}

#else
static bool start_system(gs_loopback *l, char *error, size_t size) {
    (void)l;
    SDL_strlcpy(error, "recording what the speakers play is not available on this system yet", size);
    return false;
}
static void stop_system(gs_loopback *l) { (void)l; }
#endif

gs_loopback *gs_loopback_open(const char *device, int rate, char *error, size_t error_size) {
    if (error_size) error[0] = 0;
    if (rate < 8000 || rate > 192000) return SDL_strlcpy(error, "the rate is not one gs_loopback takes", error_size), NULL;
    gs_loopback *l = SDL_calloc(1, sizeof *l);
    if (!l) return NULL;
    l->rate = rate, l->cap = rate;  // (a second, of which gs_loopback_read keeps 200 ms)
    l->ring = SDL_malloc(sizeof *l->ring * (size_t)l->cap), l->lock = SDL_CreateMutex();
    if (device) SDL_strlcpy(l->device, device, sizeof l->device);
    if (!l->ring || !l->lock || !start_system(l, error, error_size)) return gs_loopback_close(l), NULL;
    return l;
}

void gs_loopback_close(gs_loopback *l) {
    if (!l) return;
    SDL_SetAtomicInt(&l->stop, 1);
    SDL_WaitThread(l->thread, NULL);
    stop_system(l);
    SDL_DestroyMutex(l->lock);
    SDL_free(l->ring), SDL_free(l);
}
