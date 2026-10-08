// Small images fetched over HTTPS and drawn often, such as thumbnails. Each is downloaded once on a
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
// Frees textures not drawn lately; call once a frame. True when an image finished loading since the
// last call, so an app that draws only on change knows to draw again.
bool gs_images_end_frame(gs_images *im);
size_t gs_images_bytes(const gs_images *im);  // compressed bytes held
// Adds an image the app already has (a compressed JPEG or PNG file in memory) under a name used as
// its url, as if downloaded: for bundled or generated images. False when the cache is full.
bool gs_images_put(gs_images *im, const char *url, const void *data, size_t len);
