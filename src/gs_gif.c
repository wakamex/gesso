#include "gs_gif.h"

#include <stdint.h>

// ---- Reading the file, a frame at a time (GIF89a, w3.org/Graphics/GIF/spec-gif89a.txt) ----

typedef struct {
    const uint8_t *d;
    size_t len, at, first;    // the bytes; where reading is; where the first frame's blocks begin
    int w, h;
    uint8_t global[256][3];
    int global_n;
    uint8_t *canvas, *saved;  // the picture as composed; the part a "restore to previous" frame puts back
    uint8_t *index;           // one frame's colour indices
    uint16_t prefix[4096];    // the LZW dictionary
    uint8_t suffix[4096], stack[4097];
    int dispose, fx, fy, fw, fh;  // how the frame shown last goes, and where it lies
} gs_gif_reader;

static int byte_at(gs_gif_reader *r) { return r->at < r->len ? r->d[r->at++] : -1; }
static int u16_at(gs_gif_reader *r) { int a = byte_at(r), b = byte_at(r); return a < 0 || b < 0 ? -1 : a | b << 8; }
static void skip_blocks(gs_gif_reader *r) {
    for (int n; (n = byte_at(r)) > 0;) r->at += (size_t)n;
}

static void gs_gif_reader_free(gs_gif_reader *r) {
    if (r) SDL_free(r->canvas), SDL_free(r->saved), SDL_free(r->index), SDL_free(r);
}

static void gs_gif_reader_restart(gs_gif_reader *r) {
    r->at = r->first, r->dispose = 0;
    SDL_memset(r->canvas, 0, (size_t)r->w * r->h * 4);  // (the picture starts transparent, as browsers show it)
}

static gs_gif_reader *gs_gif_reader_new(const void *data, size_t len) {
    const uint8_t *d = data;
    if (len < 13 || SDL_memcmp(d, "GIF8", 4) || (d[4] != '7' && d[4] != '9') || d[5] != 'a') return NULL;
    gs_gif_reader *r = SDL_calloc(1, sizeof *r);
    if (!r) return NULL;
    r->d = d, r->len = len, r->at = 6;
    r->w = u16_at(r), r->h = u16_at(r);
    int flags = byte_at(r);
    r->at += 2;  // (the background's index and the aspect ratio, which browsers ignore)
    if (r->w <= 0 || r->h <= 0 || (size_t)r->w * r->h > 1 << 28) return gs_gif_reader_free(r), NULL;
    if (flags & 0x80) {
        r->global_n = 2 << (flags & 7);
        if (r->at + 3 * (size_t)r->global_n > len) return gs_gif_reader_free(r), NULL;
        SDL_memcpy(r->global, d + r->at, 3 * (size_t)r->global_n), r->at += 3 * (size_t)r->global_n;
    }
    r->first = r->at;
    size_t px = (size_t)r->w * r->h;
    r->canvas = SDL_calloc(px, 4), r->saved = SDL_malloc(px * 4), r->index = SDL_malloc(px);
    if (!r->canvas || !r->saved || !r->index) return gs_gif_reader_free(r), NULL;
    return r;
}

// Decodes an image's LZW data into n indices; false when it is broken (what was decoded stays).
static bool lzw(gs_gif_reader *r, uint8_t *out, size_t n) {
    int min = byte_at(r);
    if (min < 1 || min > 11) return false;
    int clear = 1 << min, end = clear + 1, next = clear + 2, size = min + 1, old = -1, first = 0;
    uint32_t bits = 0;
    int nbits = 0, left = 0;
    size_t got = 0;
    for (int i = 0; i < clear; i++) r->prefix[i] = 0xffff, r->suffix[i] = (uint8_t)i;
    for (;;) {
        while (nbits < size) {  // (codes are packed low bit first across sub-blocks)
            if (!left && (left = byte_at(r)) <= 0) return got == n;
            int b = byte_at(r);
            if (b < 0) return false;
            bits |= (uint32_t)b << nbits, nbits += 8, left--;
        }
        int code = (int)(bits & ((1u << size) - 1));
        bits >>= size, nbits -= size;
        if (code == clear) { next = clear + 2, size = min + 1, old = -1; continue; }
        if (code == end) break;
        if (old < 0) {  // the first code after a clear is a colour
            if (code >= clear) return false;
            if (got < n) out[got++] = (uint8_t)code;
            old = first = code;
            continue;
        }
        int in = code, sp = 0;
        if (code > next || code >= 4096) return false;
        if (code == next) r->stack[sp++] = (uint8_t)first, code = old;  // (the code being defined: the last string and its first colour)
        while (code >= clear && sp < 4096) r->stack[sp++] = r->suffix[code], code = r->prefix[code];
        r->stack[sp++] = (uint8_t)code, first = code;
        while (sp && got < n) out[got++] = r->stack[--sp];
        if (next < 4096) {
            r->prefix[next] = (uint16_t)old, r->suffix[next] = (uint8_t)first, next++;
            if (next == 1 << size && size < 12) size++;
        }
        old = in;
    }
    r->at += (size_t)left;  // (the rest of the sub-block the end code was in, and any after it)
    skip_blocks(r);
    return true;
}

// Composes the next frame onto the picture: 4 bytes a pixel, and how long it shows in milliseconds;
// NULL after the last frame, or where the file breaks off.
static const uint8_t *gs_gif_reader_next(gs_gif_reader *r, int *delay_ms) {
    // How the frame shown last goes: its area cleared, or put back as it was before it.
    for (int y = r->fy; (r->dispose == 2 || r->dispose == 3) && y < r->fy + r->fh; y++) {
        uint8_t *row = r->canvas + ((size_t)y * r->w + r->fx) * 4;
        if (r->dispose == 2) SDL_memset(row, 0, (size_t)r->fw * 4);
        else SDL_memcpy(row, r->saved + ((size_t)y * r->w + r->fx) * 4, (size_t)r->fw * 4);
    }
    r->dispose = 0;
    int dispose = 0, delay = 0, transparent = -1;
    for (;;) {
        int b = byte_at(r);
        if (b == 0x21) {  // an extension: the graphic control one says how the next image shows
            int label = byte_at(r);
            if (label == 0xf9 && r->at + 6 <= r->len && r->d[r->at] >= 4) {
                const uint8_t *e = r->d + r->at + 1;
                dispose = e[0] >> 2 & 7, delay = (e[1] | e[2] << 8) * 10, transparent = e[0] & 1 ? e[3] : -1;
            }
            skip_blocks(r);
        } else if (b == 0x2c) {  // an image
            int x = u16_at(r), y = u16_at(r), w = u16_at(r), h = u16_at(r), flags = byte_at(r);
            if (flags < 0) return NULL;
            uint8_t (*table)[3] = r->global, local[256][3];
            int colours = r->global_n;
            if (flags & 0x80) {
                colours = 2 << (flags & 7);
                if (r->at + 3 * (size_t)colours > r->len) return NULL;
                SDL_memcpy(local, r->d + r->at, 3 * (size_t)colours), r->at += 3 * (size_t)colours, table = local;
            }
            size_t n = (size_t)w * h;
            if (n > (size_t)r->w * r->h * 4) return NULL;  // (larger than the picture several times over: broken)
            uint8_t *idx = n <= (size_t)r->w * r->h ? r->index : SDL_malloc(n);
            if (!idx) return NULL;
            SDL_memset(idx, transparent >= 0 ? transparent : 0, n);
            bool whole = lzw(r, idx, n);
            // Clipped to the picture, the area that a later "restore to previous" puts back.
            int cx = SDL_max(0, x), cy = SDL_max(0, y), cw = SDL_min(r->w, x + w) - cx, ch = SDL_min(r->h, y + h) - cy;
            if (cw < 0) cw = 0;
            if (ch < 0) ch = 0;
            if (dispose == 3)
                for (int yy = cy; yy < cy + ch; yy++) SDL_memcpy(r->saved + ((size_t)yy * r->w + cx) * 4, r->canvas + ((size_t)yy * r->w + cx) * 4, (size_t)cw * 4);
            // Its rows, in the order interlacing stores them: every 8th from 0, every 8th from 4, every 4th from 2, every 2nd from 1.
            static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
            int row = 0;
            for (int pass = 0; pass < (flags & 0x40 ? 4 : 1); pass++)
                for (int yy = flags & 0x40 ? start[pass] : 0; yy < h; yy += flags & 0x40 ? step[pass] : 1, row++) {
                    int py = y + yy;
                    if (py < 0 || py >= r->h) continue;
                    const uint8_t *src = idx + (size_t)row * w;
                    for (int xx = 0; xx < w; xx++) {
                        int px = x + xx, c = src[xx];
                        if (px < 0 || px >= r->w || c == transparent || c >= colours) continue;
                        uint8_t *o = r->canvas + ((size_t)py * r->w + px) * 4;
                        o[0] = table[c][0], o[1] = table[c][1], o[2] = table[c][2], o[3] = 255;
                    }
                }
            if (idx != r->index) SDL_free(idx);
            r->dispose = dispose, r->fx = cx, r->fy = cy, r->fw = cw, r->fh = ch;
            *delay_ms = delay;
            (void)whole;  // (a frame that breaks off shows what was decoded of it, as browsers do)
            return r->canvas;
        } else return NULL;  // the trailer (0x3b), or the end of the bytes
    }
}

enum { CATCH_UP = 1000 };  // further behind than this, it skips to now rather than racing to catch up

struct gs_gif {
    const void *data;
    size_t len;
    SDL_Renderer *r;
    SDL_Texture *first, *tex;  // the first frame, kept; the frame shown while it plays
    int w, h, first_delay, delay;  // the delays of the first frame and of the one shown, in ms
    gs_gif_reader *reader;
    bool playing, showing_first, animated;
    Uint64 due;  // when the frame after the one shown is due
};

// How long a frame stays, as browsers show it: a delay under 20 ms (0 in most files that give none) is 100 ms.
static int shown_for(int delay) { return delay < 20 ? 100 : delay; }

gs_gif *gs_gif_open(const void *data, size_t len, SDL_Renderer *r) {
    gs_gif_reader *reader = gs_gif_reader_new(data, len);
    int delay = 0, w = reader ? reader->w : 0, h = reader ? reader->h : 0;
    const uint8_t *px = reader ? gs_gif_reader_next(reader, &delay) : NULL;
    SDL_Texture *first = px ? SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h) : NULL;
    gs_gif *g = first ? SDL_calloc(1, sizeof *g) : NULL;
    if (!g) {
        SDL_DestroyTexture(first), gs_gif_reader_free(reader);
        return NULL;
    }
    SDL_UpdateTexture(first, NULL, px, w * 4);
    SDL_SetTextureBlendMode(first, SDL_BLENDMODE_BLEND);
    // Whether there is a second frame, and the decoder let go until it plays.
    int d2;
    g->animated = gs_gif_reader_next(reader, &d2) != NULL;
    gs_gif_reader_free(reader);
    *g = (gs_gif){ .data = data, .len = len, .r = r, .first = first, .w = w, .h = h, .first_delay = shown_for(delay), .showing_first = true, .animated = g->animated };
    return g;
}

void gs_gif_free(gs_gif *g) {
    if (!g) return;
    SDL_DestroyTexture(g->first), SDL_DestroyTexture(g->tex), gs_gif_reader_free(g->reader);
    SDL_free(g);
}

void gs_gif_size(const gs_gif *g, int *width, int *height) {
    if (width) *width = g->w;
    if (height) *height = g->h;
}

bool gs_gif_animated(const gs_gif *g) { return g->animated; }

void gs_gif_play(gs_gif *g, bool playing) {
    if (!g->animated || playing == g->playing) return;
    g->playing = playing;
    // Paused, the frame shown stays; played again, it shows for its whole delay from now.
    if (playing) g->due = SDL_GetTicks() + (Uint64)(g->showing_first ? g->first_delay : g->delay);
}

bool gs_gif_playing(const gs_gif *g) { return g->playing; }

void gs_gif_rest(gs_gif *g) {
    g->playing = false, g->showing_first = true;
    gs_gif_reader_free(g->reader), g->reader = NULL;
    SDL_DestroyTexture(g->tex), g->tex = NULL;
}

// Decodes the frame after the one shown into the texture; at the end, from the first again.
static bool advance(gs_gif *g) {
    if (!g->reader) {  // (it played from the first frame, which the reader gives again)
        int d;
        if (!(g->reader = gs_gif_reader_new(g->data, g->len)) || !gs_gif_reader_next(g->reader, &d)) return false;
    }
    if (!g->tex && !(g->tex = SDL_CreateTexture(g->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, g->w, g->h))) return false;
    SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND);
    int delay;
    const uint8_t *px = gs_gif_reader_next(g->reader, &delay);
    if (!px) {  // the end: round to the first frame, which is kept
        gs_gif_reader_restart(g->reader);
        if (!gs_gif_reader_next(g->reader, &delay)) return false;
        g->showing_first = true, g->delay = g->first_delay;
        return true;
    }
    SDL_UpdateTexture(g->tex, NULL, px, g->w * 4);
    g->showing_first = false, g->delay = shown_for(delay);
    return true;
}

SDL_Texture *gs_gif_frame(gs_gif *g) {
    Uint64 now = SDL_GetTicks();
    if (g->playing && now >= g->due) {
        if (now - g->due > CATCH_UP) g->due = now;  // (it was not drawn for a while: on from here)
        while (g->playing && now >= g->due)
            if (advance(g)) g->due += (Uint64)g->delay;
            else g->playing = false;  // (a file that breaks off: it stops on the frame it reached)
    }
    return g->showing_first || !g->tex ? g->first : g->tex;
}

void gs_gif_draw(gs_gif *g, SDL_FRect dst) { SDL_RenderTexture(g->r, gs_gif_frame(g), NULL, &dst); }

int gs_gif_due_ms(const gs_gif *g) {
    if (!g->playing) return -1;
    Uint64 now = SDL_GetTicks();
    return g->due > now ? (int)(g->due - now) : 0;
}
