// gs_ui driven by synthetic events in an offscreen window: clicks, keys, typed and composed text.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_draw.h"
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

static void wheel(float x, float y, float dy) {
    SDL_Event m = { .motion = { .type = SDL_EVENT_MOUSE_MOTION, .x = x, .y = y } };
    SDL_Event e = { .wheel = { .type = SDL_EVENT_MOUSE_WHEEL, .y = dy } };
    gs_ui_event(ui, &m), gs_ui_event(ui, &e);
}

static char area[200];
static float heights[40];
static int nrows;
static gs_ui_rows shown;
static bool enter;

// A message list over a compose area; the list has 100 points of height and the area sits below it.
static void multiline_frame(void) {
    gs_ui_begin(ui);
    gs_rect list = { 0, 0, 300, 100 };
    shown = gs_ui_flow(ui, list, "flow", nrows, heights);
    gs_ui_list_end(ui);
    gs_ui_textarea(ui, (gs_rect){ 0, 120, 300, gs_ui_textarea_height(ui, area, 300, 4) }, "area", area, sizeof area, "Message");
    enter = gs_ui_key(ui, SDLK_RETURN, 0);
    gs_ui_end(ui);
}

static void test_ui_multiline(void) {
    gs_ui_begin(ui), gs_ui_end(ui);
    CHECK(gs_ui_text_height(ui, "a\nb\nc", 14, 200) == 3 * gs_ui_line_height(ui, 14));
    CHECK(gs_ui_text_height(ui, "", 14, 200) == gs_ui_line_height(ui, 14));

    // Rows of 20 points in a 100 point list stay at the end as rows are added; scrolled off it, they
    // keep their place, and rows added in front are told apart from rows added behind.
    for (int i = 0; i < 40; i++) heights[i] = 20;
    nrows = 20;
    multiline_frame();
    CHECK(shown.below == 0 && shown.above == 300 && shown.end == 20 && shown.first == 15);
    nrows = 21;
    multiline_frame();
    CHECK(shown.below == 0 && shown.above == 320 && shown.end == 21);
    wheel(50, 50, 2), multiline_frame();  // up
    float above = shown.above;
    CHECK(shown.below > 0 && above < 320);
    nrows = 22, multiline_frame();
    CHECK(shown.above == above && shown.below == 20 + (320 - above));
    gs_ui_flow_prepended(ui, "flow", 3), nrows = 25, multiline_frame();
    CHECK(shown.above == above + 60);
    nrows = 2, multiline_frame();  // fewer rows than fill the list sit at its bottom
    CHECK(shown.first == 0 && shown.end == 2 && shown.y == 60);

    // The text area: Shift+Enter breaks a line and Enter is the app's.
    click(50, 130), multiline_frame();
    CHECK(gs_ui_focused(ui, "area"));
    text("ab"), multiline_frame();
    key(SDLK_RETURN, SDL_KMOD_SHIFT), multiline_frame();
    CHECK(!strcmp(area, "ab\n") && !enter);
    text("cd"), key(SDLK_RETURN, 0), multiline_frame();
    CHECK(!strcmp(area, "ab\ncd") && enter);
    CHECK(gs_ui_textarea_height(ui, area, 300, 4) > gs_ui_textarea_height(ui, "ab", 300, 4));
    CHECK(gs_ui_textarea_height(ui, "a\nb\nc\nd\ne\nf", 300, 4) == gs_ui_textarea_height(ui, "a\nb\nc\nd", 300, 4));
    key(SDLK_HOME, 0), key(SDLK_LEFT, 0), multiline_frame();  // to the line's start, then the end of the line before
    text("X"), multiline_frame();
    CHECK(!strcmp(area, "abX\ncd"));
    key(SDLK_END, SDL_KMOD_CTRL), key(SDLK_LEFT, SDL_KMOD_SHIFT), key(SDLK_LEFT, SDL_KMOD_SHIFT), multiline_frame();  // selects "cd"
    text("Z"), multiline_frame();
    CHECK(!strcmp(area, "abX\nZ"));
    key(SDLK_UP, 0), multiline_frame();  // to the line above, which has no glyph widths without fonts
    text("Y"), multiline_frame();
    CHECK(strlen(area) == 6 && strchr(area, 'Y') && strchr(area, '\n'));
    key(SDLK_A, SDL_KMOD_CTRL), text("q"), multiline_frame();
    CHECK(!strcmp(area, "q"));
    SDL_SetClipboardText("one\r\ntwo\tthree");
    char *clip = SDL_GetClipboardText();
    if (clip && !strcmp(clip, "one\r\ntwo\tthree")) {  // (a clipboard that holds text)
        key(SDLK_V, SDL_KMOD_CTRL), multiline_frame();
        CHECK(!strcmp(area, "qone\ntwo three"));
    }
    SDL_free(clip);

    // Glyphs new to the atlas show a frame late, which wait_ms says.
    if (gs_ui_text_width(ui, "Hello", 14) > 0) {
        gs_ui_begin(ui);
        gs_ui_text(ui, (gs_rect){ 0, 200, 100, 20 }, "Hello wrld", 14, gs_ui_style_of(ui)->text, -1);
        gs_ui_end(ui);
        CHECK(gs_ui_wait_ms(ui) == 0);
        gs_ui_begin(ui);
        gs_ui_text(ui, (gs_rect){ 0, 200, 100, 20 }, "Hello wrld", 14, gs_ui_style_of(ui)->text, -1);
        gs_ui_end(ui);
        CHECK(gs_ui_wait_ms(ui) == -1);
    }
}

static bool custom, first, second, space, clipped_hit;
static float level;
static bool gate;

// A second small interface: two buttons with one label, a control of the app's own, a slider, and a
// button that is disabled while `gate` is set.
static void keyboard_frame(void) {
    gs_ui_begin(ui);
    first = gs_ui_button(ui, (gs_rect){ 0, 0, 80, 30 }, "Remove");
    second = gs_ui_button(ui, (gs_rect){ 90, 0, 80, 30 }, "Remove");
    custom = gs_ui_activated(ui, (gs_rect){ 180, 0, 80, 30 }, "custom");
    gs_ui_focus_mark(ui, (gs_rect){ 180, 0, 80, 30 }, "custom", 4);
    gs_ui_slider(ui, (gs_rect){ 0, 40, 200, 30 }, "level", &level, 0, 100);
    gs_ui_disable(ui, gate, true);
    gs_ui_toggle(ui, (gs_rect){ 0, 80, 80, 30 }, "Gated", &on);
    gs_ui_disable(ui, false, false);
    gs_rect window = { 0, 120, 100, 20 };  // a button half cut off by a clip: only what shows can be clicked
    gs_ui_clip(ui, &window);
    clipped_hit = gs_ui_button(ui, (gs_rect){ 0, 120, 100, 40 }, "Clipped");
    gs_ui_clip(ui, NULL);
    space = gs_ui_key(ui, SDLK_SPACE, 0);
    gs_ui_end(ui);
}

static void test_ui_keyboard(void) {
    // Tab walks the controls in the order drawn and Shift+Tab walks back, round and round.
    gs_ui_focus(ui, NULL), frame();
    key(SDLK_TAB, 0), frame();
    CHECK(gs_ui_focused(ui, "field"));
    text("q"), frame();
    CHECK(!strcmp(field, "q"));
    on = false;
    key(SDLK_TAB, 0), frame();
    CHECK(!gs_ui_focused(ui, "field"));
    key(SDLK_SPACE, 0), frame();  // Space presses the toggle the keyboard has reached
    CHECK(on);
    key(SDLK_RETURN, 0), frame();
    CHECK(!on);
    choice = 0;
    key(SDLK_TAB, 0), frame();
    CHECK(gs_ui_focused(ui, "sort"));
    key(SDLK_DOWN, 0), key(SDLK_DOWN, 0), frame();  // the arrows change a dropdown's choice
    CHECK(choice == 2);
    key(SDLK_UP, 0), frame();
    CHECK(choice == 1);
    selected = 0;
    key(SDLK_TAB, 0), frame();
    CHECK(gs_ui_focused(ui, "list"));
    key(SDLK_DOWN, 0), frame();
    CHECK(selected == 1);
    key(SDLK_TAB, 0), frame();
    CHECK(gs_ui_focused(ui, "field"));
    key(SDLK_TAB, SDL_KMOD_SHIFT), frame();
    CHECK(gs_ui_focused(ui, "list"));
    field[0] = 0;

    // Two buttons with one label are two stops; a control of the app's own is a third.
    gs_ui_focus(ui, NULL), keyboard_frame();
    key(SDLK_TAB, 0), keyboard_frame();
    key(SDLK_SPACE, 0), keyboard_frame();
    CHECK(first && !second && !space);
    key(SDLK_TAB, 0), keyboard_frame();
    key(SDLK_RETURN, 0), keyboard_frame();
    CHECK(!first && second);
    key(SDLK_TAB, 0), keyboard_frame();
    CHECK(gs_ui_focused(ui, "custom"));
    key(SDLK_SPACE, 0), keyboard_frame();
    CHECK(custom);
    key(SDLK_TAB, 0), keyboard_frame();
    CHECK(gs_ui_focused(ui, "level"));
    level = 50;
    key(SDLK_RIGHT, 0), keyboard_frame();
    CHECK(level == 55);
    key(SDLK_HOME, 0), keyboard_frame();
    CHECK(level == 0);
    on = false;
    key(SDLK_TAB, 0), key(SDLK_SPACE, 0), keyboard_frame(), keyboard_frame();
    CHECK(on);  // the toggle, while it is enabled

    // Disabled, it takes no click, and Tab passes it by.
    gate = true, on = false;
    keyboard_frame();
    click(10, 95), keyboard_frame();
    CHECK(!on);
    gs_ui_focus(ui, "level"), keyboard_frame();
    key(SDLK_TAB, 0), keyboard_frame();
    key(SDLK_SPACE, 0), keyboard_frame();
    CHECK(!on && clipped_hit);  // Tab went from the slider past the toggle to the button after it
    gate = false;

    // A click hides the mark and gives Space back to the app.
    click(300, 250), keyboard_frame();
    key(SDLK_SPACE, 0), keyboard_frame();
    CHECK(space && !clipped_hit);

    // What a clip cuts off cannot be clicked.
    click(50, 150), keyboard_frame();
    CHECK(!clipped_hit);
    click(50, 130), keyboard_frame();
    CHECK(clipped_hit);
    gs_ui_focus(ui, NULL);
}

static SDL_Window *offscreen_window(int w, int h) {
    // (a window that brings its own graphics context: otherwise macOS's offscreen driver is asked for
    // OpenGL, which it cannot load, and no window is made)
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    SDL_Window *win = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    return win;
}

// Two windows, each with a gs_ui: each takes its own window's events and leaves the other's.
static void test_ui_windows(SDL_Window *first, gs_fontset *fs) {
    SDL_Window *second = offscreen_window(200, 100);
    SDL_Renderer *r2 = second ? SDL_CreateRenderer(second, NULL) : NULL;
    CHECK(r2);
    if (!r2) return SDL_DestroyWindow(second);
    gs_ui *a = ui, *b = gs_ui_new(second, r2, fs);
    bool hit_a = false, hit_b = false, over_b = false;
    for (int pass = 0; pass < 3; pass++) {
        SDL_WindowID to = pass == 0 ? SDL_GetWindowID(first) : SDL_GetWindowID(second);
        SDL_Event m = { .motion = { .type = SDL_EVENT_MOUSE_MOTION, .windowID = to, .x = 20, .y = 10 } };
        SDL_Event d = { .button = { .type = SDL_EVENT_MOUSE_BUTTON_DOWN, .windowID = to, .button = SDL_BUTTON_LEFT, .down = true, .x = 20, .y = 10 } };
        SDL_Event u = { .button = { .type = SDL_EVENT_MOUSE_BUTTON_UP, .windowID = to, .button = SDL_BUTTON_LEFT, .x = 20, .y = 10 } };
        SDL_Event leave = { .window = { .type = SDL_EVENT_WINDOW_MOUSE_LEAVE, .windowID = to } };
        gs_ui_event(a, &m), gs_ui_event(b, &m);
        if (pass < 2) gs_ui_event(a, &d), gs_ui_event(b, &d), gs_ui_event(a, &u), gs_ui_event(b, &u);
        else gs_ui_event(a, &leave), gs_ui_event(b, &leave);
        gs_ui_begin(a);
        hit_a = gs_ui_button(a, (gs_rect){ 0, 0, 100, 30 }, "One");
        gs_ui_end(a);
        gs_ui_begin(b);
        over_b = gs_ui_hovered(b, (gs_rect){ 0, 0, 100, 30 });
        hit_b = gs_ui_button(b, (gs_rect){ 0, 0, 100, 30 }, "Two");
        gs_ui_end(b);
        if (pass == 0) CHECK(hit_a && !hit_b && !over_b);
        if (pass == 1) CHECK(!hit_a && hit_b && over_b);
        if (pass == 2) CHECK(!hit_a && !hit_b && !over_b);  // the pointer left the second window
    }
    gs_ui_free(b);
    SDL_DestroyRenderer(r2);
    SDL_DestroyWindow(second);
}

static float red_at(SDL_Surface *s, int x, int y) {
    Uint8 r, g, b, a;
    return SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a) ? r / 255.0f : -1;
}

// gs_draw's shapes by their pixels, and gs_ui's opacity and eased values.
static void test_ui_draw(SDL_Renderer *r) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_FColor white = { 1, 1, 1, 1 };
    gs_draw_round(r, (SDL_FRect){ 10, 10, 40, 40 }, 12, white);
    gs_draw_outline(r, (SDL_FRect){ 60, 10, 40, 40 }, 8, 3, white);
    gs_draw_circle(r, 130, 30, 10, gs_rgba(0xff0000, 0.5f));
    gs_draw_line(r, 150, 30, 190, 30, 6, white);
    gs_draw_triangle(r, 200, 10, 240, 30, 200, 50, white);
    gs_draw_triangle(r, 250, 10, 250, 50, 290, 30, white);  // (given the other way round)
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
    CHECK(s);
    if (s) {
        CHECK(red_at(s, 30, 30) > 0.95f && red_at(s, 30, 11) > 0.95f && red_at(s, 48, 30) > 0.95f);  // filled to its edges
        CHECK(red_at(s, 10, 10) < 0.05f && red_at(s, 49, 49) < 0.05f && red_at(s, 30, 8) < 0.05f);   // not in its corners, nor outside
        CHECK(red_at(s, 80, 30) < 0.05f && red_at(s, 80, 11) > 0.95f && red_at(s, 61, 30) > 0.95f && red_at(s, 80, 15) < 0.05f);  // a stroke 3 wide
        CHECK_NEAR(red_at(s, 130, 30), 0.5, 0.02);  // blended at half opacity
        CHECK(red_at(s, 170, 30) > 0.95f && red_at(s, 170, 35) < 0.05f && red_at(s, 148, 30) > 0.95f);  // round ends reach past the points
        CHECK(red_at(s, 210, 30) > 0.95f && red_at(s, 260, 30) > 0.95f && red_at(s, 235, 12) < 0.05f);
    }
    SDL_DestroySurface(s);
    CHECK(gs_ease(0) == 0 && gs_ease(1) == 1 && gs_ease(0.5f) > 0.5f && gs_ease(2) == 1);

    gs_ui_begin(ui);
    CHECK(gs_ui_ease(ui, "ease", 10, 1000) == 10);  // at its target when first asked
    gs_ui_end(ui);
    CHECK(gs_ui_wait_ms(ui) == -1);
    gs_ui_begin(ui);
    float v = gs_ui_ease(ui, "ease", 20, 100000);
    gs_ui_end(ui);
    CHECK(v >= 10 && v < 11 && gs_ui_wait_ms(ui) >= 0 && gs_ui_wait_ms(ui) <= 16);  // on its way, and asking for frames
    gs_ui_begin(ui);
    CHECK(gs_ui_ease(ui, "ease", 20, 0) == 20);
    gs_ui_wake(ui, 250);
    gs_ui_end(ui);
    CHECK(gs_ui_wait_ms(ui) == 250);

    // Opacity fades what is drawn, controls included, and a frame starts opaque.
    gs_ui_begin(ui);
    gs_ui_fill(ui, (gs_rect){ 0, 0, 400, 300 }, (SDL_FColor){ 0, 0, 0, 1 });
    gs_ui_opacity(ui, 0.5f);
    gs_ui_fill(ui, (gs_rect){ 0, 0, 20, 20 }, white);
    gs_ui_round(ui, (gs_rect){ 30, 0, 20, 20 }, 5, white);
    gs_ui_end(ui);
    gs_ui_begin(ui);
    gs_ui_fill(ui, (gs_rect){ 60, 0, 20, 20 }, white);
    gs_ui_progress(ui, (gs_rect){ 100, 0, 100, 10 }, 0.5f);
    gs_ui_end(ui);
    s = SDL_RenderReadPixels(r, NULL);
    if (s) {
        CHECK_NEAR(red_at(s, 10, 10), 0.5, 0.02);
        CHECK_NEAR(red_at(s, 40, 10), 0.5, 0.02);
        CHECK(red_at(s, 70, 10) > 0.95f);
        gs_ui_style *st = gs_ui_style_of(ui);
        CHECK_NEAR(red_at(s, 120, 5), st->accent.r, 0.02);  // the filled half of the bar
        CHECK_NEAR(red_at(s, 180, 5), st->raised.r, 0.02);
    }
    SDL_DestroySurface(s);
}

void test_ui(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO)) return;  // (no video at all: nothing to test here)
    SDL_Window *w = offscreen_window(400, 300);
    SDL_Renderer *r = w ? SDL_CreateRenderer(w, NULL) : NULL;
    CHECK(w && r);
    if (!r) return SDL_DestroyWindow(w), SDL_Quit();
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

    test_ui_keyboard();
    test_ui_draw(r);
    test_ui_windows(w, fs);
    gs_ui_free(ui);
    gs_fontset_free(fs);

    // Wrapped text, rows of differing heights and a multi-line field, with the system's fonts where
    // there are some.
    fs = gs_fontset_system();
    ui = gs_ui_new(w, r, fs);
    test_ui_multiline();
    gs_ui_free(ui);
    gs_fontset_free(fs);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
