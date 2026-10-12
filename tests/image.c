// gs_image: an image handed over, its size, a thumbnail and the whole of it, a photo that says how
// it is to be turned; and gs_ui's view of one, zoomed and moved.
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "gs_image.h"
#include "gs_ui.h"
#include "stb_image_write.h"
#include "test.h"

typedef struct { uint8_t *p; size_t n; } bytes;

static void append(void *user, void *data, int size) {
    bytes *b = user;
    b->p = realloc(b->p, b->n + (size_t)size);
    memcpy(b->p + b->n, data, (size_t)size), b->n += (size_t)size;
}

static float channel(SDL_Surface *s, int x, int y, int which) {
    Uint8 c[4];
    return SDL_ReadSurfacePixel(s, x, y, &c[0], &c[1], &c[2], &c[3]) ? c[which] / 255.0f : -1;
}

void test_image(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO)) return;
    SDL_PropertiesID p = SDL_CreateProperties();  // (as tests/ui.c makes its window, for macOS)
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 200);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 200);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    SDL_Window *win = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    SDL_Renderer *r = win ? SDL_CreateRenderer(win, NULL) : NULL;
    CHECK(r);
    if (!r) return SDL_Quit();
    gs_jobs *jobs = gs_jobs_new(1);
    gs_images *im = gs_images_new(r, jobs, 8 << 20);

    // A picture 40 by 20: its left half red, its right half blue.
    uint8_t px[40 * 20 * 4];
    for (int i = 0; i < 40 * 20; i++) px[4 * i] = i % 40 < 20 ? 255 : 0, px[4 * i + 1] = 0, px[4 * i + 2] = i % 40 < 20 ? 0 : 255, px[4 * i + 3] = 255;
    bytes png = { 0 }, jpg = { 0 };
    stbi_write_png_to_func(append, &png, 40, 20, 4, px, 160);
    stbi_write_jpg_to_func(append, &jpg, 40, 20, 4, px, 95);
    // The same as a JPEG that says it is to be turned a quarter clockwise (orientation 6), as a phone held upright writes it.
    static const uint8_t exif[] = { 0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 6, 0, 0, 0, 0, 0, 0 };
    bytes turned = { malloc(jpg.n + sizeof exif), jpg.n + sizeof exif };
    memcpy(turned.p, jpg.p, 2), memcpy(turned.p + 2, exif, sizeof exif), memcpy(turned.p + 2 + sizeof exif, jpg.p + 2, jpg.n - 2);
    CHECK(gs_images_put(im, "flat", png.p, png.n) && gs_images_put(im, "turned", turned.p, turned.n));

    int w = 0, h = 0;
    CHECK(gs_images_size(im, "flat", &w, &h) && w == 40 && h == 20);
    CHECK(gs_images_size(im, "turned", &w, &h) && w == 20 && h == 40);  // the right way up
    CHECK(gs_images_get(im, "flat", 10, 10) != NULL);
    CHECK(gs_images_full(im, "turned", &w, &h) == NULL);  // decoded on a worker first
    gs_jobs_wait(jobs);
    CHECK(gs_images_end_frame(im));  // which says to draw again
    SDL_Texture *full = gs_images_full(im, "turned", &w, &h);
    CHECK(full && w == 20 && h == 40);
    SDL_Texture *flat = gs_images_full(im, "flat", NULL, NULL);
    gs_jobs_wait(jobs);
    flat = gs_images_full(im, "flat", &w, &h);
    CHECK(flat && w == 40 && h == 20);

    // Turned, the red half is at the top.
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    if (full) SDL_RenderTexture(r, full, NULL, &(SDL_FRect){ 0, 0, 20, 40 });
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
    if (s && full) CHECK(channel(s, 10, 5, 0) > 0.8f && channel(s, 10, 5, 2) < 0.2f && channel(s, 10, 35, 2) > 0.8f && channel(s, 10, 35, 0) < 0.2f);
    SDL_DestroySurface(s);

    // The view: fitted and never enlarged at first, zoomed by the wheel and the keys, moved by a drag.
    gs_fontset *fs = gs_fontset_new();
    gs_ui *ui = gs_ui_new(win, r, fs);
    gs_rect area = { 0, 0, 200, 200 };
    SDL_Texture *big = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 800, 400);
    #define FRAME(id, t) (gs_ui_begin(ui), gs_ui_image_view(ui, area, id, t), gs_ui_end(ui))
    if (flat && big) {
        FRAME("small", flat);
        CHECK(gs_ui_image_zoom(ui, "small") == 1);  // smaller than the area: at its own size
        FRAME("big", big);
        CHECK_NEAR(gs_ui_image_zoom(ui, "big"), 0.25, 1e-6);  // 800 wide in 200
        SDL_Event move = { .motion = { .type = SDL_EVENT_MOUSE_MOTION, .x = 100, .y = 100 } }, wheel = { .wheel = { .type = SDL_EVENT_MOUSE_WHEEL, .y = 1 } };
        gs_ui_event(ui, &move), gs_ui_event(ui, &wheel), FRAME("big", big);
        CHECK_NEAR(gs_ui_image_zoom(ui, "big"), 0.3, 1e-6);
        SDL_Event wheel_out = { .wheel = { .type = SDL_EVENT_MOUSE_WHEEL, .y = -5 } };
        gs_ui_event(ui, &wheel_out), FRAME("big", big);
        CHECK_NEAR(gs_ui_image_zoom(ui, "big"), 0.25, 1e-6);  // no smaller than fitted
        gs_ui_focus(ui, "big");
        SDL_Event one = { .key = { .type = SDL_EVENT_KEY_DOWN, .key = SDLK_1, .down = true } }, zero = { .key = { .type = SDL_EVENT_KEY_DOWN, .key = SDLK_0, .down = true } };
        gs_ui_event(ui, &one), FRAME("big", big);
        CHECK(gs_ui_image_zoom(ui, "big") == 1);
        gs_ui_event(ui, &zero), FRAME("big", big);
        CHECK_NEAR(gs_ui_image_zoom(ui, "big"), 0.25, 1e-6);
        // A double click goes to its own size, and another back to fitted.
        for (int click = 0; click < 4; click++) {
            SDL_Event down = { .button = { .type = SDL_EVENT_MOUSE_BUTTON_DOWN, .button = SDL_BUTTON_LEFT, .down = true, .x = 100, .y = 100 } }, up = down;
            up.button.type = SDL_EVENT_MOUSE_BUTTON_UP, up.button.down = false;
            gs_ui_event(ui, &down), gs_ui_event(ui, &up), FRAME("big", big);
            if (click == 1) CHECK(gs_ui_image_zoom(ui, "big") == 1);
            if (click == 3) CHECK_NEAR(gs_ui_image_zoom(ui, "big"), 0.25, 1e-6);
        }
    }
    SDL_DestroyTexture(big);
    gs_ui_free(ui), gs_fontset_free(fs);
    gs_images_free(im), gs_jobs_free(jobs);
    free(png.p), free(jpg.p), free(turned.p);
    SDL_DestroyRenderer(r), SDL_DestroyWindow(win);
    SDL_Quit();
}
