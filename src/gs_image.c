#include "gs_image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gs_http.h"
#include "stb_image.h"

#define TEXTURE_FRAMES 300  // a texture not drawn for this many frames (5 s at 60 fps) is freed
#define MAX_IMAGES 1024

typedef enum { EMPTY, LOADING, READY, FAILED } image_state;

typedef struct {
    char *url;
    uint32_t hash;
    image_state state;
    uint8_t *data;           // the compressed file
    size_t len;
    uint64_t used;           // the frame it was last drawn in
    SDL_Texture *texture;
    int w, h;                // the texture's size
} image;

struct gs_images {
    SDL_Renderer *r;
    gs_jobs *jobs;
    size_t limit, bytes;
    SDL_Mutex *lock;
    image items[MAX_IMAGES];
    uint64_t frame;
    SDL_AtomicInt arrived;   // downloads finished since the last end of frame
    SDL_AtomicInt pending;
};

typedef struct { gs_images *im; int index; char url[1024]; } download;

static uint32_t hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

gs_images *gs_images_new(SDL_Renderer *r, gs_jobs *jobs, size_t cache_bytes) {
    gs_images *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->r = r, im->jobs = jobs, im->limit = cache_bytes, im->lock = SDL_CreateMutex();
    return im;
}

void gs_images_free(gs_images *im) {
    if (!im) return;
    while (SDL_GetAtomicInt(&im->pending)) SDL_Delay(5);
    for (int i = 0; i < MAX_IMAGES; i++) {
        free(im->items[i].url), SDL_free(im->items[i].data);
        if (im->items[i].texture) SDL_DestroyTexture(im->items[i].texture);
    }
    SDL_DestroyMutex(im->lock);
    free(im);
}

static void fetch(void *user) {
    download *d = user;
    gs_images *im = d->im;
    char *body = NULL;
    size_t len = 0;
    int status = gs_http_fetch(&(gs_http_request){ .url = d->url, .follow = true, .timeout_ms = 20000 }, &body, &len);
    SDL_LockMutex(im->lock);
    image *it = &im->items[d->index];
    if (it->url && !strcmp(it->url, d->url)) {  // (still the same image: the slot may have been reused)
        if (status == 200 && len) it->data = (uint8_t *)body, it->len = len, it->state = READY, im->bytes += len, body = NULL;
        else it->state = FAILED;
    }
    SDL_UnlockMutex(im->lock);
    SDL_free(body);
    free(d);
    SDL_AddAtomicInt(&im->arrived, 1);
    SDL_AddAtomicInt(&im->pending, -1);
}

// Keeps the compressed copies under the limit, dropping the least recently drawn (never one in use now).
static void trim(gs_images *im) {
    while (im->bytes > im->limit) {
        image *oldest = NULL;
        for (int i = 0; i < MAX_IMAGES; i++) {
            image *it = &im->items[i];
            if (it->state == READY && it->used < im->frame && (!oldest || it->used < oldest->used)) oldest = it;
        }
        if (!oldest) return;
        im->bytes -= oldest->len;
        SDL_free(oldest->data);
        if (oldest->texture) SDL_DestroyTexture(oldest->texture);
        free(oldest->url);
        *oldest = (image){ 0 };
    }
}

// Decodes the file and scales it to cover w x h, cropping the excess, with a box filter.
static SDL_Texture *make_texture(gs_images *im, const image *it, int w, int h) {
    int sw, sh, n;
    uint8_t *src = stbi_load_from_memory(it->data, (int)it->len, &sw, &sh, &n, 4);
    if (!src) return NULL;
    float k = fmaxf((float)w / sw, (float)h / sh);
    float ox = (sw - w / k) / 2, oy = (sh - h / k) / 2;
    uint8_t *dst = malloc((size_t)w * h * 4);
    for (int y = 0; dst && y < h; y++)
        for (int x = 0; x < w; x++) {
            float x0 = ox + x / k, x1 = ox + (x + 1) / k, y0 = oy + y / k, y1 = oy + (y + 1) / k, acc[4] = { 0 }, area = 0;
            for (int sy = (int)y0; sy < sh && sy < y1; sy++)
                for (int sx = (int)x0; sx < sw && sx < x1; sx++) {
                    float cover = (fminf(x1, sx + 1.0f) - fmaxf(x0, (float)sx)) * (fminf(y1, sy + 1.0f) - fmaxf(y0, (float)sy));
                    for (int c = 0; c < 4; c++) acc[c] += src[4 * ((size_t)sy * sw + sx) + c] * cover;
                    area += cover;
                }
            for (int c = 0; c < 4; c++) dst[4 * ((size_t)y * w + x) + c] = (uint8_t)(area > 0 ? acc[c] / area + 0.5f : 0);
        }
    stbi_image_free(src);
    SDL_Texture *t = dst ? SDL_CreateTexture(im->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h) : NULL;
    if (t) SDL_UpdateTexture(t, NULL, dst, w * 4), SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    free(dst);
    return t;
}

SDL_Texture *gs_images_get(gs_images *im, const char *url, int w, int h) {
    if (!url || !url[0] || w <= 0 || h <= 0) return NULL;
    uint32_t hs = hash(url);
    SDL_LockMutex(im->lock);
    image *it = NULL, *slot = NULL;
    for (int i = 0; i < MAX_IMAGES; i++) {  // (comparing hashes first: a few hundred images cost little)
        image *c = &im->items[i];
        if (c->state != EMPTY && c->hash == hs && !strcmp(c->url, url)) { it = c; break; }
        if (!slot && c->state == EMPTY) slot = c;
    }
    if (!it && slot) {  // new: download it
        download *d = malloc(sizeof *d);
        if (d) {
            *slot = (image){ .url = SDL_strdup(url), .hash = hs, .state = LOADING };
            d->im = im, d->index = (int)(slot - im->items);
            SDL_strlcpy(d->url, url, sizeof d->url);
            SDL_AddAtomicInt(&im->pending, 1);
            gs_jobs_add(im->jobs, fetch, d);
        }
        it = slot;
    }
    SDL_Texture *t = NULL;
    if (it) {
        it->used = im->frame;
        if (it->state == READY && (!it->texture || it->w != w || it->h != h)) {
            if (it->texture) SDL_DestroyTexture(it->texture);
            it->texture = make_texture(im, it, w, h), it->w = w, it->h = h;
            if (!it->texture) it->state = FAILED;
        }
        t = it->texture;
    }
    SDL_UnlockMutex(im->lock);
    return t;
}

bool gs_images_end_frame(gs_images *im) {
    SDL_LockMutex(im->lock);
    for (int i = 0; i < MAX_IMAGES; i++) {
        image *it = &im->items[i];
        if (it->texture && it->used + TEXTURE_FRAMES < im->frame) SDL_DestroyTexture(it->texture), it->texture = NULL;
    }
    im->frame++;
    trim(im);
    SDL_UnlockMutex(im->lock);
    return SDL_SetAtomicInt(&im->arrived, 0) != 0;
}

size_t gs_images_bytes(const gs_images *im) { return im->bytes; }

bool gs_images_put(gs_images *im, const char *url, const void *data, size_t len) {
    uint8_t *copy = SDL_malloc(len);
    if (!copy) return false;
    memcpy(copy, data, len);
    SDL_LockMutex(im->lock);
    image *slot = NULL;
    for (int i = 0; i < MAX_IMAGES && !slot; i++)
        if (im->items[i].state == EMPTY) slot = &im->items[i];
    if (slot) *slot = (image){ .url = SDL_strdup(url), .hash = hash(url), .state = READY, .data = copy, .len = len, .used = im->frame }, im->bytes += len;
    SDL_UnlockMutex(im->lock);
    if (!slot) SDL_free(copy);
    return slot != NULL;
}
