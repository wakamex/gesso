// Draws a line of text with a variable font set to a weight and with a static font of that weight,
// and reports how far the two pictures differ: a check of gs_text's variable outlines against an
// instance made by another implementation, such as fontTools' instancer
// (fonttools varLib.instancer FONT.ttf wght=600 -o STATIC.ttf):
//   zig build bench-weight -- FONT.ttf 600 STATIC.ttf [OUT.png] [TEXT]
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "gs_text.h"
#include "stb_image_write.h"

static SDL_Surface *draw(SDL_Renderer *r, const char *path, int weight, const char *text, float *width) {
    gs_fontset *fs = gs_fontset_new();
    gs_fontset_weight(fs, weight);
    gs_fontset_add(fs, path);
    gs_glyphs *g = gs_glyphs_new(r, 1024);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    *width = gs_fontset_draw(g, fs, 48, 10, 70, text, (SDL_FColor){ 1, 1, 1, 1 });
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL), *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    gs_glyphs_free(g), gs_fontset_free(fs);
    return c;
}

int main(int argc, char **argv) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_Init(SDL_INIT_VIDEO);
    // (a window that brings its own graphics context: macOS's offscreen driver cannot load OpenGL for one)
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1100);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 100);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    SDL_Window *w = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_Renderer *r = SDL_CreateRenderer(w, NULL);
    if (argc < 4) return fprintf(stderr, "usage: bench-weight FONT.ttf WEIGHT STATIC.ttf [OUT.png] [TEXT]\n"), 1;
    const char *text = argc > 5 ? argv[5] : "Hamburgefonstiv 0123 \xC3\x85\xC3\xA9\xC3\x9F\xC3\xB1 @&%g;Qj";
    float wa, wb;
    SDL_Surface *a = draw(r, argv[1], atoi(argv[2]), text, &wa), *b = draw(r, argv[3], 0, text, &wb);
    long sum = 0, ink = 0, worst = 0;
    for (int i = 0; i < a->w * a->h; i++) {
        int da = ((unsigned char *)a->pixels)[4 * i], db = ((unsigned char *)b->pixels)[4 * i], d = abs(da - db);
        sum += d, ink += db, worst = d > worst ? d : worst;
    }
    printf("width %.2f against %.2f; difference %.3f%% of the ink, worst pixel %ld of 255\n", wa, wb, 100.0 * sum / (ink ? ink : 1), worst);
    if (argc > 4) stbi_write_png(argv[4], a->w, a->h, 4, a->pixels, a->pitch);
    return 0;
}
