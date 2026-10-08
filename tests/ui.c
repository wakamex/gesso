// gs_ui driven by synthetic events in an offscreen window: clicks, keys, typed and composed text.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_ui.h"
#include "test.h"

static gs_ui *ui;

static void key(SDL_Keycode k, SDL_Keymod mod) {
    SDL_Event e = { .key = { .type = SDL_EVENT_KEY_DOWN, .key = k, .mod = mod, .down = true } };
    gs_ui_event(ui, &e);
}

static void click(float x, float y) {
    SDL_Event m = { .motion = { .type = SDL_EVENT_MOUSE_MOTION, .x = x, .y = y } };
    SDL_Event d = { .button = { .type = SDL_EVENT_MOUSE_BUTTON_DOWN, .button = SDL_BUTTON_LEFT, .down = true, .x = x, .y = y } };
    SDL_Event u = { .button = { .type = SDL_EVENT_MOUSE_BUTTON_UP, .button = SDL_BUTTON_LEFT, .x = x, .y = y } };
    gs_ui_event(ui, &m), gs_ui_event(ui, &d), gs_ui_event(ui, &u);
}

static void text(const char *t) {
    SDL_Event e = { .text = { .type = SDL_EVENT_TEXT_INPUT, .text = t } };
    gs_ui_event(ui, &e);
}

static void compose(const char *t) {
    SDL_Event e = { .edit = { .type = SDL_EVENT_TEXT_EDITING, .text = t } };
    gs_ui_event(ui, &e);
}

static char field[64];
static int selected, choice;
static bool on, activated, changed;
static const char *const items[] = { "Viewers", "Channel", "Category" };

// One frame of a small interface: a field, a toggle, a dropdown and a list of 50 rows.
static void frame(void) {
    gs_rect r = gs_ui_begin(ui);
    gs_rect top = gs_cut_top(&r, 30);
    if (gs_ui_field(ui, gs_cut_left(&top, 200), "field", field, sizeof field, "Search")) changed = true;
    gs_ui_toggle(ui, gs_cut_left(&top, 80), "18+", &on);
    gs_ui_dropdown(ui, gs_cut_left(&top, 120), "sort", items, 3, &choice);
    gs_ui_rows rows = gs_ui_list(ui, r, "list", 50, 20, &selected);
    gs_ui_list_end(ui);
    activated = rows.activated;
    gs_ui_end(ui);
}

void test_ui(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO)) return;  // (no video at all: nothing to test here)
    SDL_Window *w = SDL_CreateWindow("test", 400, 300, 0);
    SDL_Renderer *r = SDL_CreateRenderer(w, NULL);
    gs_fontset *fs = gs_fontset_new();
    ui = gs_ui_new(w, r, fs);
    frame();

    click(50, 15), frame();  // the field takes the keyboard
    CHECK(gs_ui_focused(ui, "field"));
    text("ab"), frame();
    CHECK(!strcmp(field, "ab") && changed);
    key(SDLK_LEFT, 0), text("X"), frame();
    CHECK(!strcmp(field, "aXb"));
    compose("\xE3\x81\x8B"), frame();  // composing in an input method changes nothing until committed
    CHECK(!strcmp(field, "aXb"));
    text("\xE3\x81\x8B"), frame();  // the commit
    CHECK(!strcmp(field, "aX\xE3\x81\x8B" "b"));
    key(SDLK_BACKSPACE, 0), frame();  // removes the whole character before the cursor
    CHECK(!strcmp(field, "aXb"));
    key(SDLK_A, SDL_KMOD_CTRL), text("z"), frame();  // typing over a select-all replaces the text
    CHECK(!strcmp(field, "z"));
    key(SDLK_ESCAPE, 0), frame();
    CHECK(field[0] == 0);

    click(230, 15), frame();  // the toggle
    CHECK(on);
    click(300, 15), frame();  // the dropdown opens below itself...
    click(300, 30 + 2 + 30 + 15), frame();  // ...and its second item is chosen
    CHECK(choice == 1);

    click(100, 30 + 20 * 3 + 5), frame();  // a click on the fourth row selects and activates it
    CHECK(selected == 3 && activated && gs_ui_focused(ui, "list"));
    key(SDLK_DOWN, 0), key(SDLK_DOWN, 0), frame();
    CHECK(selected == 5 && !activated);
    key(SDLK_END, 0), frame();
    CHECK(selected == 49);
    key(SDLK_RETURN, 0), frame();
    CHECK(activated);

    gs_ui_free(ui);
    gs_fontset_free(fs);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
