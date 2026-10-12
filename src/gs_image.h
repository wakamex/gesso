// Images fetched over HTTPS, or handed over by the app, and drawn often: thumbnails, and one at a
// time whole. Each is downloaded once on a
// worker (gs_jobs, gs_http), kept as its compressed file under a byte limit (the least recently drawn
// go first), decoded with stb_image (JPEG and PNG) to the size it is drawn at, and held as a texture
// only while it is drawn: a texture not drawn for a few seconds is freed and made again from the
// compressed copy when needed.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#include "gs_jobs.h"

typedef struct gs_images gs_images;

gs_images *gs_images_new(SDL_Renderer *r, gs_jobs *jobs, size_t cache_bytes);
void gs_images_free(gs_images *im);  // waits for downloads in flight

// The image at url as a w x h pixel texture (scaled to cover, cropping the excess), or NULL while it
// loads or when it failed. Call it each frame for each image drawn, on the rendering thread.
SDL_Texture *gs_images_get(gs_images *im, const char *url, int w, int h);
// The image's own size in pixels, the right way up, once it has loaded (false until then): for
// laying it out at its own shape before drawing it. A JPEG that says how it is to be turned (a
// phone's photo) is drawn turned, here and in the calls above and below.
bool gs_images_size(gs_images *im, const char *url, int *w, int *h);
// The image whole: a texture of its own size (shrunk only when it is larger than the renderer's
// largest texture, or 8192 pixels), for a view of it alone, such as gs_ui_image_view. It is decoded
// on a worker, so this returns NULL for the frames that takes (gs_images_end_frame says when to
// draw again), and the texture is freed once it has not been asked for in a few seconds. *w and *h
// get the texture's size.
SDL_Texture *gs_images_full(gs_images *im, const char *url, int *w, int *h);
// Frees textures not drawn lately; call once a frame. True when an image finished loading since the
// last call, so an app that draws only on change knows to draw again.
bool gs_images_end_frame(gs_images *im);
size_t gs_images_bytes(const gs_images *im);  // compressed bytes held
// Adds an image the app already has (a compressed JPEG or PNG file in memory) under a name used as
// its url, as if downloaded: for bundled or generated images. False when the cache is full.
bool gs_images_put(gs_images *im, const char *url, const void *data, size_t len);
