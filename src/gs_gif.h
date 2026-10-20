// An animated GIF in memory, played in place: looping at the delays its frames give, as a chat or a
// browser shows one. A GIF of one frame is a still picture.
//
// It is decoded as it plays, a frame at a time onto the frame before (with the disposal, the
// transparency and the local colour tables real files use), so a long one costs two pictures' pixels
// and a texture, not every frame. Opening reads the first frame and keeps only its texture; the
// decoder is made when it plays and let go by gs_gif_rest, so many can sit in a list, each costing a
// texture until it plays. The app owns the bytes, which must outlive it. Not thread-safe: one
// thread, the renderer's, uses it.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

typedef struct gs_gif gs_gif;

// NULL when the bytes are not a GIF that can be read, or the renderer cannot make its texture.
gs_gif *gs_gif_open(const void *data, size_t len, SDL_Renderer *r);
void gs_gif_free(gs_gif *g);
void gs_gif_size(const gs_gif *g, int *width, int *height);
bool gs_gif_animated(const gs_gif *g);  // more than one frame (known once it has played through, or from its second frame on)

void gs_gif_play(gs_gif *g, bool playing);
bool gs_gif_playing(const gs_gif *g);
// Back to its first frame, paused, with its decoder let go: for one scrolled out of view.
void gs_gif_rest(gs_gif *g);

// Draws the frame for this moment into `dst` (pixels), moving on to it if it is due.
void gs_gif_draw(gs_gif *g, SDL_FRect dst);
SDL_Texture *gs_gif_frame(gs_gif *g);  // the same, for an app that draws it itself
// Milliseconds until the next frame is due, for an app that draws only when something changes
// (gs_ui_wake); -1 when nothing will change by itself (paused, or a still picture).
int gs_gif_due_ms(const gs_gif *g);
