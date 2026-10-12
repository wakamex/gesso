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
    int nw, nh;              // the image's own size, the right way up, once read (0 before)
    SDL_Texture *full;       // the image whole, for a view of it alone
    uint8_t *full_px;        // its pixels, decoded on a worker and waiting to become that texture
    int fw, fh;
    bool decoding;           // a worker is decoding it: its compressed copy stays
    uint64_t full_used;
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

typedef struct { gs_images *im; int index, max_side; char url[1024]; } download;

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
        free(im->items[i].url), SDL_free(im->items[i].data), free(im->items[i].full_px);
        if (im->items[i].texture) SDL_DestroyTexture(im->items[i].texture);
        if (im->items[i].full) SDL_DestroyTexture(im->items[i].full);
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
            if (it->state == READY && !it->decoding && it->used < im->frame && (!oldest || it->used < oldest->used)) oldest = it;
        }
        if (!oldest) return;
        im->bytes -= oldest->len;
        SDL_free(oldest->data), free(oldest->full_px);
        if (oldest->texture) SDL_DestroyTexture(oldest->texture);
        if (oldest->full) SDL_DestroyTexture(oldest->full);
        free(oldest->url);
        *oldest = (image){ 0 };
    }
}

// How a JPEG says it is to be turned to stand the right way up (Exif's orientation, 1 to 8), which
// phone cameras rely on; 1, no turning, for any other file.
static int orientation(const uint8_t *d, size_t len) {
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
    for (size_t at = 2; at + 4 <= len && d[at] == 0xFF;) {
        size_t size = (size_t)d[at + 2] << 8 | d[at + 3];
        if (d[at + 1] == 0xE1 && at + 4 + 14 <= len && size >= 16 && at + 2 + size <= len && !memcmp(d + at + 4, "Exif\0\0", 6)) {
            const uint8_t *t = d + at + 10, *end = d + at + 2 + size;  // the TIFF block: its byte order, then offsets from here
            bool little = t[0] == 'I';
            #define U16(p) (little ? (uint32_t)((p)[0] | (p)[1] << 8) : (uint32_t)((p)[0] << 8 | (p)[1]))
            #define U32(p) (little ? U16(p) | U16((p) + 2) << 16 : U16(p) << 16 | U16((p) + 2))
            const uint8_t *dir = t + U32(t + 4);
            if (dir + 2 > end) return 1;
            for (uint32_t i = 0, n = U16(dir); i < n && dir + 2 + 12 * (i + 1) <= end; i++) {
                const uint8_t *e = dir + 2 + 12 * i;
                if (U16(e) == 0x0112) { uint32_t o = U16(e + 8); return o >= 1 && o <= 8 ? (int)o : 1; }
            }
            #undef U16
            #undef U32
            return 1;
        }
        if (d[at + 1] == 0xDA) break;  // the picture itself begins
        at += 2 + size;
    }
    return 1;
}

// The file decoded to RGBA, the right way up. free() the result.
static uint8_t *decode(const uint8_t *data, size_t len, int *w, int *h) {
    int sw, sh, n, turn = orientation(data, len);
    uint8_t *src = stbi_load_from_memory(data, (int)len, &sw, &sh, &n, 4);
    if (!src) return NULL;
    *w = turn >= 5 ? sh : sw, *h = turn >= 5 ? sw : sh;
    if (turn == 1) return src;  // (stb's memory is malloc's)
    uint32_t *out = malloc((size_t)sw * sh * 4), *in = (uint32_t *)src;
    for (int y = 0; out && y < *h; y++)
        for (int x = 0; x < *w; x++) {
            int sx = turn == 2 || turn == 3 ? sw - 1 - x : turn == 4 ? x : turn == 5 || turn == 6 ? y : sw - 1 - y;
            int sy = turn == 3 || turn == 4 ? sh - 1 - y : turn == 2 ? y : turn == 5 || turn == 8 ? x : sh - 1 - x;
            out[(size_t)y * *w + x] = in[(size_t)sy * sw + sx];
        }
    stbi_image_free(src);
    return (uint8_t *)out;
}

// Scales pixels to dw x dh with a box filter, taking the part of the source from (ox, oy) on at k
// output pixels to a source pixel.
static uint8_t *scaled(const uint8_t *src, int sw, int sh, int dw, int dh, float k, float ox, float oy) {
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    for (int y = 0; dst && y < dh; y++)
        for (int x = 0; x < dw; x++) {
            float x0 = ox + x / k, x1 = ox + (x + 1) / k, y0 = oy + y / k, y1 = oy + (y + 1) / k, acc[4] = { 0 }, area = 0;
            for (int sy = (int)y0; sy < sh && sy < y1; sy++)
                for (int sx = (int)x0; sx < sw && sx < x1; sx++) {
                    float cover = (fminf(x1, sx + 1.0f) - fmaxf(x0, (float)sx)) * (fminf(y1, sy + 1.0f) - fmaxf(y0, (float)sy));
                    for (int c = 0; c < 4; c++) acc[c] += src[4 * ((size_t)sy * sw + sx) + c] * cover;
                    area += cover;
                }
            for (int c = 0; c < 4; c++) dst[4 * ((size_t)y * dw + x) + c] = (uint8_t)(area > 0 ? acc[c] / area + 0.5f : 0);
        }
    return dst;
}

static SDL_Texture *texture_of(gs_images *im, const uint8_t *px, int w, int h) {
    SDL_Texture *t = px ? SDL_CreateTexture(im->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h) : NULL;
    if (t) SDL_UpdateTexture(t, NULL, px, w * 4), SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

// Decodes the file and scales it to cover w x h, cropping the excess.
static SDL_Texture *make_texture(gs_images *im, image *it, int w, int h) {
    int sw, sh;
    uint8_t *src = decode(it->data, it->len, &sw, &sh);
    if (!src) return NULL;
    it->nw = sw, it->nh = sh;
    float k = fmaxf((float)w / sw, (float)h / sh);
    uint8_t *dst = scaled(src, sw, sh, w, h, k, (sw - w / k) / 2, (sh - h / k) / 2);
    free(src);
    SDL_Texture *t = texture_of(im, dst, w, h);
    free(dst);
    return t;
}

// The image's place in the cache, with its download begun when it is new. Call with the lock held.
static image *entry(gs_images *im, const char *url) {
    uint32_t hs = hash(url);
    image *slot = NULL;
    for (int i = 0; i < MAX_IMAGES; i++) {  // (comparing hashes first: a few hundred images cost little)
        image *c = &im->items[i];
        if (c->state != EMPTY && c->hash == hs && !strcmp(c->url, url)) return c;
        if (!slot && c->state == EMPTY) slot = c;
    }
    download *d = slot ? malloc(sizeof *d) : NULL;
    if (!d) return NULL;
    *slot = (image){ .url = SDL_strdup(url), .hash = hs, .state = LOADING };
    d->im = im, d->index = (int)(slot - im->items);
    SDL_strlcpy(d->url, url, sizeof d->url);
    SDL_AddAtomicInt(&im->pending, 1);
    gs_jobs_add(im->jobs, fetch, d);
    return slot;
}

SDL_Texture *gs_images_get(gs_images *im, const char *url, int w, int h) {
    if (!url || !url[0] || w <= 0 || h <= 0) return NULL;
    SDL_LockMutex(im->lock);
    image *it = entry(im, url);
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

bool gs_images_size(gs_images *im, const char *url, int *w, int *h) {
    if (!url || !url[0]) return false;
    SDL_LockMutex(im->lock);
    image *it = entry(im, url);
    if (it && it->state == READY && !it->nw) {  // read from the file's header, without decoding it
        int sw, sh, n;
        if (stbi_info_from_memory(it->data, (int)it->len, &sw, &sh, &n)) {
            bool turned = orientation(it->data, it->len) >= 5;
            it->nw = turned ? sh : sw, it->nh = turned ? sw : sh;
        } else it->state = FAILED;
    }
    bool known = it && it->state == READY && it->nw;
    if (known) *w = it->nw, *h = it->nh, it->used = im->frame;
    SDL_UnlockMutex(im->lock);
    return known;
}

// A worker's job: the image whole, shrunk only as far as the largest texture allows.
static void decode_full(void *user) {
    download *d = user;
    gs_images *im = d->im;
    image *it = &im->items[d->index];
    int w = 0, h = 0;
    uint8_t *px = decode(it->data, it->len, &w, &h);  // (its compressed copy is kept while `decoding` is set)
    int longest = SDL_max(w, h);
    if (px && longest > d->max_side) {
        float k = (float)d->max_side / longest;
        int dw = SDL_max(1, (int)(w * k)), dh = SDL_max(1, (int)(h * k));
        uint8_t *small = scaled(px, w, h, dw, dh, k, 0, 0);
        free(px), px = small, w = dw, h = dh;
    }
    SDL_LockMutex(im->lock);
    it->decoding = false;
    if (it->url && !strcmp(it->url, d->url) && px) it->full_px = px, it->fw = w, it->fh = h, it->nw = it->nw ? it->nw : w, it->nh = it->nh ? it->nh : h, px = NULL;
    else if (it->url && !strcmp(it->url, d->url)) it->state = FAILED;
    SDL_UnlockMutex(im->lock);
    free(px), free(d);
    SDL_AddAtomicInt(&im->arrived, 1);
    SDL_AddAtomicInt(&im->pending, -1);
}

SDL_Texture *gs_images_full(gs_images *im, const char *url, int *w, int *h) {
    if (!url || !url[0]) return NULL;
    SDL_LockMutex(im->lock);
    image *it = entry(im, url);
    SDL_Texture *t = NULL;
    if (it && it->state == READY) {
        it->used = it->full_used = im->frame;
        if (!it->full && it->full_px) {  // decoded: made a texture here, on the rendering thread
            it->full = texture_of(im, it->full_px, it->fw, it->fh);
            free(it->full_px), it->full_px = NULL;
            if (!it->full) it->state = FAILED;
        } else if (!it->full && !it->decoding) {
            download *d = malloc(sizeof *d);
            if (d) {
                int limit = (int)SDL_GetNumberProperty(SDL_GetRendererProperties(im->r), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096);
                d->im = im, d->index = (int)(it - im->items), d->max_side = SDL_clamp(limit, 256, 8192);
                SDL_strlcpy(d->url, url, sizeof d->url);
                it->decoding = true;
                SDL_AddAtomicInt(&im->pending, 1);
                gs_jobs_add(im->jobs, decode_full, d);
            }
        }
        if ((t = it->full)) {
            if (w) *w = it->fw;
            if (h) *h = it->fh;
        }
    }
    SDL_UnlockMutex(im->lock);
    return t;
}

bool gs_images_end_frame(gs_images *im) {
    SDL_LockMutex(im->lock);
    for (int i = 0; i < MAX_IMAGES; i++) {
        image *it = &im->items[i];
        if (it->texture && it->used + TEXTURE_FRAMES < im->frame) SDL_DestroyTexture(it->texture), it->texture = NULL;
        if (it->full && it->full_used + TEXTURE_FRAMES < im->frame) SDL_DestroyTexture(it->full), it->full = NULL;
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
