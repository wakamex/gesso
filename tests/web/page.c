// A gesso page for tests/web/check.py: one text field with the keyboard, filling the browser's page,
// drawn every frame and again at once when the page is resized (?watch=0 leaves the redraw out).
#include <SDL3/SDL.h>
#include <emscripten.h>

#include "gs_text.h"
#include "gs_ui.h"

static gs_ui *ui;
static SDL_Renderer *r;
static char field[256];

static void draw(void) {
    SDL_SetRenderDrawColor(r, 20, 40, 60, 255);
    SDL_RenderClear(r);
    gs_ui_begin(ui);
    gs_ui_field(ui, (gs_rect){ 20, 20, 300, 30 }, "field", field, sizeof field, "type here");
    gs_ui_end(ui);
    SDL_RenderPresent(r);
}

static void frame(void *unused) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) gs_ui_event(ui, &e);
    draw();
}

// SDL resizes the canvas inside the browser's resize event, which clears it; the watch runs right
// there, so the page is drawn again before the browser shows the cleared canvas.
static bool SDLCALL redraw(void *unused, SDL_Event *e) {
    if (e->type == SDL_EVENT_WINDOW_RESIZED || e->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) draw();
    return true;
}

int main(void) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");  // (a 2D canvas, whose pixels the check reads back)
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window *w = SDL_CreateWindow("gesso web test", 400, 120, SDL_WINDOW_FILL_DOCUMENT);
    r = SDL_CreateRenderer(w, NULL);
    gs_fontset *fs = gs_fontset_new();
    gs_fontset_add(fs, "/font.ttf");
    ui = gs_ui_new(w, r, fs);
    gs_ui_focus(ui, "field");
    if (emscripten_run_script_int("new URLSearchParams(location.search).get('watch') !== '0'")) SDL_AddEventWatch(redraw, NULL);
    emscripten_run_script("document.title = \"ready\"");
    emscripten_set_main_loop_arg(frame, NULL, 0, true);
    return 0;
}
