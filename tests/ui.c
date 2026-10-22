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

static gs_fontset *fonts;  // the set the interface under test draws with

static void test_ui_multiline(void) {
    gs_ui_begin(ui), gs_ui_end(ui);
    CHECK(gs_ui_text_height(ui, "a\nb\nc", 14, 200) == 3 * gs_ui_line_height(ui, 14));
    CHECK(gs_ui_text_height(ui, "", 14, 200) == gs_ui_line_height(ui, 14));
    // The space between lines is the font's own unless the app says otherwise.
    float ascent, descent, gap;
    if (gs_fontset_vmetrics(fonts, 10, &ascent, &descent, &gap)) CHECK_NEAR(gs_ui_line_height(ui, 10), ascent - descent + gap, 1e-4);
    else CHECK_NEAR(gs_ui_line_height(ui, 10), 14, 1e-4);
    gs_ui_style_of(ui)->line_height = 1.4f;
    float area_before = gs_ui_textarea_height(ui, "a\nb\nc\nd", 200, 8);
    gs_ui_style_of(ui)->line_height = 1.1f;
    CHECK_NEAR(gs_ui_line_height(ui, 10), 11, 1e-4);
    CHECK_NEAR(gs_ui_text_height(ui, "a\nb\nc", 14, 200), 3 * 14 * 1.1, 1e-3);
    CHECK_NEAR(area_before - gs_ui_textarea_height(ui, "a\nb\nc\nd", 200, 8), 4 * 14 * 0.3, 1e-3);
    gs_ui_style_of(ui)->line_height = 0;

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
// A one-line text too long for its rectangle is cut and ends with an ellipsis. Thousands of different
// texts, so that some share the ellipsis's place in the fontset's cache of shaped lines, where taking
// the ellipsis's measure puts the text's own line out: each must still fill most of the room. Freed
// memory is overwritten for the test where the C library can do that (glibc), so that a line read
// after it was put out is read wrong.
#ifdef __GLIBC__
#include <malloc.h>
#endif
static void test_ui_cut(void) {
    if (gs_ui_text_width(ui, "Hello", 14) <= 0) return;  // (no font to shape with)
    int narrow = 0;
#ifdef __GLIBC__
    mallopt(M_PERTURB, 0x5a);
#endif
    gs_ui_begin(ui);
    for (int i = 0; i < 3000; i++) {
        char text[96];
        SDL_snprintf(text, sizeof text, "Conversation %d with a title much too long for the room it has", i);
        float w = gs_ui_text(ui, (gs_rect){ 0, 200, 200, 20 }, text, 14, gs_ui_style_of(ui)->text, -1);
        if (w < 150 || w > 200) narrow++;
    }
    gs_ui_end(ui);
#ifdef __GLIBC__
    mallopt(M_PERTURB, 0);
#endif
    CHECK(narrow == 0);
    if (narrow) fprintf(stderr, "  %d of 3000 cut texts did not fill their room\n", narrow);
}

// A menu hung from something near the bottom of the window opens upward, and is chosen from there.
static int menu_chosen;
static void menu_frame(gs_rect anchor) {
    gs_ui_begin(ui);
    if (gs_ui_menu_begin(ui, "menu", anchor, 120)) {
        for (int i = 1; i <= 3; i++)
            if (gs_ui_menu_item(ui, i == 1 ? "One" : i == 2 ? "Two" : "Three", true)) menu_chosen = i;
        gs_ui_menu_end(ui);
    }
    gs_ui_end(ui);
}

static void test_ui_menu(void) {
    const float row = gs_ui_style_of(ui)->row;
    const gs_rect low = { 10, 270, 80, 20 }, high = { 10, 20, 80, 20 };  // (in a window 300 high)
    gs_ui_menu_toggle(ui, "menu"), menu_frame(low), menu_frame(low);
    menu_chosen = 0, click(20, 270 - 2 - 3 * row + row * 1.5f), menu_frame(low);  // the second of three rows ending just above the anchor
    CHECK(menu_chosen == 2);
    gs_ui_menu_toggle(ui, "menu"), menu_frame(high), menu_frame(high);
    menu_chosen = 0, click(20, 20 + 20 + 2 + row * 2.5f), menu_frame(high);        // with room below, the third row below it
    CHECK(menu_chosen == 3);
}

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
// A window named a place file comes back at the size kept there, and its new size is kept when it
// changes and the interface is freed.
static void test_ui_place(gs_fontset *fs) {
    char path[512];
    SDL_snprintf(path, sizeof path, "./gesso-test-place-%d.txt", (int)SDL_GetTicks());  // (a path, with its slash: here, on every system)
    gs_window_state was = { 40, 50, 333, 222, false, false };
    CHECK(gs_window_state_save(path, &was));
    SDL_Window *win = offscreen_window(200, 100);
    SDL_SetStringProperty(SDL_GetWindowProperties(win), GS_UI_PLACE, path);
    SDL_Renderer *r = SDL_CreateRenderer(win, NULL);
    gs_ui *placed = gs_ui_new(win, r, fs);
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    CHECK(w == 333 && h == 222);
    SDL_SetWindowSize(win, 300, 180), SDL_SyncWindow(win);
    SDL_Event e;
    while (SDL_PollEvent(&e)) gs_ui_event(placed, &e);
    gs_ui_begin(placed), gs_ui_end(placed);
    CHECK(gs_ui_wait_ms(placed) > 0 && gs_ui_wait_ms(placed) <= 500);  // (a frame asked for, to save it once it rests)
    gs_ui_free(placed);
    gs_window_state now = { 0 };
    CHECK(gs_window_state_load(path, &now) && now.w == 300 && now.h == 180);
    SDL_RemovePath(path);
    SDL_DestroyRenderer(r), SDL_DestroyWindow(win);
}

// Zoomed, the interface is larger by that much: the area in points shrinks, a fill covers that many
// more pixels, and a click lands on the control drawn under it.
static void test_ui_zoom(SDL_Renderer *r) {
    gs_ui_set_zoom(ui, 2);
    gs_rect area = gs_ui_begin(ui);
    CHECK(gs_ui_zoom(ui) == 2 && area.w == 200 && area.h == 150 && gs_ui_scale(ui) == 2);
    gs_ui_end(ui);
    click(45, 30);  // (window coordinates: 22.5, 15 in points)
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    gs_ui_begin(ui);
    gs_ui_fill(ui, (gs_rect){ 0, 0, 10, 10 }, (SDL_FColor){ 1, 1, 1, 1 });
    bool hit = gs_ui_clicked(ui, (gs_rect){ 20, 10, 10, 10 }), miss = gs_ui_clicked(ui, (gs_rect){ 40, 25, 10, 10 });
    gs_ui_end(ui);
    CHECK(hit && !miss);
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
    Uint8 in = 0, out = 0, g, b;
    if (s) SDL_ReadSurfacePixel(s, 19, 19, &in, &g, &b, NULL), SDL_ReadSurfacePixel(s, 21, 21, &out, &g, &b, NULL);
    SDL_DestroySurface(s);
    CHECK(in > 200 && out < 50);  // (10 points are 20 pixels)
    gs_ui_set_zoom(ui, 1);
    gs_ui_begin(ui), gs_ui_end(ui);
}

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
// How much of a line of text shows: the sum, over the picture, of how far each pixel is from the
// background toward the text's colour.
static double ink(SDL_Renderer *r, gs_fontset *fs, float gamma, bool light_text) {
    const SDL_FColor dark = { 0.1f, 0.1f, 0.1f, 1 }, white = { 1, 1, 1, 1 };
    gs_glyphs *g = gs_glyphs_new(r, 512);
    gs_glyphs_correction(g, gamma);
    gs_glyphs_behind(g, light_text ? &dark : &white);
    SDL_SetRenderDrawColorFloat(r, light_text ? 0.1f : 1, light_text ? 0.1f : 1, light_text ? 0.1f : 1, 1);
    SDL_RenderClear(r);
    gs_fontset_draw(g, fs, 14, 10.3f, 40, "The quick brown fox", light_text ? white : dark);
    double sum = 0;
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL), *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    for (int y = 0; c && y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            const double v = ((const Uint8 *)c->pixels)[y * c->pitch + x * 4] / 255.0;
            sum += light_text ? (v - 0.1) / 0.9 : (1 - v) / 0.9;
        }
    SDL_DestroySurface(c), SDL_DestroySurface(s);
    gs_glyphs_free(g);
    return sum;
}

// Light text on dark is given the weight blending in the display's encoding takes from it: at the
// default it shows a tenth to a half more than its bare coverage, by how thin the font is. Dark text on light is left alone.
static void test_ui_text_weight(SDL_Renderer *r, gs_fontset *fs) {
    if (gs_fontset_width(fs, 14, "The") <= 0) return;  // (no font to draw with)
    double bare = ink(r, fs, 1, true), corrected = ink(r, fs, 2.2f, true);
    CHECK(bare > 50);
    CHECK(corrected > bare * 1.08 && corrected < bare * 1.5);
    CHECK_NEAR(ink(r, fs, 2.2f, false), ink(r, fs, 1, false), 0.01);
    if (!(corrected > bare * 1.08 && corrected < bare * 1.5)) fprintf(stderr, "  ink %.1f bare, %.1f corrected\n", bare, corrected);
}

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

static char spaced[64];
static void spaces_frame(void) {
    gs_ui_begin(ui);
    gs_ui_textarea(ui, (gs_rect){ 0, 120, 300, 60 }, "area", area, sizeof area, NULL);
    gs_ui_field(ui, (gs_rect){ 0, 200, 300, 30 }, "spaced", spaced, sizeof spaced, NULL);
    gs_ui_end(ui);
}

static int caret(SDL_Window *w) {  // where the cursor was last drawn, as the input method is told
    int x = -1;
    SDL_GetTextInputArea(w, NULL, &x);
    return x;
}

// The cursor stands after spaces at the end of a line: each one typed moves it, a click past them
// and End put it after them, and the next character goes there.
static void test_ui_trailing_spaces(SDL_Window *w) {
    if (gs_ui_text_width(ui, "a b", 14) <= gs_ui_text_width(ui, "ab", 14)) return;  // (no font to shape with)
    area[0] = 0, spaced[0] = 0;
    for (int field = 0; field < 2; field++) {
        const char *buf = field ? spaced : area;
        click(50, field ? 215 : 130), spaces_frame();
        text("ab"), spaces_frame(), spaces_frame();
        int x0 = caret(w);
        text(" "), spaces_frame(), spaces_frame();
        int x1 = caret(w);
        text(" "), spaces_frame(), spaces_frame();
        int x2 = caret(w);
        CHECK(x1 >= x0 + 2 && x2 >= x1 + 2);
        if (!(x1 >= x0 + 2 && x2 >= x1 + 2)) fprintf(stderr, "  %s caret at %d, %d, %d\n", field ? "field" : "area", x0, x1, x2);
        key(SDLK_HOME, 0), spaces_frame();
        click(290, field ? 215 : 130), spaces_frame(), spaces_frame();  // past the end of the line
        CHECK(caret(w) == x2);
        if (caret(w) != x2) fprintf(stderr, "  %s caret after a click at %d, not %d\n", field ? "field" : "area", caret(w), x2);
        text("c"), spaces_frame();
        CHECK(!strcmp(buf, "ab  c"));
    }
    // A line that a newline ends, and one that wraps: the space the wrap falls after leads to the next line.
    click(50, 130), spaces_frame();
    key(SDLK_A, SDL_KMOD_CTRL), text("ab  "), spaces_frame();
    key(SDLK_RETURN, SDL_KMOD_SHIFT), spaces_frame();
    text("cd"), spaces_frame();
    key(SDLK_UP, 0), key(SDLK_END, 0), spaces_frame();
    text("X"), spaces_frame();
    CHECK(!strcmp(area, "ab  X\ncd"));
    key(SDLK_A, SDL_KMOD_CTRL), spaces_frame();
    text("a wide word and then some more words to fill up the line and a bit   over it"), spaces_frame(), spaces_frame();
    key(SDLK_HOME, SDL_KMOD_CTRL), key(SDLK_END, 0), spaces_frame(), spaces_frame();
    int end = caret(w);
    key(SDLK_RIGHT, 0), spaces_frame(), spaces_frame();
    CHECK(end > 100 && caret(w) < 20);  // (from the first line's end to the second's start)
    area[0] = 0;
}

static const char *message = "see https://example.com/page for the notes and the rest of what was said about it yesterday";
static int sel_from, sel_to, link_clicked;
static SDL_Window *select_window;
static float message_height;
static void select_frame(void) {
    const gs_ui_text_range link = { 4, 28, gs_rgba(0x4080ff, 1), true };
    SDL_SetRenderDrawColor(SDL_GetRenderer(select_window), 0, 0, 0, 255);
    SDL_RenderClear(SDL_GetRenderer(select_window));
    gs_ui_begin(ui);
    message_height = gs_ui_text_select(ui, (gs_rect){ 10, 100, 200, 0 }, "message", message, 14, gs_ui_style_of(ui)->text, &link, 1, &sel_from, &sel_to, &link_clicked);
    gs_ui_end(ui);
}

static void pointer(SDL_EventType type, float x, float y) {
    SDL_Event e = { .button = { .type = type, .button = SDL_BUTTON_LEFT, .down = type == SDL_EVENT_MOUSE_BUTTON_DOWN, .x = x, .y = y } };
    if (type == SDL_EVENT_MOUSE_MOTION) e = (SDL_Event){ .motion = { .type = type, .x = x, .y = y } };
    gs_ui_event(ui, &e);
}

// How many pixels of the picture are nearer to colour c than 0.02 a channel.
static int pixels_of(SDL_Renderer *r, SDL_FColor c) {
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL), *p = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    int n = 0;
    for (int y = 0; p && y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            const Uint8 *q = (const Uint8 *)p->pixels + y * p->pitch + x * 4;
            if (fabsf(q[0] / 255.f - c.r) < 0.02f && fabsf(q[1] / 255.f - c.g) < 0.02f && fabsf(q[2] / 255.f - c.b) < 0.02f) n++;
        }
    SDL_DestroySurface(p), SDL_DestroySurface(s);
    return n;
}

// Thirty messages in a flow 100 points tall, each a selectable text in one group, 10 points apart.
static char bodies[30][32];
static int parts[30][2];
static float body_h, row_heights[30];
static gs_ui_rows convo;
static bool began;
static void convo_frame(void) {
    gs_ui_begin(ui);
    gs_rect area = { 0, 100, 300, 100 };
    convo = gs_ui_flow(ui, area, "convo", 30, row_heights);
    gs_ui_select_group(ui, "bodies");
    float y = convo.y;
    for (int i = convo.first; i < convo.end; y += row_heights[i++]) {
        char id[16];
        SDL_snprintf(id, sizeof id, "body-%d", i);
        gs_ui_text_select(ui, (gs_rect){ 0, y, 300, 0 }, id, bodies[i], 14, gs_ui_style_of(ui)->text, NULL, 0, &parts[i][0], &parts[i][1], NULL);
    }
    gs_ui_select_group(ui, NULL);
    gs_ui_list_end(ui);
    began = gs_ui_select_began(ui, "bodies");
    gs_ui_end(ui);
}

static float row_top(int i) {
    float y = convo.y;
    for (int j = convo.first; j < i; j++) y += row_heights[j];
    return y;
}

// A drag across messages selects the rest of the first, all of those between and the start of the
// last; going up it mirrors; held below the flow it scrolls on and takes in the messages it brings.
static void test_ui_select_across(void) {
    if (gs_ui_text_width(ui, "see", 14) <= 0) return;  // (no font to shape with)
    body_h = gs_ui_line_height(ui, 14);
    for (int i = 0; i < 30; i++) SDL_snprintf(bodies[i], sizeof bodies[i], "message %d says hello", i), row_heights[i] = body_h + 10;
    convo_frame(), convo_frame();
    int first = convo.first + 1, len = (int)strlen(bodies[first]);
    float mid = gs_ui_text_width(ui, "message", 14) + 1;  // (just after "message")
    pointer(SDL_EVENT_MOUSE_MOTION, mid, row_top(first) + body_h / 2), pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, mid, row_top(first) + body_h / 2), convo_frame();
    CHECK(began && parts[first][0] == 7 && parts[first][1] == 7);
    pointer(SDL_EVENT_MOUSE_MOTION, mid, row_top(first + 2) + body_h / 2), convo_frame();
    CHECK(!began);
    CHECK(parts[first][0] == 7 && parts[first][1] == len);
    CHECK(SDL_min(parts[first + 1][0], parts[first + 1][1]) == 0 && SDL_max(parts[first + 1][0], parts[first + 1][1]) == (int)strlen(bodies[first + 1]));
    CHECK(parts[first + 2][0] == 0 && parts[first + 2][1] == 7);
    pointer(SDL_EVENT_MOUSE_MOTION, mid, row_top(first + 2) - 4), convo_frame();  // in the gap above the third: the second to its end, the third nothing
    CHECK(parts[first + 2][0] == parts[first + 2][1] && SDL_max(parts[first + 1][0], parts[first + 1][1]) == (int)strlen(bodies[first + 1]));
    pointer(SDL_EVENT_MOUSE_MOTION, mid, row_top(first) - 4), convo_frame();  // above the first message: its start, back to the press
    CHECK(parts[first][0] == 7 && parts[first][1] == 0 && parts[first + 1][0] == parts[first + 1][1]);
    // Held above the flow (which starts at its bottom), it scrolls and takes in what comes down.
    pointer(SDL_EVENT_MOUSE_MOTION, mid, 80), convo_frame();
    int was = convo.first;
    for (int i = 0; i < 40; i++) SDL_Delay(10), convo_frame();
    CHECK(convo.first < was);
    CHECK(parts[convo.first][0] == (int)strlen(bodies[convo.first]) && parts[convo.first][1] == 0 && parts[first][0] == 7 && parts[first][1] == 0);
    pointer(SDL_EVENT_MOUSE_BUTTON_UP, mid, 80), convo_frame();
}

// Wrapped text the pointer selects in: a drag reports the bytes from where it began to where it is
// and they are drawn highlighted, and a click on a range marked a link reports that range.
static void test_ui_text_select(SDL_Renderer *r) {
    if (gs_ui_text_width(ui, "see", 14) <= 0) return;  // (no font to shape with)
    select_window = SDL_GetRenderWindow(r);
    gs_ui_style_of(ui)->border = gs_rgba(0xff00ff, 1);  // (a colour nothing else in the picture has)
    float lh = gs_ui_line_height(ui, 14), see = gs_ui_text_width(ui, "see ", 14), line = gs_ui_text_width(ui, "see https://example.com/page", 14);
    SDL_FColor back = gs_ui_style_of(ui)->border;
    select_frame();
    CHECK(message_height >= 3 * lh && pixels_of(r, back) == 0);
    // A drag from the start of the first line to the middle of the second.
    pointer(SDL_EVENT_MOUSE_MOTION, 10, 100 + lh / 2), pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, 10, 100 + lh / 2), select_frame();
    CHECK(sel_from == 0 && sel_to == 0);
    pointer(SDL_EVENT_MOUSE_MOTION, 10 + see + 1, 100 + lh / 2), select_frame();
    CHECK(sel_from == 0 && sel_to == 4);
    pointer(SDL_EVENT_MOUSE_MOTION, 80, 100 + lh * 1.5f), select_frame();
    int second = sel_to;
    CHECK(sel_from == 0 && second > 28 && second < (int)strlen(message));
    pointer(SDL_EVENT_MOUSE_MOTION, 80, 500), select_frame();  // below the text: the last line
    CHECK(sel_to > second);
    pointer(SDL_EVENT_MOUSE_MOTION, 80, 100 + lh * 1.5f), pointer(SDL_EVENT_MOUSE_BUTTON_UP, 80, 100 + lh * 1.5f), select_frame();
    CHECK(sel_from == 0 && sel_to == second && link_clicked == -1);  // (a drag that ends on nothing clicks nothing)
    CHECK(pixels_of(r, back) > (int)(line * lh * 0.5f));
    pointer(SDL_EVENT_MOUSE_MOTION, 300, 250), select_frame();  // the pointer elsewhere changes nothing
    CHECK(sel_from == 0 && sel_to == second);
    // A click on the link, and one on a plain word.
    click(10 + see + 20, 100 + lh / 2), select_frame();
    CHECK(link_clicked == 0 && sel_from == sel_to && pixels_of(r, back) == 0);
    select_frame();
    CHECK(link_clicked == -1);
    click(10 + see / 2, 100 + lh / 2), select_frame();
    CHECK(link_clicked == -1);
    // A drag that ends on the link it began on selects and does not open it.
    pointer(SDL_EVENT_MOUSE_MOTION, 10 + see + 1, 100 + lh / 2), pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, 10 + see + 1, 100 + lh / 2), select_frame();
    pointer(SDL_EVENT_MOUSE_MOTION, 10 + see + 60, 100 + lh / 2), pointer(SDL_EVENT_MOUSE_BUTTON_UP, 10 + see + 60, 100 + lh / 2), select_frame();
    CHECK(link_clicked == -1 && sel_from == 4 && sel_to > 6 && sel_to < 28);
    // A double click selects the word under the pointer, a drag from it grows by words, and a triple
    // click selects the paragraph (here the whole text, which has no newline).
    SDL_Event two = { .button = { .type = SDL_EVENT_MOUSE_BUTTON_DOWN, .button = SDL_BUTTON_LEFT, .down = true, .clicks = 2, } };
    int notes = (int)(strstr(message, "notes") - message), row = 0;
    gs_wrap *wrapped = gs_fontset_wrap(fonts, 14, message, 200);
    while (row + 1 < wrapped->count && wrapped->lines[row + 1].start <= notes) row++;
    char head[96];
    SDL_snprintf(head, sizeof head, "%.*s", notes + 2 - wrapped->lines[row].start, message + wrapped->lines[row].start);
    float word_x = 10 + gs_ui_text_width(ui, head, 14), word_y = 100 + lh * (row + 0.5f);  // (inside "notes")
    gs_wrap_free(wrapped);
    two.button.x = word_x, two.button.y = word_y;
    pointer(SDL_EVENT_MOUSE_MOTION, word_x, word_y), gs_ui_event(ui, &two), select_frame();
    int lo = SDL_min(sel_from, sel_to), hi = SDL_max(sel_from, sel_to);
    CHECK(hi - lo == 5 && !strncmp(message + lo, "notes", 5));
    if (!(hi - lo == 5 && !strncmp(message + lo, "notes", 5))) fprintf(stderr, "  double click selected '%.*s'\n", hi - lo, message + lo);
    pointer(SDL_EVENT_MOUSE_MOTION, 10 + see + 1, 100 + lh / 2), select_frame();  // back into the address: whole words up to its start
    lo = SDL_min(sel_from, sel_to), hi = SDL_max(sel_from, sel_to);
    CHECK(!strncmp(message + hi - 5, "notes", 5) && (lo == 4 || message[lo - 1] == '/' || message[lo - 1] == '.' || message[lo - 1] == ':'));
    pointer(SDL_EVENT_MOUSE_BUTTON_UP, 10 + see + 1, 100 + lh / 2), select_frame();
    CHECK(link_clicked == -1);
    two.button.clicks = 3;
    gs_ui_event(ui, &two), pointer(SDL_EVENT_MOUSE_BUTTON_UP, word_x, word_y), select_frame();
    CHECK(SDL_min(sel_from, sel_to) == 0 && SDL_max(sel_from, sel_to) == (int)strlen(message));
    // The link is in its own colour.
    CHECK(pixels_of(r, gs_rgba(0x4080ff, 1)) > 0);
}

static void area_frame(void) {
    gs_ui_begin(ui);
    gs_ui_textarea(ui, (gs_rect){ 0, 120, 300, 60 }, "area", area, sizeof area, NULL);
    gs_ui_end(ui);
}

// Undo and redo: typing goes back a word at a time, a paste and the deletion of a selection a step
// each, the cursor and selection with them; a text the app puts in starts a fresh history.
static void test_ui_undo(void) {
    area[0] = 0;
    click(50, 130), area_frame();
    text("hello"), area_frame(), text(" "), area_frame(), text("wor"), area_frame(), text("ld"), area_frame();
    CHECK(!strcmp(area, "hello world"));
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hello "));
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(area[0] == 0);
    key(SDLK_Z, SDL_KMOD_CTRL | SDL_KMOD_SHIFT), area_frame();
    CHECK(!strcmp(area, "hello "));
    key(SDLK_Y, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hello world"));
    key(SDLK_Y, SDL_KMOD_CTRL), area_frame();  // (nothing more to redo)
    CHECK(!strcmp(area, "hello world"));
    // Backspace a character at a time is one step; a selection deleted is one, and comes back selected.
    key(SDLK_BACKSPACE, 0), key(SDLK_BACKSPACE, 0), area_frame(), key(SDLK_BACKSPACE, 0), area_frame();
    CHECK(!strcmp(area, "hello wo"));
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hello world"));
    gs_ui_textarea_select(ui, "area", 0, 5), area_frame();
    key(SDLK_DELETE, 0), area_frame();
    CHECK(!strcmp(area, " world"));
    text("X"), area_frame();
    CHECK(!strcmp(area, "X world"));
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, " world"));
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    int from = -1, to = -1;
    CHECK(!strcmp(area, "hello world") && gs_ui_textarea_selection(ui, "area", &from, &to) && from == 0 && to == 5);
    // A paste is one step.
    SDL_SetClipboardText("pasted text");
    key(SDLK_END, SDL_KMOD_CTRL), key(SDLK_V, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hello worldpasted text"));
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hello world"));
    // An edit the app records is a step: a partial name completed, then taken back, then the typing before.
    area[0] = 0, area_frame();
    text("hi "), area_frame(), text("@Me"), area_frame();
    strcpy(area, "hi @Melissa "), gs_ui_edited(ui, "area", area, 12), area_frame();
    text("x"), area_frame();
    CHECK(!strcmp(area, "hi @Melissa x"));
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hi @Melissa "));
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hi @Me") && gs_ui_textarea_selection(ui, "area", &from, &to) == false && from == 6);
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hi "));
    key(SDLK_Y, SDL_KMOD_CTRL), key(SDLK_Y, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "hi @Melissa "));
    // The app clears the box (a message sent): nothing it held before can be brought back.
    area[0] = 0, area_frame();
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(area[0] == 0);
    strcpy(area, "a draft"), area_frame();  // (a draft put back)
    text("!"), area_frame();
    key(SDLK_Z, SDL_KMOD_CTRL), area_frame(), key(SDLK_Z, SDL_KMOD_CTRL), area_frame();
    CHECK(!strcmp(area, "a draft"));
    area[0] = 0;

    // The single-line field too, and Esc's clearing.
    click(50, 15), frame();
    text("ab"), frame(), text(" "), frame(), text("cd"), frame();
    key(SDLK_ESCAPE, 0), frame();
    CHECK(field[0] == 0);
    key(SDLK_Z, SDL_KMOD_CTRL), frame();
    CHECK(!strcmp(field, "ab cd"));
    key(SDLK_Z, SDL_KMOD_CTRL), frame();
    CHECK(!strcmp(field, "ab "));
    key(SDLK_ESCAPE, 0), frame(), key(SDLK_ESCAPE, 0), frame();
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
    // A field with the keyboard blinks, and asks for no frames while the window lacks the keyboard.
    frame(), frame();
    CHECK(gs_ui_wait_ms(ui) > 0 && gs_ui_wait_ms(ui) <= 530);
    SDL_Event away = { .window = { .type = SDL_EVENT_WINDOW_FOCUS_LOST } }, back = { .window = { .type = SDL_EVENT_WINDOW_FOCUS_GAINED } };
    gs_ui_event(ui, &away), frame(), frame();
    CHECK(gs_ui_wait_ms(ui) == -1 && gs_ui_focused(ui, "field"));
    gs_ui_event(ui, &back), frame();
    CHECK(gs_ui_wait_ms(ui) > 480 && gs_ui_wait_ms(ui) <= 530);  // (the caret shows from now, for a whole blink)
    key(SDLK_ESCAPE, 0), frame();
    CHECK(field[0] == 0);
    // The app selects a field's whole text, as a find shortcut does, and typing replaces it; or places its cursor.
    text("f3b275"), frame();
    gs_ui_textarea_select(ui, "field", 0, (int)strlen(field)), frame();
    text("lorem"), frame();
    CHECK(!strcmp(field, "lorem"));
    gs_ui_textarea_select(ui, "field", 2, 2), frame();
    text("X"), frame();
    CHECK(!strcmp(field, "loXrem"));
    key(SDLK_ESCAPE, 0), frame();

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

    test_ui_menu();
    test_ui_keyboard();
    test_ui_form();
    test_ui_hotkey();
    test_ui_draw(r);
    test_ui_zoom(r);
    test_ui_windows(w, fs);
    test_ui_place(fs);
    gs_ui_free(ui);
    gs_fontset_free(fs);

    // Wrapped text, rows of differing heights and a multi-line field, with the system's fonts where
    // there are some.
    fs = gs_fontset_system();
    ui = gs_ui_new(w, r, fs), fonts = fs;
    test_ui_multiline();
    test_ui_trailing_spaces(w);
    test_ui_undo();
    test_ui_text_select(r);
    test_ui_select_across();
    test_ui_cut();
    test_ui_text_weight(r, fs);
    gs_ui_free(ui);
    gs_fontset_free(fs);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();
}
