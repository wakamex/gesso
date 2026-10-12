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
    // A row asked for is brought to the middle, and the list then stays put as rows are added.
    for (int i = 0; i < 40; i++) heights[i] = 20;
    nrows = 30;
    gs_ui_flow_show(ui, "flow", 10), multiline_frame();
    CHECK(shown.above == 160 && shown.first == 8 && shown.below == 340);  // row 10 spans 200 to 220, centred in 100 points
    nrows = 31, multiline_frame();
    CHECK(shown.above == 160 && shown.below == 360);
    gs_ui_flow_show(ui, "flow", 0), multiline_frame();  // near an end, as far as it goes
    CHECK(shown.above == 0 && shown.first == 0);
    gs_ui_flow_show(ui, "flow", 30), multiline_frame();  // the last row: at the end again, and following it
    CHECK(shown.below == 0);
    nrows = 32, multiline_frame();
    CHECK(shown.below == 0 && shown.end == 32);
    heights[0] = 20, heights[1] = 60, heights[2] = 20, nrows = 3;
    gs_ui_flow_show(ui, "fresh flow", 1);  // a list never drawn before, with rows of differing heights
    gs_ui_begin(ui);
    gs_ui_rows fresh = gs_ui_flow(ui, (gs_rect){ 0, 0, 300, 40 }, "fresh flow", 3, heights);
    gs_ui_list_end(ui);
    gs_ui_end(ui);
    CHECK(fresh.above == 30 && fresh.first == 1 && fresh.below == 30);  // row 1 spans 20 to 80: its middle, 50, at the middle of 40 points
    for (int i = 0; i < 40; i++) heights[i] = 20;
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
    // What is selected can be read, and set.
    int from = -1, to = -1;
    CHECK(!gs_ui_textarea_selection(ui, "area", &from, &to) && from == to);
    key(SDLK_HOME, SDL_KMOD_CTRL), key(SDLK_RIGHT, SDL_KMOD_SHIFT), key(SDLK_RIGHT, SDL_KMOD_SHIFT), multiline_frame();
    CHECK(gs_ui_textarea_selection(ui, "area", &from, &to) && from == 0 && to == 2);
    gs_ui_textarea_select(ui, "area", 1, 3), multiline_frame();
    CHECK(gs_ui_textarea_selection(ui, "area", &from, &to) && from == 1 && to == 3);
    char kept[sizeof area];
    strcpy(kept, area);
    text("__"), multiline_frame();  // typing replaces the range that was set
    CHECK(strlen(area) == strlen(kept) && area[0] == kept[0] && area[1] == '_' && area[2] == '_' && !strcmp(area + 3, kept + 3));
    gs_ui_textarea_select(ui, "area", 0, 0), multiline_frame();
    CHECK(!gs_ui_textarea_selection(ui, "area", &from, &to) && from == 0);
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

static char secret[32], kept[32];
static char note[200];
static bool wheel_note;
static double amount;
static bool sw, radio_hit, link_hit, deep_hit, under_hit, asking;
static int radio = 0, stop_value, answer;
static float page_scroll;

// A form: a masked field, a field that keeps its text, a number, a switch, two radio buttons, a
// slider with stops and a link, then a page 100 points tall holding 600 points with a button near
// its end, and a question over it all while `asking`.
static void form_frame(void) {
    gs_ui_begin(ui);
    gs_ui_input(ui, (gs_rect){ 0, 0, 100, 30 }, "secret", secret, sizeof secret, "Key", GS_UI_MASKED);
    gs_ui_input(ui, (gs_rect){ 110, 0, 100, 30 }, "kept", kept, sizeof kept, NULL, GS_UI_KEEP);
    gs_ui_number(ui, (gs_rect){ 220, 0, 80, 30 }, "amount", &amount, 0, 100, 5, 0);
    gs_ui_switch(ui, (gs_rect){ 0, 40, 40, 22 }, "switch", &sw);
    radio_hit = false;
    for (int i = 0; i < 2; i++)
        if (gs_ui_radio(ui, (gs_rect){ 50 + 80.0f * i, 40, 70, 22 }, i ? "radio b" : "radio a", i ? "B" : "A", radio == i)) radio = i, radio_hit = true;
    gs_ui_stops(ui, (gs_rect){ 220, 40, 118, 22 }, "stops", &stop_value, 5);
    link_hit = gs_ui_link(ui, (gs_rect){ 0, 70, 200, 20 }, "Read more");
    under_hit = gs_ui_button(ui, (gs_rect){ 300, 250, 90, 30 }, "Under");
    gs_rect area = { 0, 100, 300, 100 };
    gs_rect page = gs_ui_page_begin(ui, area, "page");
    if (wheel_note) gs_ui_textarea(ui, (gs_rect){ page.x, page.y + 10, 200, 60 }, "note", note, sizeof note, NULL);  // (fixed under the pointer at 50, 130 while the page is at its top)
    gs_cut_top(&page, 500);
    deep_hit = gs_ui_button(ui, gs_cut_top(&page, 30), "Deep");
    gs_cut_top(&page, 70);
    gs_ui_page_end(ui, page.y);
    page_scroll = gs_ui_page_scroll(ui, "page");
    answer = asking ? gs_ui_confirm(ui, "Remove it?", "Remove", "Cancel", true) : 0;
    if (answer) asking = false;
    gs_ui_end(ui);
}

static void test_ui_form(void) {
    gs_ui_focus(ui, NULL), form_frame();
    // A masked field edits as any field does and does not copy out.
    click(50, 15), form_frame();
    text("abc"), form_frame();
    CHECK(!strcmp(secret, "abc"));
    key(SDLK_LEFT, 0), key(SDLK_BACKSPACE, 0), form_frame();
    CHECK(!strcmp(secret, "ac"));
    SDL_SetClipboardText("before");
    char *clip = SDL_GetClipboardText();
    if (clip && !strcmp(clip, "before")) {  // (a clipboard that holds text)
        key(SDLK_A, SDL_KMOD_CTRL), key(SDLK_C, SDL_KMOD_CTRL), form_frame();
        SDL_free(clip), clip = SDL_GetClipboardText();
        CHECK(clip && !strcmp(clip, "before"));
    }
    SDL_free(clip);
    // Esc in a field that keeps its text lets go of the keyboard.
    click(150, 15), form_frame();
    text("stay"), form_frame();
    key(SDLK_ESCAPE, 0), form_frame();
    CHECK(!strcmp(kept, "stay") && !gs_ui_focused(ui, "kept"));

    // A number: typing replaces it, letters are left out, Up and Down step it, and it stays in range.
    amount = 10;
    click(250, 15), form_frame(), form_frame();
    text("4x2"), form_frame();
    CHECK(amount == 42);
    key(SDLK_UP, 0), form_frame();
    CHECK(amount == 47);
    key(SDLK_DOWN, 0), key(SDLK_DOWN, 0), form_frame();
    CHECK(amount == 37);
    key(SDLK_A, SDL_KMOD_CTRL), text("250"), form_frame();
    CHECK(amount == 100);
    key(SDLK_A, SDL_KMOD_CTRL), text("-3"), form_frame();
    CHECK(amount == 0);

    click(20, 50), form_frame();
    CHECK(sw);
    click(140, 50), form_frame();
    CHECK(radio == 1 && radio_hit);
    click(140, 50), form_frame();
    CHECK(!radio_hit);  // already chosen
    click(220 + 9 + 75, 50), form_frame();  // three quarters along a slider of five positions
    CHECK(stop_value == 3);
    gs_ui_focus(ui, "stops"), key(SDLK_TAB, SDL_KMOD_SHIFT), key(SDLK_TAB, 0), form_frame();  // (marked by the keyboard)
    key(SDLK_LEFT, 0), form_frame();
    CHECK(stop_value == 2);
    click(10, 80), form_frame();
    CHECK(link_hit);

    // The page: scrolled by the wheel, by its own call and by Tab reaching a control out of view; what
    // it cuts off cannot be clicked.
    CHECK(page_scroll == 0);
    click(50, 210), form_frame();  // below the page, where its far button would be were it not cut off
    CHECK(!deep_hit);
    wheel(50, 150, -1), form_frame(), form_frame();
    CHECK(page_scroll == 48);
    gs_ui_page_scroll_to(ui, "page", 10000, false), form_frame(), form_frame();
    CHECK(page_scroll == 500);  // 600 points of content in 100
    gs_ui_page_scroll_to(ui, "page", 0, false), form_frame();
    gs_ui_focus(ui, "page-before"), key(SDLK_TAB, SDL_KMOD_SHIFT), form_frame(), form_frame();  // back from the first stop to the last: the far button
    CHECK(page_scroll > 400);
    key(SDLK_SPACE, 0), form_frame();
    CHECK(deep_hit);

    // A text area in the page takes the wheel while it has further to scroll, and then the page does.
    gs_rect where;
    CHECK(gs_ui_where(ui, "Deep", &where) && where.w == 300 && where.h == 30);  // a button, by its label
    CHECK(!gs_ui_where(ui, "No such control", &where));
    gs_ui_page_scroll_to(ui, "page", 0, false), wheel_note = true, form_frame();
    SDL_strlcpy(note, "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight", sizeof note);
    form_frame();
    for (int i = 0; i < 12; i++) wheel(50, 130, -1), form_frame();
    form_frame();
    CHECK(page_scroll > 0);  // the note reached its end and the page went on
    float reached = page_scroll;
    SDL_strlcpy(note, "short", sizeof note), form_frame();
    wheel(50, 130, -1), form_frame(), form_frame();
    CHECK(page_scroll == reached + 48 || page_scroll == 500);  // a note with nothing to scroll leaves the wheel alone
    wheel_note = false, gs_ui_page_scroll_to(ui, "page", 0, false), form_frame();

    // A question: what is under it takes nothing, Enter declines, and Tab then Enter accepts.
    asking = true, form_frame();
    click(340, 265), form_frame();
    CHECK(!under_hit && asking);
    CHECK(gs_ui_where(ui, GS_UI_CONFIRM_YES, &where) && gs_ui_where(ui, GS_UI_CONFIRM_NO, &where) && !gs_ui_where(ui, "Under", &where));  // its buttons can be found; what is under it cannot
    key(SDLK_RETURN, 0), form_frame();
    CHECK(answer == -1 && !asking);
    form_frame();
    click(340, 265), form_frame();
    CHECK(under_hit);  // live again
    asking = true, form_frame(), form_frame();
    key(SDLK_TAB, 0), form_frame();
    key(SDLK_RETURN, 0), form_frame();
    CHECK(answer == 1);
    asking = true, form_frame(), form_frame();
    key(SDLK_ESCAPE, 0), form_frame();
    CHECK(answer == -1);
    form_frame();
    gs_ui_focus(ui, NULL);
}

static char hotkey[64];
static bool hotkey_set, capturing;

static void hotkey_frame(void) {
    gs_ui_begin(ui);
    hotkey_set = gs_ui_hotkey(ui, (gs_rect){ 0, 0, 200, 30 }, "hotkey", hotkey, "Not set", hotkey, sizeof hotkey);
    gs_ui_button(ui, (gs_rect){ 0, 40, 80, 30 }, "After");
    capturing = gs_ui_capturing(ui);
    space = gs_ui_key(ui, SDLK_SPACE, 0);
    gs_ui_end(ui);
}

static void scan(SDL_Scancode sc, SDL_Keycode k, SDL_Keymod mod) {
    SDL_Event e = { .key = { .type = SDL_EVENT_KEY_DOWN, .scancode = sc, .key = k, .mod = mod, .down = true } };
    gs_ui_event(ui, &e);
}

static void test_ui_hotkey(void) {
    char a[64];
    CHECK(gs_ui_accelerator(SDL_SCANCODE_F5, SDL_KMOD_LALT, a, sizeof a) && !strcmp(a, "Alt+F5"));
    CHECK(gs_ui_accelerator(SDL_SCANCODE_KP_3, SDL_KMOD_LSHIFT | SDL_KMOD_RALT, a, sizeof a) && !strcmp(a, "Alt+Shift+num3"));
#ifdef __APPLE__
    CHECK(gs_ui_accelerator(SDL_SCANCODE_SPACE, SDL_KMOD_LGUI | SDL_KMOD_LSHIFT, a, sizeof a) && !strcmp(a, "Command+Shift+Space"));
#else
    CHECK(gs_ui_accelerator(SDL_SCANCODE_SPACE, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT, a, sizeof a) && !strcmp(a, "CommandOrControl+Shift+Space"));
#endif
    CHECK(!gs_ui_accelerator(SDL_SCANCODE_A, 0, a, sizeof a) && !gs_ui_accelerator(SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, a, sizeof a));

    // A click starts the wait; a key alone is swallowed, a combination fills the field and ends it.
    gs_ui_focus(ui, NULL), hotkey_frame();
    click(50, 15), hotkey_frame();
    CHECK(capturing && !hotkey_set);
    scan(SDL_SCANCODE_SPACE, SDLK_SPACE, 0), hotkey_frame();
    CHECK(capturing && !space && !hotkey[0]);  // not the app's while the field waits
    scan(SDL_SCANCODE_K, SDLK_K, SDL_KMOD_LALT | SDL_KMOD_LSHIFT), hotkey_frame();
    CHECK(hotkey_set && !strcmp(hotkey, "Alt+Shift+K"));
    hotkey_frame();
    CHECK(!capturing);
    // Esc ends the wait and keeps what was there; so does Tab, which moves on.
    click(50, 15), hotkey_frame();
    scan(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0), hotkey_frame();
    CHECK(!capturing && !strcmp(hotkey, "Alt+Shift+K"));
    click(50, 15), hotkey_frame();
    key(SDLK_TAB, 0), hotkey_frame();
    CHECK(!capturing && !gs_ui_focused(ui, "hotkey"));
    click(50, 15), hotkey_frame();
    click(300, 200), hotkey_frame();  // a click elsewhere
    CHECK(!capturing);
    gs_ui_focus(ui, "hotkey"), key(SDLK_TAB, 0), hotkey_frame();
    // With the keyboard: Shift+Tab back to it, Enter to start, and the window losing the keyboard ends it.
    key(SDLK_TAB, SDL_KMOD_SHIFT), hotkey_frame();
    key(SDLK_RETURN, 0), hotkey_frame();
    CHECK(capturing);
    SDL_Event lost = { .window = { .type = SDL_EVENT_WINDOW_FOCUS_LOST } };
    gs_ui_event(ui, &lost), hotkey_frame();
    CHECK(!capturing);
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

    gs_ui_begin(ui), gs_ui_end(ui);  // (the tests before this one moved the keyboard, which asks for a frame)
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

    // A frame that was handed input asks for one more, and the one after that for none.
    key(SDLK_F5, 0);
    gs_ui_begin(ui), gs_ui_end(ui);
    CHECK(gs_ui_wait_ms(ui) == 0);
    gs_ui_begin(ui), gs_ui_end(ui);
    CHECK(gs_ui_wait_ms(ui) == -1);

    // Giving a control the keyboard asks for a frame too, in which a field starts taking text.
    gs_ui_focus(ui, "somewhere");
    gs_ui_begin(ui), gs_ui_end(ui);
    CHECK(gs_ui_wait_ms(ui) == 0);
    gs_ui_begin(ui), gs_ui_end(ui);
    CHECK(gs_ui_wait_ms(ui) == -1);
    gs_ui_focus(ui, NULL);
    gs_ui_begin(ui), gs_ui_end(ui);

    // A button filled as the main action or as one that cannot be undone, round as a pill; and the frame saved as a picture.
    gs_ui_style *look = gs_ui_style_of(ui);
    look->button_radius = 99;
    gs_ui_begin(ui);
    gs_ui_fill(ui, (gs_rect){ 0, 0, 400, 300 }, (SDL_FColor){ 0, 0, 0, 1 });
    gs_ui_button_primary(ui, (gs_rect){ 0, 0, 100, 30 }, "Go");
    gs_ui_button_danger(ui, (gs_rect){ 110, 0, 100, 30 }, "Erase");
    gs_ui_end(ui);
    char shot[1024];
    SDL_snprintf(shot, sizeof shot, "%sgesso-test-shot.png", SDL_GetBasePath());
    CHECK(gs_draw_save_png(r, shot));
    SDL_Surface *saved = SDL_LoadPNG(shot);
    CHECK(saved && saved->w == 400 && saved->h == 300);
    if (saved) {
        CHECK_NEAR(red_at(saved, 50, 4), look->accent.r, 0.02);    // filled in the accent
        CHECK_NEAR(red_at(saved, 160, 4), look->warning.r, 0.02);  // and in the warning colour
        CHECK(red_at(saved, 1, 1) < 0.05f && red_at(saved, 98, 28) < 0.05f);  // with its corners cut away
    }
    SDL_DestroySurface(saved);
    SDL_RemovePath(shot);
    look->button_radius = -1;

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
    test_ui_form();
    test_ui_hotkey();
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
