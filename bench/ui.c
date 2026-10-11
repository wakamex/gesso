// Renders a page of gs_ui's controls and gs_draw's shapes, headless, to a PNG:
//   zig build bench-ui -- OUT.png [radius] [tabs] [ask|tip]
// For checking the controls' look on each system by eye: `radius` rounds their corners (6 unless
// given) and `tabs` presses Tab that many times first, which marks the control it reaches; `ask` shows a question over
// the page and `tip` a tooltip.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "gs_draw.h"
#include "gs_ui.h"
#include "stb_image_write.h"

static char name[64] = "Ada Lovelace", notes[200] = "Wrapped text in a field\nof several lines.";
static bool on = true, off;
static float volume = 0.4f;
static int choice = 1, selected = 2;
static const char *const items[] = { "Small", "Medium", "Large" };

static char key[64] = "sk-1234567890", plain[64] = "Kept on Esc";
static double amount = 12.5;
static bool sw_on = true, sw_off;
static int radio = 1, stops = 2;
static bool ask, tip;

static void form(gs_ui *ui, gs_rect r) {
    gs_ui_style *st = gs_ui_style_of(ui);
    gs_rect row = gs_cut_top(&r, st->row);
    gs_ui_input(ui, gs_cut_left(&row, 150), "key", key, sizeof key, "API key", GS_UI_MASKED);
    gs_cut_left(&row, 10);
    gs_ui_number(ui, gs_cut_left(&row, 70), "amount", &amount, 0, 100, 0.5, 1);
    gs_cut_left(&row, 10);
    gs_ui_switch(ui, (gs_rect){ row.x, row.y + 4, 40, 22 }, "sw on", &sw_on);
    gs_ui_switch(ui, (gs_rect){ row.x + 50, row.y + 4, 40, 22 }, "sw off", &sw_off);
    gs_cut_top(&r, 8);
    row = gs_cut_top(&r, st->row);
    static const char *const names[] = { "Raw", "Clean", "Both" };
    for (int i = 0; i < 3; i++)
        if (gs_ui_radio(ui, gs_cut_left(&row, 80), names[i], names[i], radio == i)) radio = i;
    gs_ui_stops(ui, row, "stops", &stops, 5);
    gs_cut_top(&r, 8);
    row = gs_cut_top(&r, st->row);
    gs_rect link = gs_cut_left(&row, 120);
    gs_ui_link(ui, link, "Read the guide");
    gs_ui_tooltip(ui, link, "A tooltip longer than one line wraps to several, so a line that was cut short can show all of its text when the pointer rests on it.");
    static char hotkey[64] = "Ctrl+Shift+Space";
    gs_ui_hotkey(ui, gs_cut_right(&row, 150), "hotkey", hotkey, "Not set", hotkey, sizeof hotkey);
    gs_cut_right(&row, 10);
    gs_ui_disable(ui, true, true);
    gs_ui_button(ui, gs_cut_right(&row, 80), "Disabled");
    gs_ui_disable(ui, false, false);
    gs_cut_top(&r, 8);
    gs_ui_outline(ui, r, st->radius, 1, st->border);
    gs_rect page = gs_ui_page_begin(ui, gs_inset(r, 1), "page");
    page = (gs_rect){ page.x + 10, page.y + 10, page.w - 30, page.h };
    for (int i = 0; i < 6; i++) {
        char title[32];
        SDL_snprintf(title, sizeof title, "Section %d", i + 1);
        gs_ui_text(ui, gs_cut_top(&page, 24), title, st->text_px, st->text, -1);
        page.y += gs_ui_text_wrapped(ui, page, "A page holds more than its area shows and scrolls by the wheel, the keys and its thumb.", st->small_px, st->muted) + 6;
        gs_ui_input(ui, gs_cut_top(&page, st->row), title, plain, sizeof plain, NULL, GS_UI_KEEP);
        gs_cut_top(&page, 12);
    }
    gs_ui_page_end(ui, page.y);
}

static void page(gs_ui *ui) {
    gs_ui_style *st = gs_ui_style_of(ui);
    gs_rect r = gs_ui_begin(ui);
    gs_ui_fill(ui, r, st->background);
    r = gs_inset(r, 20);
    gs_rect left = gs_cut_left(&r, 360);
    gs_cut_left(&r, 20);

    gs_rect row = gs_cut_top(&left, st->row);
    gs_ui_button(ui, gs_cut_left(&row, 100), "Save");
    gs_cut_left(&row, 10);
    gs_ui_toggle(ui, gs_cut_left(&row, 80), "On", &on);
    gs_ui_toggle(ui, gs_cut_left(&row, 80), "Off", &off);
    gs_cut_top(&left, 10);
    gs_ui_field(ui, gs_cut_top(&left, st->row), "name", name, sizeof name, "Name");
    gs_cut_top(&left, 10);
    gs_ui_slider(ui, gs_cut_top(&left, st->row), "volume", &volume, 0, 1);
    gs_cut_top(&left, 10);
    gs_ui_dropdown(ui, gs_cut_top(&left, st->row), "size", items, 3, &choice);
    gs_cut_top(&left, 10);
    gs_ui_textarea(ui, gs_cut_top(&left, 70), "notes", notes, sizeof notes, "Notes");
    gs_cut_top(&left, 10);
    gs_ui_progress(ui, gs_cut_top(&left, 6), 0.62f);
    gs_cut_top(&left, 10);
    gs_rect list = gs_cut_top(&left, 100);
    gs_ui_rows rows = gs_ui_list(ui, list, "list", 8, 24, &selected);
    for (int i = rows.first; i < rows.end; i++) {
        gs_rect item = { list.x, rows.y + (float)i * 24, list.w, 24 };
        if (i == selected) gs_ui_round(ui, item, st->radius, st->raised);
        char label[32];
        SDL_snprintf(label, sizeof label, "Row %d", i + 1);
        gs_ui_text(ui, gs_inset(item, 6), label, st->text_px, st->text, -1);
    }
    gs_ui_list_end(ui);

    // Shapes: fills and outlines at several radii, opacity, and strokes.
    for (int i = 0; i < 5; i++) {
        gs_rect box = { r.x + (float)i * 70, r.y, 60, 40 };
        gs_ui_round(ui, box, (float)i * 5, st->accent);
        box.y += 50;
        gs_ui_outline(ui, box, (float)i * 5, 1 + (float)i, st->text);
        gs_ui_opacity(ui, 1 - (float)i * 0.2f);
        gs_ui_circle(ui, box.x + 30, box.y + 80, 20, st->warning);
        gs_ui_opacity(ui, 1);
    }
    SDL_Renderer *ren = SDL_GetRenderer(SDL_GetWindows(NULL)[0]);
    float s = gs_ui_scale(ui);
    for (int i = 0; i < 8; i++) gs_draw_line(ren, (r.x + 10 + (float)i * 40) * s, (r.y + 180) * s, (r.x + 40 + (float)i * 40) * s, (r.y + 180 + (float)i * 6) * s, (1 + (float)i) * s, st->text);
    gs_draw_triangle(ren, (r.x + 10) * s, (r.y + 250) * s, (r.x + 60) * s, (r.y + 275) * s, (r.x + 10) * s, (r.y + 300) * s, st->accent);
    r.y += 320, r.h -= 320;
    form(ui, r);
    if (ask) gs_ui_confirm(ui, "Remove this model? Its file is deleted from the disk, and this can't be undone.", "Remove", "Cancel", true);
    gs_ui_end(ui);
}

int main(int argc, char **argv) {
    if (argc < 2) return fprintf(stderr, "usage: bench-ui OUT.png [radius] [tabs] [ask|tip]\n"), 1;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "%s\n", SDL_GetError()), 1;
    SDL_Window *w = SDL_CreateWindow("bench-ui", 800, 640, 0);
    SDL_Renderer *r = SDL_CreateRenderer(w, NULL);
    gs_fontset *fs = gs_fontset_system();
    gs_ui *ui = gs_ui_new(w, r, fs);
    gs_ui_style_of(ui)->radius = argc > 2 ? (float)SDL_atof(argv[2]) : 6;
    page(ui);
    for (int i = 0; i < (argc > 3 ? SDL_atoi(argv[3]) : 0); i++) {  // Tab pressed so many times, to see a control's focus mark
        SDL_Event e = { .key = { .type = SDL_EVENT_KEY_DOWN, .key = SDLK_TAB, .down = true } };
        gs_ui_event(ui, &e);
        page(ui);
    }
    ask = argc > 4 && !SDL_strcmp(argv[4], "ask"), tip = argc > 4 && !SDL_strcmp(argv[4], "tip");
    if (tip) {  // the pointer rests on the link until its tooltip shows
        SDL_Event e = { .motion = { .type = SDL_EVENT_MOUSE_MOTION, .x = 440, .y = 430 } };
        gs_ui_event(ui, &e);
        page(ui);
        SDL_Delay(600);
    }
    for (int frame = 0; frame < 3; frame++) page(ui);  // (a glyph first drawn in one frame appears in the next)
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL), *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    bool ok = c && stbi_write_png(argv[1], c->w, c->h, 4, c->pixels, c->pitch);
    SDL_DestroySurface(c), SDL_DestroySurface(s);
    gs_ui_free(ui), gs_fontset_free(fs);
    SDL_Quit();
    return ok ? 0 : 1;
}
