#include "gs_ui.h"

#include "gs_draw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATES 128     // widgets with state between frames (ids hashed into a small table)
#define FLOATING 64    // rows drawn in the floating layer per frame
#define KEYS 32        // key presses kept per frame
#define STOPS 256      // controls in the Tab order per frame
#define DIM 0.45f      // the opacity of controls drawn disabled and dimmed
#define TOOLTIP_MS 500
#define TOOLTIP_W 280    // the widest a tooltip gets before its text wraps, in points
#define BLINK_MS 530
#define OUTSIDE -1e6f  // where the pointer is once it has left the window

// ---- Rectangles ----

gs_rect gs_cut_left(gs_rect *r, float w) { w = fminf(w, r->w); gs_rect c = { r->x, r->y, w, r->h }; r->x += w, r->w -= w; return c; }
gs_rect gs_cut_right(gs_rect *r, float w) { w = fminf(w, r->w); r->w -= w; return (gs_rect){ r->x + r->w, r->y, w, r->h }; }
gs_rect gs_cut_top(gs_rect *r, float h) { h = fminf(h, r->h); gs_rect c = { r->x, r->y, r->w, h }; r->y += h, r->h -= h; return c; }
gs_rect gs_cut_bottom(gs_rect *r, float h) { h = fminf(h, r->h); r->h -= h; return (gs_rect){ r->x, r->y + r->h, r->w, h }; }
gs_rect gs_inset(gs_rect r, float d) { return (gs_rect){ r.x + d, r.y + d, fmaxf(0, r.w - 2 * d), fmaxf(0, r.h - 2 * d) }; }
bool gs_rect_contains(gs_rect r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// ---- State ----

typedef struct {
    uint32_t id;
    uint64_t frame;   // last drawn
    float scroll;     // a list's offset, in points
    int cursor;       // a field's cursor, a byte offset
    bool open;        // a dropdown or menu
    uint64_t opened;  // the frame it opened in
    bool all;         // a field's whole text is selected
    int anchor;       // a text area's selection runs from here to the cursor...
    bool anchored;    // ...when it has one
    bool away;        // a flow is scrolled off its end
    int prepended;    // rows added in front of a flow, to be told apart from rows added after
    int show;         // the row a flow is to bring into view when next drawn, counted from 1; 0 for none
    float want_x;     // the column a text area's cursor keeps moving up and down along
    bool want;
    float from, to, value;  // an eased value: where it set out from, where it is going, where it is
    uint64_t since;         // when it set out
    bool eased;
    float zoom, cx, cy;     // an image view: screen pixels to an image pixel, and the image's point at the view's centre
    bool fitted;            // ...which stays fitted to its area until the user zooms
    gs_rect view;           // a page's area
    float extent, grab;     // the height of its content; where on its scroll thumb the pointer holds it
} widget;

typedef enum { FL_FILL, FL_TEXT, FL_FRAME, FL_ROUND, FL_OUTLINE } fl_kind;
typedef struct { fl_kind kind; gs_rect r; SDL_FColor c; char text[160]; float px; int align; float radius, width; gs_fontset *fonts; } fl_item;

#define WRAPS 256      // texts kept wrapped

typedef struct { char *text; int size4, width4, max; gs_fontset *fonts; gs_wrap *wrap; } wrap_entry;

typedef struct { uint32_t id, page; gs_rect r; } stop;  // a control the Tab key reaches, and the page it is in
typedef struct { gs_rect r; bool on; } clip_state;

typedef struct {
    float x, y, wheel;
    bool down, pressed, released;
    SDL_Keycode keys[KEYS];
    SDL_Keymod mods[KEYS];
    SDL_Scancode scans[KEYS];
    int nkeys;
    char text[256];
} input;

struct gs_ui {
    SDL_Window *win;
    SDL_Renderer *r;
    gs_glyphs *glyphs;
    gs_fontset *fonts, *own_fonts;  // the fonts text is drawn with now, and the ones it was made with
    gs_ui_style style;
    float scale, opacity;
    gs_rect area;
    uint64_t frame, now;
    input pending, in;
    bool taken[KEYS];
    float press_x, press_y;  // where the left button went down
    uint64_t press_at, press_before;  // when it went down, and the time before that, for a double click
    float before_x, before_y;
    char composition[128];   // the input method's text being composed
    uint32_t focus, drag;    // the widget with the keyboard; the slider being dragged
    bool marked;             // the keyboard moved the focus last, so the control that has it is marked
    stop stops[STOPS], last_stops[STOPS];  // the Tab order: this frame's as it is drawn, and the last frame's, which Tab walks
    int nstops, nlast;
    bool disabled, dim;      // controls drawn now take no input; and are faded
    clip_state clip, list_clip;  // the clip in force, which the pointer cannot reach through; and the one a list put aside
    gs_rect list_area;
    bool list_marked;
    uint32_t page;           // the page being drawn
    clip_state page_clip;
    bool modal_last, modal_now, in_modal;  // a modal was drawn last frame, has been drawn in this one, is being drawn
    uint32_t modal_focus;    // what had the keyboard before it
    uint32_t number;         // the number field being edited, and its text
    char number_text[32];
    uint32_t capture;        // the hotkey field waiting for a key combination
    char captured[64];       // the combination pressed for it
    bool capture_drawn;
    int cursor_want, cursor_now;  // the pointer's shape: 0 the default, 1 a hand, 2 a text beam, 3 the four arrows of something moved
    SDL_Cursor *cursors[4];
    uint64_t caret_since;
    widget states[STATES];
    // The floating layer: what it covered last frame takes the pointer from the widgets under it.
    gs_rect float_last, float_now;
    bool float_last_on, float_now_on, in_float;
    fl_item fl[FLOATING];
    int nfl;
    uint32_t menu;           // the menu being built
    gs_rect menu_rect;
    float menu_y;
    int menu_background;     // its background in the floating layer, sized when the menu ends
    int menu_at, menu_count; // the row the keyboard highlights (from 1), and the rows so far
    bool menu_choose;
    uint32_t tip;            // the tooltip's anchor
    uint64_t tip_since;
    gs_rect tip_rect;
    char tip_text[400];
    bool tip_wanted;
    int wait_ms;
    bool focus_moved;        // the keyboard went to another control since the last frame ended
    bool had_input;          // this frame was handed a press, a release, the wheel, a key or text
    wrap_entry wraps[WRAPS];  // wrapped text kept, so text drawn every frame is broken into lines once
    char *piece;              // a line's text, copied out to be shaped
    size_t piece_cap;
};

static uint32_t hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h ? h : 1;
}

static widget *state(gs_ui *ui, uint32_t id) {
    widget *free_slot = NULL;
    for (uint32_t i = 0; i < STATES; i++) {
        widget *w = &ui->states[(id + i) % STATES];
        if (w->id == id) return w->frame = ui->frame, w;
        if (!free_slot && (!w->id || w->frame + 120 < ui->frame)) free_slot = w;  // empty, or unused for two seconds
    }
    if (!free_slot) free_slot = &ui->states[id % STATES];
    *free_slot = (widget){ .id = id, .frame = ui->frame };
    return free_slot;
}

gs_ui *gs_ui_new(SDL_Window *win, SDL_Renderer *r, gs_fontset *fonts) {
    gs_ui *ui = calloc(1, sizeof *ui);
    if (!ui) return NULL;
    ui->win = win, ui->r = r, ui->fonts = ui->own_fonts = fonts;
    ui->glyphs = gs_glyphs_new(r, 2048);
    ui->style = (gs_ui_style){
        .background = { 0.067f, 0.075f, 0.071f, 1 }, .panel = { 0.098f, 0.110f, 0.106f, 1 }, .raised = { 0.153f, 0.169f, 0.165f, 1 },
        .text = { 0.92f, 0.93f, 0.92f, 1 }, .muted = { 0.59f, 0.61f, 0.60f, 1 }, .accent = { 0.208f, 0.761f, 0.647f, 1 },
        .accent_text = { 0.04f, 0.07f, 0.06f, 1 }, .border = { 0.22f, 0.24f, 0.235f, 1 }, .warning = { 0.95f, 0.60f, 0.29f, 1 },
        .text_px = 14, .small_px = 12, .row = 30,
    };
    ui->style.focus = ui->style.accent;
    ui->opacity = 1;
    ui->pending.x = ui->pending.y = OUTSIDE;  // until the pointer is seen in the window
    return ui;
}

void gs_ui_free(gs_ui *ui) {
    if (!ui) return;
    gs_glyphs_free(ui->glyphs);
    for (int i = 1; i < 4; i++) SDL_DestroyCursor(ui->cursors[i]);
    for (int i = 0; i < WRAPS; i++) SDL_free(ui->wraps[i].text), gs_wrap_free(ui->wraps[i].wrap);
    free(ui->piece);
    free(ui);
}

gs_ui_style *gs_ui_style_of(gs_ui *ui) { return &ui->style; }
gs_glyphs *gs_ui_glyphs(gs_ui *ui) { return ui->glyphs; }
float gs_ui_scale(const gs_ui *ui) { return ui->scale; }
int gs_ui_wait_ms(const gs_ui *ui) { return ui->wait_ms; }

static void wake_within(gs_ui *ui, int ms) {
    if (ui->wait_ms < 0 || ms < ui->wait_ms) ui->wait_ms = ms;
}

void gs_ui_wake(gs_ui *ui, int ms) { wake_within(ui, SDL_max(0, ms)); }

// The window an event is for; 0 for one that names none.
static SDL_WindowID event_window(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: return e->motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return e->button.windowID;
    case SDL_EVENT_MOUSE_WHEEL: return e->wheel.windowID;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.windowID;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID;
    case SDL_EVENT_TEXT_EDITING: return e->edit.windowID;
    default: return e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST ? e->window.windowID : 0;
    }
}

bool gs_ui_event(gs_ui *ui, const SDL_Event *e) {
    input *p = &ui->pending;
    SDL_WindowID to = event_window(e);
    if (to && to != SDL_GetWindowID(ui->win)) return false;  // another window's
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: p->x = e->motion.x, p->y = e->motion.y; return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) p->down = p->pressed = true, p->x = e->button.x, p->y = e->button.y;
        return false;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) p->down = false, p->released = true, p->x = e->button.x, p->y = e->button.y;
        return false;
    case SDL_EVENT_MOUSE_WHEEL: p->wheel += e->wheel.y; return false;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: p->x = p->y = OUTSIDE; return false;  // nothing is under a pointer that has left
    case SDL_EVENT_WINDOW_FOCUS_LOST: ui->composition[0] = 0, ui->capture = 0; return false;  // (keys pressed elsewhere are not a hotkey field's)
    case SDL_EVENT_KEY_DOWN:  // (whether a key is taken is settled while drawing: see gs_ui_key)
        if (p->nkeys < KEYS) p->keys[p->nkeys] = e->key.key, p->scans[p->nkeys] = e->key.scancode, p->mods[p->nkeys++] = e->key.mod;
        return false;
    case SDL_EVENT_TEXT_INPUT:
        SDL_strlcat(p->text, e->text.text, sizeof p->text);
        ui->composition[0] = 0;
        return true;
    case SDL_EVENT_TEXT_EDITING:
        SDL_strlcpy(ui->composition, e->edit.text ? e->edit.text : "", sizeof ui->composition);
        return true;
    default: return false;
    }
}

static void set_focus(gs_ui *ui, uint32_t h) {
    if (h == ui->focus) return;
    ui->focus = h, ui->caret_since = ui->now, ui->focus_moved = true;
    if (SDL_TextInputActive(ui->win)) SDL_StopTextInput(ui->win);
    ui->composition[0] = 0;
}

// Tab and Shift+Tab: the focus goes to the next or the previous of the controls the last frame drew,
// round and round, starting at the first or the last when none of them has it.
static void tab(gs_ui *ui, int step) {
    if (!ui->nlast) return;
    int at = -1;
    for (int i = 0; i < ui->nlast; i++)
        if (ui->last_stops[i].id == ui->focus) at = i;
    at = at < 0 ? (step > 0 ? 0 : ui->nlast - 1) : (at + step + ui->nlast) % ui->nlast;
    const stop *to = &ui->last_stops[at];
    set_focus(ui, to->id);
    ui->marked = true;
    if (to->page) {  // brought into view in its page, with room around it
        widget *page = state(ui, to->page);
        gs_rect v = page->view;
        if (to->r.y < v.y + 12 || to->r.y + to->r.h > v.y + v.h - 12) page->scroll += to->r.y + to->r.h / 2 - (v.y + v.h / 2), page->eased = false;
    }
}

gs_rect gs_ui_begin(gs_ui *ui) {
    int ww, wh, pw, ph;
    SDL_GetWindowSize(ui->win, &ww, &wh);
    SDL_GetCurrentRenderOutputSize(ui->r, &pw, &ph);
    ui->scale = ww > 0 ? (float)pw / ww : 1;  // pixels per point, from the window's pixel density
    ui->area = (gs_rect){ 0, 0, (float)ww, (float)wh };
    ui->frame++;
    ui->now = SDL_GetTicks();
    bool down = ui->pending.down;
    ui->in = ui->pending;
    ui->pending = (input){ .x = ui->in.x, .y = ui->in.y, .down = down };
    ui->had_input = ui->in.pressed || ui->in.released || ui->in.wheel != 0 || ui->in.nkeys || ui->in.text[0];
    if (ui->in.pressed) ui->before_x = ui->press_x, ui->before_y = ui->press_y, ui->press_before = ui->press_at, ui->press_x = ui->in.x, ui->press_y = ui->in.y, ui->press_at = ui->now;
    memset(ui->taken, 0, sizeof ui->taken);
    ui->float_last = ui->float_now, ui->float_last_on = ui->float_now_on;
    ui->float_now_on = false, ui->nfl = 0, ui->tip_wanted = false, ui->wait_ms = -1, ui->opacity = 1;
    if (!down) ui->drag = 0;
    ui->disabled = ui->dim = false, ui->clip.on = false, ui->page = 0, ui->cursor_want = 0, ui->fonts = ui->own_fonts;
    bool was_modal = ui->modal_last;
    ui->modal_last = ui->modal_now, ui->modal_now = ui->in_modal = false;
    if (was_modal && !ui->modal_last) set_focus(ui, ui->modal_focus);  // the modal is gone: the keyboard goes back
    if (ui->in.pressed) ui->marked = false;
    memcpy(ui->last_stops, ui->stops, sizeof(stop) * (size_t)ui->nstops);
    ui->nlast = ui->nstops, ui->nstops = 0;
    for (int i = 0; i < ui->in.nkeys; i++)
        if (ui->in.keys[i] == SDLK_TAB && !(ui->in.mods[i] & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)))
            ui->taken[i] = true, tab(ui, ui->in.mods[i] & SDL_KMOD_SHIFT ? -1 : 1);
    // A hotkey field that waits for keys has every key left: Esc ends the wait, a combination fills it.
    ui->captured[0] = 0, ui->capture_drawn = false;
    for (int i = 0; ui->capture && i < ui->in.nkeys; i++) {
        if (ui->taken[i]) { ui->capture = 0; break; }  // (Tab has moved on)
        ui->taken[i] = true;
        if (ui->in.keys[i] == SDLK_ESCAPE) ui->capture = 0;
        else if (!ui->captured[0]) gs_ui_accelerator(ui->in.scans[i], ui->in.mods[i], ui->captured, sizeof ui->captured);
    }
    gs_glyphs_begin_frame(ui->glyphs);
    return ui->area;
}

// ---- Drawing ----

static SDL_FRect px(const gs_ui *ui, gs_rect r) { return (SDL_FRect){ r.x * ui->scale, r.y * ui->scale, r.w * ui->scale, r.h * ui->scale }; }

static void fill_now(gs_ui *ui, gs_rect r, SDL_FColor c) {
    SDL_FRect f = px(ui, r);
    SDL_SetRenderDrawBlendMode(ui->r, c.a < 1 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColorFloat(ui->r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ui->r, &f);
}

static void frame_now(gs_ui *ui, gs_rect r, SDL_FColor c) {
    float t = fmaxf(1, floorf(ui->scale));
    SDL_FRect f = px(ui, r);
    SDL_FRect sides[4] = { { f.x, f.y, f.w, t }, { f.x, f.y + f.h - t, f.w, t }, { f.x, f.y, t, f.h }, { f.x + f.w - t, f.y, t, f.h } };
    SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(ui->r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRects(ui->r, sides, 4);
}

float gs_ui_text_width(gs_ui *ui, const char *text, float size) { return gs_fontset_width(ui->fonts, size * ui->scale, text) / ui->scale; }

static float text_now(gs_ui *ui, gs_rect r, const char *text, float size, SDL_FColor c, int align) {
    float s = ui->scale, avail = r.w * s;
    const gs_line *l = gs_fontset_line(ui->fonts, size * s, text);
    if (!l || !l->count) return 0;
    float width = l->width;
    bool rtl = l->rtl;
    char cut[512];
    if (width > avail) {  // the longest start that fits beside an ellipsis
        float ell = gs_fontset_width(ui->fonts, size * s, "\xE2\x80\xA6");
        int keep = 0;
        for (int i = 0; i < l->count; i++) {
            const gs_glyph_pos *g = &l->glyphs[i];
            float edge = rtl ? width - g->x : g->x + g->advance;
            if (edge <= avail - ell && g->cluster >= keep) {
                int next = gs_utf8_grapheme_next(text, g->cluster);
                if (next > keep) keep = next;
            }
        }
        if (keep >= (int)sizeof cut - 4) keep = (int)sizeof cut - 4;
        snprintf(cut, sizeof cut, "%.*s\xE2\x80\xA6", keep, text);
        l = gs_fontset_line(ui->fonts, size * s, cut);
        width = l ? l->width : 0;
    }
    if (align < 0 && rtl) align = 1;
    float x = r.x * s + (align == 0 ? (avail - width) / 2 : align > 0 ? avail - width : 0);
    float baseline = (r.y + r.h / 2 + size * 0.36f) * s;
    gs_line_draw(ui->glyphs, l, roundf(x), roundf(baseline), c);
    return width / s;
}

static fl_item *defer(gs_ui *ui, fl_kind kind, gs_rect r, SDL_FColor c, const char *text, float size, int align) {
    static fl_item spare;
    if (ui->nfl == FLOATING) return &spare;
    fl_item *f = &ui->fl[ui->nfl++];
    *f = (fl_item){ kind, r, c, "", size, align, 0, 0, ui->fonts };
    if (text) SDL_strlcpy(f->text, text, sizeof f->text);
    return f;
}

static void round_now(gs_ui *ui, gs_rect r, float radius, SDL_FColor c) {
    if (radius <= 0) fill_now(ui, r, c);
    else gs_draw_round(ui->r, px(ui, r), radius * ui->scale, c);
}

static void outline_now(gs_ui *ui, gs_rect r, float radius, float width, SDL_FColor c) {
    if (radius <= 0 && width == 1) frame_now(ui, r, c);
    else gs_draw_outline(ui->r, px(ui, r), radius * ui->scale, width == 1 ? fmaxf(1, floorf(ui->scale)) : width * ui->scale, c);
}

// A colour as it is drawn: faded by the opacity in force.
static SDL_FColor ink(const gs_ui *ui, SDL_FColor c) { return c.a *= ui->opacity * (ui->dim ? DIM : 1), c; }

void gs_ui_font(gs_ui *ui, gs_fontset *fonts) { ui->fonts = fonts ? fonts : ui->own_fonts; }

void gs_ui_opacity(gs_ui *ui, float opacity) { ui->opacity = SDL_clamp(opacity, 0, 1); }

void gs_ui_fill(gs_ui *ui, gs_rect r, SDL_FColor c) { ui->in_float ? (void)defer(ui, FL_FILL, r, ink(ui, c), NULL, 0, 0) : fill_now(ui, r, ink(ui, c)); }
void gs_ui_frame(gs_ui *ui, gs_rect r, SDL_FColor c) { ui->in_float ? (void)defer(ui, FL_FRAME, r, ink(ui, c), NULL, 0, 0) : frame_now(ui, r, ink(ui, c)); }

void gs_ui_round(gs_ui *ui, gs_rect r, float radius, SDL_FColor c) {
    if (ui->in_float) defer(ui, FL_ROUND, r, ink(ui, c), NULL, 0, 0)->radius = radius;
    else round_now(ui, r, radius, ink(ui, c));
}

void gs_ui_outline(gs_ui *ui, gs_rect r, float radius, float width, SDL_FColor c) {
    if (ui->in_float) {
        fl_item *f = defer(ui, FL_OUTLINE, r, ink(ui, c), NULL, 0, 0);
        f->radius = radius, f->width = width;
    } else outline_now(ui, r, radius, width, ink(ui, c));
}

void gs_ui_circle(gs_ui *ui, float cx, float cy, float radius, SDL_FColor c) { gs_ui_round(ui, (gs_rect){ cx - radius, cy - radius, 2 * radius, 2 * radius }, radius, c); }

void gs_ui_progress(gs_ui *ui, gs_rect r, float fraction) {
    gs_ui_round(ui, r, r.h / 2, ui->style.raised);
    float w = r.w * SDL_clamp(fraction, 0, 1);
    if (w > 0) gs_ui_round(ui, (gs_rect){ r.x, r.y, fmaxf(w, fminf(r.h, r.w)), r.h }, r.h / 2, ui->style.accent);
}

float gs_ui_text(gs_ui *ui, gs_rect r, const char *text, float size, SDL_FColor c, int align) {
    if (ui->in_float) return defer(ui, FL_TEXT, r, ink(ui, c), text, size, align), gs_ui_text_width(ui, text, size);
    return text_now(ui, r, text, size, ink(ui, c), align);
}

// ---- Wrapped text ----

// `text` broken into lines of at most `width` points, kept for the frames that draw it again.
static const gs_wrap *wrap_lines(gs_ui *ui, const char *text, float size, float width, int max);
static const gs_wrap *wrap_of(gs_ui *ui, const char *text, float size, float width) { return wrap_lines(ui, text, size, width, 0); }

// The same held to `max` lines (0: as many as it takes).
static const gs_wrap *wrap_lines(gs_ui *ui, const char *text, float size, float width, int max) {
    float s = ui->scale > 0 ? ui->scale : 1;
    int size4 = (int)lroundf(size * s * 4), width4 = (int)lroundf(width * s * 4);
    wrap_entry *e = &ui->wraps[(hash(text) ^ (uint32_t)size4 * 40503u ^ (uint32_t)width4 * 2654435761u ^ (uint32_t)(uintptr_t)ui->fonts ^ (uint32_t)max * 97u) % WRAPS];
    if (e->wrap && e->size4 == size4 && e->width4 == width4 && e->max == max && e->fonts == ui->fonts && !strcmp(e->text, text)) return e->wrap;
    SDL_free(e->text), gs_wrap_free(e->wrap);
    *e = (wrap_entry){ SDL_strdup(text), size4, width4, max, ui->fonts, gs_fontset_wrap_lines(ui->fonts, size * s, text, width * s, max) };
    return e->wrap;
}

// text[from, to) as a string of its own, in a buffer that lasts until the next call.
static const char *piece(gs_ui *ui, const char *text, int from, int to) {
    size_t n = (size_t)(to - from);
    if (n + 1 > ui->piece_cap) ui->piece = realloc(ui->piece, ui->piece_cap = n + 1 + 256);
    memcpy(ui->piece, text + from, n), ui->piece[n] = 0;
    return ui->piece;
}

float gs_ui_line_height(const gs_ui *ui, float size) { (void)ui; return size * 1.4f; }

float gs_ui_text_height(gs_ui *ui, const char *text, float size, float width) {
    const gs_wrap *w = wrap_of(ui, text, size, width);
    return w ? w->count * gs_ui_line_height(ui, size) : 0;
}

// Draws line `i` of a wrapped text, in a line box of height `lh` whose top is `top`, aligned to the
// side its direction reads from within [x, x + width). Returns the shaped line, for its width.
static const gs_line *wrapped_line(gs_ui *ui, const char *text, const gs_wrap_line *ln, float size, float x, float width, float top, float lh, SDL_FColor c) {
    float s = ui->scale;
    const gs_line *l = gs_fontset_line_part(ui->fonts, size * s, text, ln->start, ln->end, ln->rtl ? GS_DIR_RTL : GS_DIR_LTR);
    if (l && l->count) gs_line_draw(ui->glyphs, l, roundf((x + (ln->rtl ? width - l->width / s : 0)) * s), roundf((top + lh / 2 + size * 0.36f) * s), c);
    return l;
}

float gs_ui_text_wrapped(gs_ui *ui, gs_rect r, const char *text, float size, SDL_FColor c) {
    const gs_wrap *w = wrap_of(ui, text, size, r.w);
    if (!w) return 0;
    float lh = gs_ui_line_height(ui, size);
    c = ink(ui, c);
    int first = r.y < 0 ? (int)(-r.y / lh) : 0;  // (only the lines on screen are drawn)
    for (int i = first; i < w->count && r.y + i * lh < ui->area.h; i++)
        wrapped_line(ui, text, &w->lines[i], size, r.x, r.w, r.y + i * lh, lh, c);
    return w->count * lh;
}

float gs_ui_text_lines(gs_ui *ui, gs_rect r, const char *text, float size, SDL_FColor c, int max_lines, bool *cut) {
    const gs_wrap *w = wrap_lines(ui, text, size, r.w, SDL_max(1, max_lines));
    if (cut) *cut = w && w->cut;
    if (!w) return 0;
    float lh = gs_ui_line_height(ui, size), s = ui->scale;
    c = ink(ui, c);
    for (int i = 0; i < w->count; i++) {
        const gs_wrap_line *ln = &w->lines[i];
        if (!w->cut || i < w->count - 1) { wrapped_line(ui, text, ln, size, r.x, r.w, r.y + i * lh, lh, c); continue; }
        // The last line of a text cut short, with its ellipsis.
        size_t n = (size_t)(ln->end - ln->start);
        if (n + 4 > ui->piece_cap) ui->piece = realloc(ui->piece, ui->piece_cap = n + 4 + 256);
        memcpy(ui->piece, text + ln->start, n), memcpy(ui->piece + n, "\xE2\x80\xA6", 4);
        const gs_line *l = gs_fontset_line_dir(ui->fonts, size * s, ui->piece, ln->rtl ? GS_DIR_RTL : GS_DIR_LTR);
        if (l && l->count) gs_line_draw(ui->glyphs, l, roundf((r.x + (ln->rtl ? r.w - l->width / s : 0)) * s), roundf((r.y + i * lh + lh / 2 + size * 0.36f) * s), c);
    }
    return w->count * lh;
}

void gs_ui_texture(gs_ui *ui, gs_rect r, SDL_Texture *t, const SDL_FRect *src) {
    float tw, th;
    if (!t || !SDL_GetTextureSize(t, &tw, &th)) return;
    SDL_FRect s = src ? *src : (SDL_FRect){ 0, 0, tw, th };
    float want = r.w / r.h, have = s.w / s.h;  // crop the source to the target's aspect
    if (have > want) s.x += (s.w - s.h * want) / 2, s.w = s.h * want;
    else if (have < want) s.y += (s.h - s.w / want) / 2, s.h = s.w / want;
    SDL_FRect d = px(ui, r);
    float alpha = 1;
    SDL_GetTextureAlphaModFloat(t, &alpha);
    float fade = ui->opacity * (ui->dim ? DIM : 1);
    if (fade < 1) SDL_SetTextureAlphaModFloat(t, alpha * fade);
    SDL_RenderTexture(ui->r, t, &s, &d);
    if (fade < 1) SDL_SetTextureAlphaModFloat(t, alpha);
}

void gs_ui_clip(gs_ui *ui, const gs_rect *r) {
    ui->clip.on = r != NULL;
    if (!r) { SDL_SetRenderClipRect(ui->r, NULL); return; }
    ui->clip.r = *r;
    SDL_FRect f = px(ui, *r);
    SDL_Rect c = { (int)floorf(f.x), (int)floorf(f.y), (int)ceilf(f.w), (int)ceilf(f.h) };
    SDL_SetRenderClipRect(ui->r, &c);
}

// Clips to the part of r inside the clip in force, which it returns for clip_pop to put back: a
// control that clips its own drawing leaves the clip of the list or page it is in as it was.
static clip_state clip_push(gs_ui *ui, gs_rect r) {
    clip_state before = ui->clip;
    if (before.on) {
        float x0 = fmaxf(r.x, before.r.x), y0 = fmaxf(r.y, before.r.y), x1 = fminf(r.x + r.w, before.r.x + before.r.w), y1 = fminf(r.y + r.h, before.r.y + before.r.h);
        r = (gs_rect){ x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0) };
    }
    gs_ui_clip(ui, &r);
    return before;
}

static void clip_pop(gs_ui *ui, clip_state before) { gs_ui_clip(ui, before.on ? &before.r : NULL); }

// ---- Input ----

// The pointer is free for a widget unless the floating layer covered it last frame (widgets in that
// layer, drawn while in_float, are the ones it belongs to).
// Under a modal, what is not in it takes nothing: no pointer, no keys, no place in the Tab order.
static bool covered(const gs_ui *ui) { return ui->modal_last && !ui->in_modal; }
static bool inert(const gs_ui *ui) { return ui->disabled || covered(ui); }

static bool pointer_over(gs_ui *ui, gs_rect r, float x, float y) {
    if (inert(ui)) return false;
    if (!ui->in_float && ui->float_last_on && gs_rect_contains(ui->float_last, x, y)) return false;
    if (!ui->in_float && ui->clip.on && !gs_rect_contains(ui->clip.r, x, y)) return false;  // (cut off by a list's or a page's edge)
    return gs_rect_contains(r, x, y);
}

bool gs_ui_hovered(gs_ui *ui, gs_rect r) { return pointer_over(ui, r, ui->in.x, ui->in.y); }
bool gs_ui_clicked(gs_ui *ui, gs_rect r) { return ui->in.released && pointer_over(ui, r, ui->in.x, ui->in.y) && pointer_over(ui, r, ui->press_x, ui->press_y); }
static gs_rect outset(gs_rect r, float d) { return (gs_rect){ r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d }; }
static bool pressed(gs_ui *ui, gs_rect r) { return ui->in.pressed && pointer_over(ui, r, ui->in.x, ui->in.y); }

bool gs_ui_key(gs_ui *ui, SDL_Keycode key, SDL_Keymod mods) {
    if (covered(ui)) return false;
    for (int i = 0; i < ui->in.nkeys; i++)
        if (!ui->taken[i] && ui->in.keys[i] == key && (mods ? (ui->in.mods[i] & mods) != 0 : !(ui->in.mods[i] & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))))
            return ui->taken[i] = true;
    return false;
}

void gs_ui_focus(gs_ui *ui, const char *id) { set_focus(ui, id ? hash(id) : 0); }
bool gs_ui_focused(gs_ui *ui, const char *id) { return ui->focus == hash(id); }

void gs_ui_disable(gs_ui *ui, bool disabled, bool dim) { ui->disabled = disabled, ui->dim = dim; }

// Enters a control into the Tab order. True when it has the keyboard.
static bool stop_at(gs_ui *ui, uint32_t h, gs_rect r) {
    if (inert(ui) || ui->in_float) return false;
    if (ui->nstops < STOPS) ui->stops[ui->nstops++] = (stop){ h, ui->page, r };
    return ui->focus == h;
}

// An id for a control that has only a label: the label's, or another when a control drawn earlier in
// the frame has the same label, so each of several "Remove" buttons is a stop of its own.
static uint32_t label_id(gs_ui *ui, const char *label) {
    uint32_t h = hash(label);
    for (int i = 0; i < ui->nstops; i++)
        if (ui->stops[i].id == h) h = h * 16777619u + 1, h += !h, i = -1;
    return h;
}

// Space or Enter for the control that has the keyboard. Only once the keyboard has moved the focus:
// after a click those keys are the app's again, and an open menu or dropdown has them first.
static bool act(gs_ui *ui, bool focused) {
    return focused && ui->marked && !ui->float_last_on && (gs_ui_key(ui, SDLK_SPACE, 0) || gs_ui_key(ui, SDLK_RETURN, 0) || gs_ui_key(ui, SDLK_KP_ENTER, 0));
}

// The focus mark: a ring around the control the keyboard has reached.
static void mark(gs_ui *ui, bool focused, gs_rect r, float radius) {
    if (focused && ui->marked) gs_ui_outline(ui, outset(r, 3), radius + 3, 2, ui->style.focus);
}

bool gs_ui_activated(gs_ui *ui, gs_rect r, const char *id) {
    bool focused = stop_at(ui, hash(id), r);
    return gs_ui_clicked(ui, r) || act(ui, focused);
}

void gs_ui_focus_mark(gs_ui *ui, gs_rect r, const char *id, float radius) { mark(ui, ui->focus == hash(id) && !inert(ui), r, radius); }

// ---- Widgets ----

// A button with an id: plain, or filled in `fill` with the accent's text colour.
static bool button(gs_ui *ui, gs_rect r, uint32_t h, const char *label, const SDL_FColor *fill) {
    bool focused = stop_at(ui, h, r), hot = gs_ui_hovered(ui, r);
    gs_ui_round(ui, r, ui->style.radius, fill ? (hot ? ui->style.text : *fill) : hot ? ui->style.border : ui->style.raised);
    gs_ui_text(ui, gs_inset(r, 6), label, ui->style.text_px, fill ? ui->style.accent_text : ui->style.text, 0);
    mark(ui, focused, r, ui->style.radius);
    return gs_ui_clicked(ui, r) || act(ui, focused);
}

bool gs_ui_button(gs_ui *ui, gs_rect r, const char *label) { return button(ui, r, label_id(ui, label), label, NULL); }

static SDL_FColor mix(SDL_FColor a, SDL_FColor b, float t) {
    return (SDL_FColor){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

static float ease_to(gs_ui *ui, widget *w, float target, int ms) {
    if (!w->eased) w->eased = true, w->from = w->to = w->value = target;
    if (target != w->to) w->from = w->value, w->to = target, w->since = ui->now;
    float t = ms > 0 && w->from != w->to ? (float)(ui->now - w->since) / (float)ms : 1;
    w->value = t >= 1 ? w->to : w->from + (w->to - w->from) * gs_ease(t);
    if (t < 1) wake_within(ui, 16);
    return w->value;
}

float gs_ui_ease(gs_ui *ui, const char *id, float target, int ms) { return ease_to(ui, state(ui, hash(id)), target, ms); }

bool gs_ui_switch(gs_ui *ui, gs_rect r, const char *id, bool *on) {
    uint32_t h = hash(id);
    bool focused = stop_at(ui, h, r), hit = gs_ui_clicked(ui, r) || act(ui, focused);
    if (hit) *on = !*on;
    float t = ease_to(ui, state(ui, h), *on ? 1 : 0, 120), half = r.h / 2;
    gs_ui_round(ui, r, half, mix(ui->style.raised, ui->style.accent, t));
    SDL_FColor edge = gs_ui_hovered(ui, r) ? ui->style.muted : ui->style.border;
    edge.a *= 1 - t;
    gs_ui_outline(ui, r, half, 1, edge);
    gs_ui_circle(ui, r.x + half + (r.w - r.h) * t, r.y + half, half - 3, mix(ui->style.muted, ui->style.accent_text, t));
    mark(ui, focused, r, half);
    return hit;
}

bool gs_ui_radio(gs_ui *ui, gs_rect r, const char *id, const char *label, bool selected) {
    bool focused = stop_at(ui, hash(id), r), hit = gs_ui_clicked(ui, r) || act(ui, focused);
    gs_rect ring = { r.x + 1, r.y + r.h / 2 - 8, 16, 16 };
    gs_ui_outline(ui, ring, 8, 1.5f, selected ? ui->style.accent : gs_ui_hovered(ui, r) ? ui->style.text : ui->style.muted);
    if (selected) gs_ui_circle(ui, ring.x + 8, ring.y + 8, 4.5f, ui->style.accent);
    gs_ui_text(ui, (gs_rect){ r.x + 26, r.y, fmaxf(0, r.w - 26), r.h }, label, ui->style.text_px, ui->style.text, -1);
    mark(ui, focused, ring, 8);
    return hit && !selected;
}

bool gs_ui_stops(gs_ui *ui, gs_rect r, const char *id, int *value, int count) {
    if (count < 2) return false;
    uint32_t h = hash(id);
    bool focused = stop_at(ui, h, r);
    int before = *value;
    float x0 = r.x + 9, x1 = r.x + r.w - 9, y = r.y + r.h / 2;
    if (pressed(ui, r)) ui->drag = h;
    if (ui->drag == h) *value = (int)floorf((ui->in.x - x0) / (x1 - x0) * (float)(count - 1) + 0.5f);
    if (focused && ui->marked) {
        while (gs_ui_key(ui, SDLK_RIGHT, 0) || gs_ui_key(ui, SDLK_UP, 0)) ++*value;
        while (gs_ui_key(ui, SDLK_LEFT, 0) || gs_ui_key(ui, SDLK_DOWN, 0)) --*value;
        if (gs_ui_key(ui, SDLK_HOME, 0)) *value = 0;
        if (gs_ui_key(ui, SDLK_END, 0)) *value = count - 1;
    }
    *value = SDL_clamp(*value, 0, count - 1);
    float knob = x0 + (x1 - x0) * (float)*value / (float)(count - 1);
    gs_ui_round(ui, (gs_rect){ x0, y - 2, x1 - x0, 4 }, 2, ui->style.raised);
    gs_ui_round(ui, (gs_rect){ x0, y - 2, knob - x0, 4 }, 2, ui->style.accent);
    for (int i = 0; i < count; i++) gs_ui_circle(ui, x0 + (x1 - x0) * (float)i / (float)(count - 1), y, 2.5f, i <= *value ? ui->style.accent : ui->style.muted);
    gs_ui_circle(ui, knob, y, 8, gs_ui_hovered(ui, r) || ui->drag == h ? ui->style.text : ui->style.accent);
    mark(ui, focused, (gs_rect){ knob - 8, y - 8, 16, 16 }, 8);
    return *value != before;
}

bool gs_ui_link(gs_ui *ui, gs_rect r, const char *label) {
    float size = ui->style.text_px;
    float width = gs_ui_text_width(ui, label, size);
    gs_rect hit = { r.x, r.y, width > 0 ? fminf(r.w, width) : r.w, r.h };
    bool focused = stop_at(ui, label_id(ui, label), hit), hot = gs_ui_hovered(ui, hit);
    if (hot) ui->cursor_want = 1;
    SDL_FColor c = hot ? ui->style.text : ui->style.accent;
    gs_ui_text(ui, hit, label, size, c, -1);
    gs_ui_fill(ui, (gs_rect){ hit.x, r.y + r.h / 2 + size * 0.36f + 2, hit.w, 1 }, c);
    mark(ui, focused, hit, 2);
    return gs_ui_clicked(ui, hit) || act(ui, focused);
}

bool gs_ui_toggle(gs_ui *ui, gs_rect r, const char *label, bool *on) {
    bool focused = stop_at(ui, label_id(ui, label), r);
    bool hit = gs_ui_clicked(ui, r) || act(ui, focused);
    if (hit) *on = !*on;
    gs_rect box = { r.x, r.y + (r.h - 16) / 2, 16, 16 };
    float radius = fminf(ui->style.radius, 4);
    gs_ui_round(ui, box, radius, *on ? ui->style.accent : ui->style.raised);
    gs_ui_outline(ui, box, radius, 1, gs_ui_hovered(ui, r) ? ui->style.text : ui->style.border);
    if (*on) gs_ui_round(ui, gs_inset(box, 4), radius / 2, ui->style.accent_text);
    gs_rect rest = r;
    gs_cut_left(&rest, 22);
    gs_ui_text(ui, rest, label, ui->style.text_px, ui->style.text, -1);
    mark(ui, focused, box, radius);
    return hit;
}

bool gs_ui_slider(gs_ui *ui, gs_rect r, const char *id, float *value, float min, float max) {
    uint32_t h = hash(id);
    bool focused = stop_at(ui, h, r);
    if (pressed(ui, r)) ui->drag = h;
    float before = *value;
    if (ui->drag == h && max > min) *value = fminf(max, fmaxf(min, min + (ui->in.x - r.x) / r.w * (max - min)));
    if (focused && ui->marked && max > min) {  // the arrows move it a twentieth of its range, Home and End to its ends
        float step = (max - min) / 20;
        while (gs_ui_key(ui, SDLK_RIGHT, 0) || gs_ui_key(ui, SDLK_UP, 0)) *value = fminf(max, *value + step);
        while (gs_ui_key(ui, SDLK_LEFT, 0) || gs_ui_key(ui, SDLK_DOWN, 0)) *value = fmaxf(min, *value - step);
        if (gs_ui_key(ui, SDLK_HOME, 0)) *value = min;
        if (gs_ui_key(ui, SDLK_END, 0)) *value = max;
    }
    float t = max > min ? (*value - min) / (max - min) : 0;
    gs_rect track = { r.x, r.y + r.h / 2 - 2, r.w, 4 }, knob = { r.x + r.w * t - 5, r.y + r.h / 2 - 7, 10, 14 };
    float radius = ui->style.radius;
    gs_ui_round(ui, track, fminf(radius, 2), ui->style.raised);
    gs_ui_round(ui, (gs_rect){ track.x, track.y, track.w * t, track.h }, fminf(radius, 2), ui->style.accent);
    gs_ui_round(ui, knob, fminf(radius, 5), gs_ui_hovered(ui, r) || ui->drag == h ? ui->style.text : ui->style.muted);
    mark(ui, focused, knob, fminf(radius, 5));
    return *value != before;
}

// The field's edits: typed text, and the keys that move the cursor or change the text.
static bool edit(gs_ui *ui, widget *w, char *buf, size_t size, bool secret) {
    bool changed = false;
    size_t len = strlen(buf);
    if (w->cursor > (int)len) w->cursor = (int)len;
    char clean[512];
    const char *typed = ui->in.text;
    for (int i = 0; i < ui->in.nkeys; i++) {
        SDL_Keycode k = ui->in.keys[i];
        bool ctrl = ui->in.mods[i] & (SDL_KMOD_CTRL | SDL_KMOD_GUI);
        if (ui->taken[i]) continue;
        if (ctrl && k == SDLK_V) {
            char *paste = SDL_GetClipboardText();
            size_t n = 0;
            for (const char *p = paste ? paste : ""; *p && n + 1 < sizeof clean; p++) clean[n++] = *p == '\n' || *p == '\r' || *p == '\t' ? ' ' : *p;
            clean[n] = 0;
            SDL_free(paste);
            typed = clean;
        } else if (ctrl && k == SDLK_A) {
            w->all = true;
        } else if (ctrl && (k == SDLK_C || k == SDLK_X) && w->all && len && !secret) {  // (what is shown as dots is not copied out)
            SDL_SetClipboardText(buf);  // (only when the user copies)
            if (k == SDLK_X) buf[0] = 0, len = 0, w->cursor = 0, w->all = false, changed = true;
        } else if (k == SDLK_BACKSPACE || k == SDLK_DELETE) {
            if (w->all) buf[0] = 0, len = 0, w->cursor = 0, w->all = false, changed = true;
            else {
                int from = k == SDLK_BACKSPACE ? gs_utf8_grapheme_prev(buf, w->cursor) : w->cursor;
                int to = k == SDLK_BACKSPACE ? w->cursor : gs_utf8_grapheme_next(buf, w->cursor);
                if (to > from) memmove(buf + from, buf + to, len - to + 1), len -= to - from, w->cursor = from, changed = true;
            }
        } else if (k == SDLK_LEFT) {
            w->cursor = w->all ? 0 : gs_utf8_grapheme_prev(buf, w->cursor), w->all = false;
        } else if (k == SDLK_RIGHT) {
            w->cursor = w->all ? (int)len : gs_utf8_grapheme_next(buf, w->cursor), w->all = false;
        } else if (k == SDLK_HOME) {
            w->cursor = 0, w->all = false;
        } else if (k == SDLK_END) {
            w->cursor = (int)len, w->all = false;
        } else if (k == SDLK_ESCAPE && len) {
            buf[0] = 0, len = 0, w->cursor = 0, changed = true;  // Esc clears the field; a second Esc goes to the app
        } else if (!ctrl && !(ui->in.mods[i] & SDL_KMOD_ALT) && k >= 32 && k < 127) {
            // A printable key arrives as typed text too: it is the field's, not a shortcut's.
        } else {
            continue;
        }
        ui->taken[i] = true, ui->caret_since = ui->now;
    }
    if (typed[0]) {
        if (w->all) buf[0] = 0, len = 0, w->cursor = 0, w->all = false;
        size_t n = strlen(typed);
        if (len + n < size) {
            memmove(buf + w->cursor + n, buf + w->cursor, len - w->cursor + 1);
            memcpy(buf + w->cursor, typed, n);
            w->cursor += (int)n, changed = true, ui->caret_since = ui->now;
        }
    }
    return changed;
}

// buf as a masked field shows it, a bullet for each character, and where its cursor is among them.
static int bullets(const char *buf, int cursor, char *out, size_t size) {
    int at = 0, n = 0;
    for (int i = 0; buf[i] && (size_t)n + 4 < size; i = gs_utf8_grapheme_next(buf, i)) {
        if (i < cursor) at = n + 3;
        memcpy(out + n, "\xE2\x80\xA2", 3), n += 3;
    }
    out[n] = 0;
    return at;
}

static bool field(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder, int flags) {
    uint32_t h = hash(id);
    widget *w = state(ui, h);
    float s = ui->scale, pad = 8, text_px = ui->style.text_px;
    gs_rect inner = gs_inset(r, pad);
    inner.y = r.y, inner.h = r.h;
    char dots[768];
    bool was = stop_at(ui, h, r), masked = (flags & GS_UI_MASKED) && !((flags & GS_UI_REVEAL) && was);
    if (gs_ui_hovered(ui, r)) ui->cursor_want = 2;
    if (pressed(ui, r)) {
        if (ui->focus != h) gs_ui_focus(ui, id);
        if (masked) bullets(buf, 0, dots, sizeof dots);
        const gs_line *l = gs_fontset_line(ui->fonts, text_px * s, masked ? dots : buf);
        int at = l ? gs_line_hit(l, (ui->in.x - inner.x) * s, masked ? dots : buf) : 0;
        if (masked) {  // (the bullet's place, as the character's)
            int to = 0;
            for (int n = at / 3; n > 0 && buf[to]; n--) to = gs_utf8_grapheme_next(buf, to);
            at = to;
        }
        w->cursor = at, w->all = false;
        ui->caret_since = ui->now;
    }
    if (ui->focus == h && ui->in.pressed && !gs_rect_contains(r, ui->in.x, ui->in.y)) gs_ui_focus(ui, NULL);  // a click elsewhere leaves it
    bool focused = ui->focus == h && !inert(ui), changed = false;
    if (focused && (flags & GS_UI_KEEP) && gs_ui_key(ui, SDLK_ESCAPE, 0)) gs_ui_focus(ui, NULL), focused = false;  // Esc lets go and keeps the text
    masked = (flags & GS_UI_MASKED) && !((flags & GS_UI_REVEAL) && focused);
    if (focused) {
        if (!SDL_TextInputActive(ui->win)) SDL_StartTextInput(ui->win);
        changed = edit(ui, w, buf, size, masked);
    }
    if (!focused) w->all = false;  // (a selection is the focused field's)
    int cursor = masked ? bullets(buf, w->cursor, dots, sizeof dots) : w->cursor;
    const char *shown = masked ? dots : buf;
    gs_ui_round(ui, r, ui->style.radius, ui->style.raised);
    gs_ui_outline(ui, r, ui->style.radius, 1, focused ? ui->style.accent : gs_ui_hovered(ui, r) ? ui->style.muted : ui->style.border);
    clip_state outer = clip_push(ui, r);
    if (w->all && buf[0]) gs_ui_fill(ui, (gs_rect){ inner.x, r.y + 5, fminf(gs_ui_text_width(ui, shown, text_px), inner.w), r.h - 10 }, ui->style.border);
    if (buf[0] || ui->composition[0]) gs_ui_text(ui, (gs_rect){ inner.x, r.y, 1e6f, r.h }, shown, text_px, ui->style.text, -1);
    else if (placeholder) gs_ui_text(ui, inner, placeholder, text_px, ui->style.muted, -1);
    if (focused) {
        const gs_line *l = gs_fontset_line(ui->fonts, text_px * s, shown);
        float cx = inner.x + (l ? gs_line_caret(l, cursor) / s : 0);
        if (ui->composition[0]) {  // what the input method is composing, underlined at the cursor
            float cw = gs_ui_text(ui, (gs_rect){ cx, r.y, 1e6f, r.h }, ui->composition, text_px, ui->style.accent, -1);
            gs_ui_fill(ui, (gs_rect){ cx, r.y + r.h - 7, cw, 1 }, ui->style.accent);
            cx += cw;
        }
        uint64_t t = ui->now - ui->caret_since;
        if (t % (2 * BLINK_MS) < BLINK_MS) gs_ui_fill(ui, (gs_rect){ cx, r.y + 6, 1.5f, r.h - 12 }, ui->style.text);
        wake_within(ui, (int)(BLINK_MS - t % BLINK_MS));
        SDL_Rect area = { (int)r.x, (int)r.y, (int)r.w, (int)r.h };  // where the input method's candidates go
        SDL_SetTextInputArea(ui->win, &area, (int)(cx - r.x));
    }
    clip_pop(ui, outer);
    return changed;
}

bool gs_ui_field(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder) { return field(ui, r, id, buf, size, placeholder, 0); }
bool gs_ui_input(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder, int flags) { return field(ui, r, id, buf, size, placeholder, flags); }

static void number_text(char *out, size_t size, double value, int decimals) { snprintf(out, size, "%.*f", SDL_clamp(decimals, 0, 9), value); }

bool gs_ui_number(gs_ui *ui, gs_rect r, const char *id, double *value, double min, double max, double step, int decimals) {
    uint32_t h = hash(id);
    double before = *value;
    if (ui->focus != h || inert(ui)) {  // at rest it shows the value
        char shown[32];
        if (ui->number == h) ui->number = 0;
        number_text(shown, sizeof shown, *value, decimals);
        field(ui, r, id, shown, sizeof shown, NULL, GS_UI_KEEP);
        return false;
    }
    widget *w = state(ui, h);
    if (ui->number != h) {  // taken up: its text is the value's, all selected, so that typing replaces it
        ui->number = h;
        number_text(ui->number_text, sizeof ui->number_text, *value, decimals);
        w->all = true, w->cursor = (int)strlen(ui->number_text);
    }
    char *typed = ui->in.text, *keep = typed;  // only what a number is written with gets in
    for (; *typed; typed++) {
        char c = *typed == ',' ? '.' : *typed;
        if ((c >= '0' && c <= '9') || c == '-' || (c == '.' && decimals > 0)) *keep++ = c;
    }
    *keep = 0;
    int steps = 0;
    bool commit = false;
    while (gs_ui_key(ui, SDLK_UP, 0)) steps++;
    while (gs_ui_key(ui, SDLK_DOWN, 0)) steps--;
    for (int i = 0; i < ui->in.nkeys; i++) commit = commit || ui->in.keys[i] == SDLK_RETURN || ui->in.keys[i] == SDLK_KP_ENTER;  // (Enter stays the app's)
    field(ui, r, id, ui->number_text, sizeof ui->number_text, NULL, GS_UI_KEEP);
    char *end;
    double v = SDL_strtod(ui->number_text, &end), unit = pow(10, SDL_clamp(decimals, 0, 9));
    if (end == ui->number_text) v = *value;  // (nothing of a number yet)
    if (steps) v += steps * step;
    *value = SDL_clamp(round(v * unit) / unit, min, max);
    if (steps || commit) {  // written out again as the value it came to
        number_text(ui->number_text, sizeof ui->number_text, *value, decimals);
        w->cursor = (int)strlen(ui->number_text), w->all = false;
    }
    return *value != before;
}

// ---- Text area ----

#define AREA_PAD_X 8
#define AREA_PAD_Y 5

float gs_ui_textarea_height(gs_ui *ui, const char *buf, float width, int max_lines) {
    const gs_wrap *w = wrap_of(ui, buf, ui->style.text_px, width - 2 * AREA_PAD_X);
    int lines = w ? w->count : 1;
    return fmaxf(ui->style.row, fminf(lines, fmaxf(1, max_lines)) * gs_ui_line_height(ui, ui->style.text_px) + 2 * AREA_PAD_Y);
}

static int line_at(const gs_wrap *w, int offset) {  // the line a cursor at `offset` is on: the last to start at or before it
    int line = 0;
    for (int i = 1; i < w->count && w->lines[i].start <= offset; i++) line = i;
    return line;
}

// A line shaped alone, and where it starts across: a right-to-left line reads from the right edge.
static const gs_line *area_line(gs_ui *ui, const char *buf, const gs_wrap_line *ln, float inner_x, float inner_w, float *origin) {
    piece(ui, buf, ln->start, ln->end);  // (the line's own text, for whoever asks where a point falls in it)
    const gs_line *l = gs_fontset_line_part(ui->fonts, ui->style.text_px * ui->scale, buf, ln->start, ln->end, ln->rtl ? GS_DIR_RTL : GS_DIR_LTR);
    *origin = inner_x + (ln->rtl && l ? inner_w - l->width / ui->scale : 0);
    return l;
}

static float caret_x(gs_ui *ui, const char *buf, const gs_wrap_line *ln, int offset, float inner_x, float inner_w) {
    float origin;
    const gs_line *l = area_line(ui, buf, ln, inner_x, inner_w, &origin);
    return origin + (l ? gs_line_caret(l, SDL_clamp(offset, ln->start, ln->end) - ln->start) / ui->scale : 0);
}

// The offset nearest to point x across line `ln`.
static int offset_at(gs_ui *ui, const char *buf, const gs_wrap_line *ln, float x, float inner_x, float inner_w) {
    float origin;
    const gs_line *l = area_line(ui, buf, ln, inner_x, inner_w, &origin);
    if (!l) return ln->start;
    return ln->start + SDL_min(gs_line_hit(l, (x - origin) * ui->scale, ui->piece), ln->end - ln->start);
}

static bool is_gap(char ch) { return ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r'; }
static int word_prev(const char *b, int p) {
    while (p > 0 && is_gap(b[p - 1])) p--;
    while (p > 0 && !is_gap(b[p - 1])) p--;
    return p;
}
static int word_next(const char *b, int len, int p) {
    while (p < len && is_gap(b[p])) p++;
    while (p < len && !is_gap(b[p])) p++;
    return p;
}

// Replaces the selection (or nothing, at the cursor) with n bytes of text, as many as fit.
static bool put(char *buf, size_t size, widget *w, const char *text, size_t n) {
    size_t len = strlen(buf);
    int from = w->cursor, to = w->cursor;
    if (w->anchored) from = SDL_min(w->anchor, w->cursor), to = SDL_max(w->anchor, w->cursor);
    size_t room = size - 1 - (len - (size_t)(to - from));
    if (n > room) {
        n = room;
        while (n && (text[n] & 0xC0) == 0x80) n--;  // (not through the middle of a character)
    }
    if (to == from && !n) return false;
    memmove(buf + from + n, buf + to, len - (size_t)to + 1);
    memcpy(buf + from, text, n);
    w->cursor = from + (int)n, w->anchored = false, w->want = false;
    return true;
}

static void select_to(widget *w, int offset, bool extend) {
    if (extend && !w->anchored) w->anchor = w->cursor, w->anchored = true;
    if (!extend) w->anchored = false;
    w->cursor = offset;
}

// The clipboard's text as it is put in a text area: line ends as \n, tabs as spaces.
static char *clipboard_lines(void) {
    char *text = SDL_GetClipboardText(), *out = text ? malloc(strlen(text) + 1) : NULL;
    size_t n = 0;
    for (const char *p = text; out && *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        out[n++] = *p == '\r' ? '\n' : *p == '\t' ? ' ' : *p;
    }
    if (out) out[n] = 0;
    SDL_free(text);
    return out;
}

static bool area_edit(gs_ui *ui, widget *w, char *buf, size_t size, gs_rect inner, bool *moved) {
    bool changed = false;
    for (int i = 0; i < ui->in.nkeys; i++) {
        SDL_Keycode k = ui->in.keys[i];
        SDL_Keymod mods = ui->in.mods[i];
        bool ctrl = mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI), shift = mods & SDL_KMOD_SHIFT;
        if (ui->taken[i]) continue;
        int len = (int)strlen(buf);
        w->cursor = SDL_clamp(w->cursor, 0, len), w->anchor = SDL_clamp(w->anchor, 0, len);
        bool has = w->anchored && w->anchor != w->cursor;
        int lo = SDL_min(w->anchor, w->cursor), hi = SDL_max(w->anchor, w->cursor);
        const gs_wrap *wr = NULL;
        if (k == SDLK_UP || k == SDLK_DOWN || ((k == SDLK_HOME || k == SDLK_END) && !ctrl)) wr = wrap_of(ui, buf, ui->style.text_px, inner.w);
        bool vertical = false;
        if (ctrl && k == SDLK_V) {
            char *paste = clipboard_lines();
            if (paste) changed |= put(buf, size, w, paste, strlen(paste));
            free(paste);
        } else if (ctrl && k == SDLK_A) {
            w->anchor = 0, w->cursor = len, w->anchored = true;
        } else if (ctrl && (k == SDLK_C || k == SDLK_X)) {
            if (has) {
                SDL_SetClipboardText(piece(ui, buf, lo, hi));
                if (k == SDLK_X) changed |= put(buf, size, w, "", 0);
            }
        } else if (k == SDLK_BACKSPACE || k == SDLK_DELETE) {
            if (!has) {
                int back = k == SDLK_BACKSPACE, at = w->cursor;
                int to = back ? (ctrl ? word_prev(buf, at) : gs_utf8_grapheme_prev(buf, at)) : (ctrl ? word_next(buf, len, at) : gs_utf8_grapheme_next(buf, at));
                w->anchor = to, w->anchored = true;
            }
            changed |= put(buf, size, w, "", 0);
        } else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
            bool left = k == SDLK_LEFT;
            if (has && !shift) select_to(w, left ? lo : hi, false);
            else select_to(w, left ? (ctrl ? word_prev(buf, w->cursor) : gs_utf8_grapheme_prev(buf, w->cursor)) : (ctrl ? word_next(buf, len, w->cursor) : gs_utf8_grapheme_next(buf, w->cursor)), shift);
        } else if (k == SDLK_UP || k == SDLK_DOWN) {
            int line = wr ? line_at(wr, w->cursor) : 0, to = line + (k == SDLK_UP ? -1 : 1);
            if (!w->want) w->want_x = caret_x(ui, buf, &wr->lines[line], w->cursor, inner.x, inner.w), w->want = true;
            if (to < 0) select_to(w, 0, shift);
            else if (to >= wr->count) select_to(w, len, shift);
            else select_to(w, offset_at(ui, buf, &wr->lines[to], w->want_x, inner.x, inner.w), shift);
            vertical = true;
        } else if (k == SDLK_HOME || k == SDLK_END) {
            bool home = k == SDLK_HOME;
            if (ctrl) select_to(w, home ? 0 : len, shift);
            else select_to(w, home ? wr->lines[line_at(wr, w->cursor)].start : wr->lines[line_at(wr, w->cursor)].end, shift);
        } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && shift) {
            changed |= put(buf, size, w, "\n", 1);  // (a plain Enter is the app's)
        } else if (!ctrl && !(mods & SDL_KMOD_ALT) && k >= 32 && k < 127) {
            // A printable key arrives as typed text too: it is the field's, not a shortcut's.
        } else {
            continue;
        }
        if (!vertical) w->want = false;
        ui->taken[i] = true, ui->caret_since = ui->now, *moved = true;
    }
    if (ui->in.text[0]) changed |= put(buf, size, w, ui->in.text, strlen(ui->in.text)), ui->caret_since = ui->now, *moved = true;
    return changed;
}

bool gs_ui_textarea(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder) {
    uint32_t h = hash(id);
    widget *w = state(ui, h);
    float px = ui->style.text_px, lh = gs_ui_line_height(ui, px);
    gs_rect inner = { r.x + AREA_PAD_X, r.y + AREA_PAD_Y, fmaxf(1, r.w - 2 * AREA_PAD_X), fmaxf(1, r.h - 2 * AREA_PAD_Y) };
    bool moved = false;
    w->cursor = SDL_clamp(w->cursor, 0, (int)strlen(buf)), w->anchor = SDL_clamp(w->anchor, 0, (int)strlen(buf));
    if (gs_ui_hovered(ui, r)) ui->cursor_want = 2;
    if (pressed(ui, r)) {  // a press puts the cursor under the pointer and starts a drag selection
        if (ui->focus != h) gs_ui_focus(ui, id);
        ui->drag = h;
        w->anchor = w->cursor, w->anchored = true;
        moved = true;
    }
    if (ui->focus == h && ui->in.pressed && !gs_rect_contains(r, ui->in.x, ui->in.y)) gs_ui_focus(ui, NULL);  // a click elsewhere leaves it
    if (ui->drag == h && ui->in.down) {
        const gs_wrap *wr = wrap_of(ui, buf, px, inner.w);
        if (wr) {
            int line = SDL_clamp((int)floorf((ui->in.y - inner.y + w->scroll) / lh), 0, wr->count - 1);
            bool extend = !ui->in.pressed || (SDL_GetModState() & SDL_KMOD_SHIFT);
            int at = offset_at(ui, buf, &wr->lines[line], ui->in.x, inner.x, inner.w);
            if (ui->in.pressed && !extend) w->anchor = at;
            else if (ui->in.pressed) w->anchor = w->anchored ? w->anchor : w->cursor;
            w->cursor = at, w->want = false, ui->caret_since = ui->now;
        }
    }
    bool focused = stop_at(ui, h, r), changed = false;
    if (focused) {
        if (!SDL_TextInputActive(ui->win)) SDL_StartTextInput(ui->win);
        changed = area_edit(ui, w, buf, size, inner, &moved);
    }

    const gs_wrap *wr = wrap_of(ui, buf, px, inner.w);
    int lines = wr ? wr->count : 1;
    float max_scroll = fmaxf(0, lines * lh - inner.h);
    if (gs_ui_hovered(ui, r) && ui->in.wheel && max_scroll > 0) w->scroll -= ui->in.wheel * lh * 3, ui->in.wheel = 0;  // (taken from a page it is in)
    int cline = wr ? line_at(wr, w->cursor) : 0;
    if (moved && focused) {  // keep the cursor's line in view
        if (cline * lh < w->scroll) w->scroll = cline * lh;
        if ((cline + 1) * lh > w->scroll + inner.h) w->scroll = (cline + 1) * lh - inner.h;
    }
    w->scroll = fminf(max_scroll, fmaxf(0, w->scroll));

    gs_ui_round(ui, r, ui->style.radius, ui->style.raised);
    gs_ui_outline(ui, r, ui->style.radius, 1, focused ? ui->style.accent : gs_ui_hovered(ui, r) ? ui->style.muted : ui->style.border);
    clip_state outer = clip_push(ui, r);
    if (!buf[0] && !ui->composition[0] && placeholder) gs_ui_text(ui, (gs_rect){ inner.x, inner.y, inner.w, lh }, placeholder, px, ui->style.muted, -1);
    int lo = SDL_min(w->anchor, w->cursor), hi = SDL_max(w->anchor, w->cursor);
    bool has = w->anchored && lo != hi;
    for (int i = wr ? (int)(w->scroll / lh) : 0; wr && i < wr->count && inner.y + i * lh - w->scroll < r.y + r.h; i++) {
        const gs_wrap_line *ln = &wr->lines[i];
        float top = inner.y + i * lh - w->scroll;
        if (has && hi > ln->start && (lo <= ln->end || (i + 1 < wr->count && lo < wr->lines[i + 1].start))) {  // the selected part of the line
            int from = SDL_max(lo, ln->start), to = SDL_min(hi, ln->end);
            float x0 = caret_x(ui, buf, ln, from, inner.x, inner.w), x1 = caret_x(ui, buf, ln, to, inner.x, inner.w);
            if (hi > ln->end) x1 += 4;  // (the line end it runs past)
            gs_ui_fill(ui, (gs_rect){ fminf(x0, x1), top, fabsf(x1 - x0), lh }, ui->style.border);
        }
        wrapped_line(ui, buf, ln, px, inner.x, inner.w, top, lh, ink(ui, ui->style.text));
    }
    if (focused && wr) {
        const gs_wrap_line *ln = &wr->lines[cline];
        float cx = caret_x(ui, buf, ln, w->cursor, inner.x, inner.w), top = inner.y + cline * lh - w->scroll;
        if (ui->composition[0]) {  // what the input method is composing, underlined at the cursor
            float cw = gs_ui_text(ui, (gs_rect){ cx, top, 1e6f, lh }, ui->composition, px, ui->style.accent, -1);
            gs_ui_fill(ui, (gs_rect){ cx, top + lh - 3, cw, 1 }, ui->style.accent);
            cx += cw;
        }
        uint64_t t = ui->now - ui->caret_since;
        if (t % (2 * BLINK_MS) < BLINK_MS) gs_ui_fill(ui, (gs_rect){ cx, top + 2, 1.5f, lh - 4 }, ui->style.text);
        wake_within(ui, (int)(BLINK_MS - t % BLINK_MS));
        SDL_Rect area = { (int)r.x, (int)top, (int)r.w, (int)lh };  // where the input method's candidates go
        SDL_SetTextInputArea(ui->win, &area, (int)(cx - r.x));
    }
    clip_pop(ui, outer);
    return changed;
}

bool gs_ui_textarea_selection(gs_ui *ui, const char *id, int *start, int *end) {
    const widget *w = state(ui, hash(id));
    bool has = w->anchored && w->anchor != w->cursor;
    if (start) *start = has ? SDL_min(w->anchor, w->cursor) : w->cursor;
    if (end) *end = has ? SDL_max(w->anchor, w->cursor) : w->cursor;
    return has;
}

void gs_ui_textarea_select(gs_ui *ui, const char *id, int start, int end) {
    widget *w = state(ui, hash(id));
    w->anchor = start, w->cursor = end, w->anchored = start != end, w->want = false;  // (held within the text when the area is next drawn)
}

void gs_ui_arrow(gs_ui *ui, gs_rect r, SDL_FColor c) {
    float s = ui->scale, w = fminf(r.w, 8), cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    c = ink(ui, c);
    SDL_Vertex v[3] = { { { (cx - w / 2) * s, (cy - w / 4) * s }, c, { 0 } }, { { (cx + w / 2) * s, (cy - w / 4) * s }, c, { 0 } },
                        { { cx * s, (cy + w / 4) * s }, c, { 0 } } };
    SDL_RenderGeometry(ui->r, NULL, v, 3, NULL, 0);
}

bool gs_ui_dropdown(gs_ui *ui, gs_rect r, const char *id, const char *const *items, int count, int *selected) {
    uint32_t h = hash(id);
    widget *w = state(ui, h);
    bool focused = stop_at(ui, h, r), hot = gs_ui_hovered(ui, r), changed = false;
    if (focused && ui->marked && count > 0) {  // the arrows change the choice, open or not; Space and Enter open and close the list
        int next = *selected;
        while (gs_ui_key(ui, SDLK_DOWN, 0)) next++;
        while (gs_ui_key(ui, SDLK_UP, 0)) next--;
        next = SDL_clamp(next, 0, count - 1);
        changed = next != *selected, *selected = next;
        if (gs_ui_key(ui, SDLK_SPACE, 0) || gs_ui_key(ui, SDLK_RETURN, 0) || gs_ui_key(ui, SDLK_KP_ENTER, 0)) w->open = !w->open;
    }
    if (ui->marked && !focused) w->open = false;  // Tab has moved on
    gs_ui_round(ui, r, ui->style.radius, hot || w->open ? ui->style.border : ui->style.raised);
    gs_rect label = gs_inset(r, 8);
    gs_ui_arrow(ui, gs_cut_right(&label, 12), ui->style.muted);
    gs_ui_text(ui, label, *selected >= 0 && *selected < count ? items[*selected] : "", ui->style.text_px, ui->style.text, -1);
    mark(ui, focused, r, ui->style.radius);
    if (gs_ui_clicked(ui, r)) w->open = !w->open;
    if (!w->open) return changed;
    float row = ui->style.row;
    gs_rect list = { r.x, r.y + r.h + 2, r.w, row * count };
    if (list.y + list.h > ui->area.h) list.y = r.y - list.h - 2;  // above when there is no room below
    ui->in_float = true;
    gs_ui_round(ui, list, ui->style.radius, ui->style.panel);
    gs_ui_outline(ui, list, ui->style.radius, 1, ui->style.border);
    for (int i = 0; i < count; i++) {
        gs_rect item = { list.x, list.y + i * row, list.w, row };
        if (gs_ui_hovered(ui, item) || i == *selected) gs_ui_round(ui, gs_inset(item, 1), ui->style.radius, i == *selected ? ui->style.raised : ui->style.border);
        gs_ui_text(ui, gs_inset(item, 8), items[i], ui->style.text_px, ui->style.text, -1);
        if (gs_ui_clicked(ui, item)) changed = changed || *selected != i, *selected = i, w->open = false;
    }
    ui->in_float = false;
    ui->float_now = list, ui->float_now_on = true;
    if (w->open && ((ui->in.released && !gs_rect_contains(list, ui->in.x, ui->in.y) && !gs_rect_contains(r, ui->in.x, ui->in.y)) || gs_ui_key(ui, SDLK_ESCAPE, 0)))
        w->open = false;
    return changed;
}

void gs_ui_tooltip(gs_ui *ui, gs_rect r, const char *text) {
    if (!gs_ui_hovered(ui, r) || ui->in.down) return;
    uint32_t h = hash(text);
    if (ui->tip != h) ui->tip = h, ui->tip_since = ui->now;
    ui->tip_wanted = true, ui->tip_rect = r;
    SDL_strlcpy(ui->tip_text, text, sizeof ui->tip_text);
}

void gs_ui_menu_toggle(gs_ui *ui, const char *id) {
    widget *w = state(ui, hash(id));
    w->open = !w->open, w->opened = ui->frame, w->cursor = 0;
}

bool gs_ui_menu_begin(gs_ui *ui, const char *id, gs_rect anchor, float width) {
    widget *w = state(ui, hash(id));
    if (!w->open) return false;
    ui->menu = hash(id);
    // Up and Down move a highlight through its rows (w->cursor, from 1; the rows were counted last
    // frame into w->anchor), and Space or Enter chooses the highlighted one.
    int n = w->anchor;
    while (n && gs_ui_key(ui, SDLK_DOWN, 0)) w->cursor = w->cursor % n + 1;
    while (n && gs_ui_key(ui, SDLK_UP, 0)) w->cursor = w->cursor <= 1 ? n : w->cursor - 1;
    ui->menu_choose = w->cursor && w->opened != ui->frame && (gs_ui_key(ui, SDLK_SPACE, 0) || gs_ui_key(ui, SDLK_RETURN, 0) || gs_ui_key(ui, SDLK_KP_ENTER, 0));
    ui->menu_at = w->cursor, ui->menu_count = 0;
    ui->menu_rect = (gs_rect){ fminf(anchor.x, ui->area.w - width - 4), anchor.y + anchor.h + 2, width, 0 };
    ui->menu_y = ui->menu_rect.y;
    ui->in_float = true;
    // Its background goes first; its height is known once the items are in.
    ui->menu_background = ui->nfl;
    defer(ui, FL_ROUND, ui->menu_rect, ui->style.panel, NULL, 0, 0)->radius = ui->style.radius;
    return true;
}

bool gs_ui_menu_item(gs_ui *ui, const char *label, bool enabled) {
    gs_rect item = { ui->menu_rect.x, ui->menu_y, ui->menu_rect.w, ui->style.row };
    ui->menu_y += item.h;
    bool at = ++ui->menu_count == ui->menu_at;
    if (enabled && (at || gs_ui_hovered(ui, item))) gs_ui_round(ui, gs_inset(item, 1), ui->style.radius, ui->style.border);
    gs_ui_text(ui, gs_inset(item, 10), label, ui->style.text_px, enabled ? ui->style.text : ui->style.muted, -1);
    bool chosen = enabled && (gs_ui_clicked(ui, item) || (at && ui->menu_choose));
    if (chosen) state(ui, ui->menu)->open = false;
    return chosen;
}

void gs_ui_menu_end(gs_ui *ui) {
    ui->menu_rect.h = ui->menu_y - ui->menu_rect.y;
    if (ui->menu_background < ui->nfl) ui->fl[ui->menu_background].r.h = ui->menu_rect.h;
    gs_ui_outline(ui, ui->menu_rect, ui->style.radius, 1, ui->style.border);
    ui->in_float = false;
    ui->float_now = ui->menu_rect, ui->float_now_on = true;
    widget *w = state(ui, ui->menu);
    w->anchor = ui->menu_count;
    // A click elsewhere or Esc closes it; not the click that opened it, in this same frame.
    if (w->opened != ui->frame && ui->in.released && !gs_rect_contains(ui->menu_rect, ui->in.x, ui->in.y)) w->open = false;
    if (gs_ui_key(ui, SDLK_ESCAPE, 0)) w->open = false;
}

static void list_begin(gs_ui *ui, gs_rect area, bool focused) {
    ui->list_area = area, ui->list_marked = focused && ui->marked;
    ui->list_clip = clip_push(ui, area);
}

gs_ui_rows gs_ui_list(gs_ui *ui, gs_rect area, const char *id, int count, float row_h, int *selected) {
    uint32_t h = hash(id);
    widget *w = state(ui, h);
    gs_ui_rows rows = { 0 };
    float max_scroll = fmaxf(0, count * row_h - area.h);
    bool moved = false, focused = stop_at(ui, h, area);
    if (gs_ui_hovered(ui, area) && ui->in.wheel && max_scroll > 0) w->scroll -= ui->in.wheel * row_h * 1.5f, ui->in.wheel = 0;  // (taken from a page it is in)
    if (focused && count) {
        int page = (int)fmaxf(1, area.h / row_h - 1), sel = *selected;
        while (gs_ui_key(ui, SDLK_DOWN, 0)) sel++, moved = true;  // (every press since the last frame)
        while (gs_ui_key(ui, SDLK_UP, 0)) sel--, moved = true;
        while (gs_ui_key(ui, SDLK_PAGEDOWN, 0)) sel += page, moved = true;
        while (gs_ui_key(ui, SDLK_PAGEUP, 0)) sel -= page, moved = true;
        if (gs_ui_key(ui, SDLK_HOME, 0)) sel = 0, moved = true;
        if (gs_ui_key(ui, SDLK_END, 0)) sel = count - 1, moved = true;
        *selected = sel < 0 ? 0 : sel >= count ? count - 1 : sel;
        if (*selected >= 0 && (gs_ui_key(ui, SDLK_RETURN, 0) || gs_ui_key(ui, SDLK_KP_ENTER, 0))) rows.activated = true;
    }
    if (*selected >= count) *selected = count - 1;
    if (moved && *selected >= 0) {  // keep the selection in view
        float top = *selected * row_h, bottom = top + row_h;
        if (top < w->scroll) w->scroll = top;
        if (bottom > w->scroll + area.h) w->scroll = bottom - area.h;
    }
    w->scroll = fminf(max_scroll, fmaxf(0, w->scroll));
    if (gs_ui_clicked(ui, area)) {
        int i = (int)((ui->in.y - area.y + w->scroll) / row_h);
        if (i >= 0 && i < count) *selected = i, rows.activated = true;
        if (ui->focus != h) gs_ui_focus(ui, id);
    }
    rows.first = (int)(w->scroll / row_h);
    rows.end = SDL_min(count, (int)((w->scroll + area.h) / row_h) + 1);
    rows.y = area.y - w->scroll;
    list_begin(ui, area, focused);
    return rows;
}

void gs_ui_list_end(gs_ui *ui) {
    if (ui->list_marked) gs_ui_outline(ui, ui->list_area, ui->style.radius, 2, ui->style.focus);  // the focus mark, inside its edge
    clip_pop(ui, ui->list_clip);
}

gs_ui_rows gs_ui_flow(gs_ui *ui, gs_rect area, const char *id, int count, const float *heights) {
    widget *w = state(ui, hash(id));
    gs_ui_rows rows = { 0 };
    bool focused = stop_at(ui, hash(id), area);
    float total = 0;
    for (int i = 0; i < count; i++) total += heights[i];
    for (int i = 0; i < w->prepended && i < count; i++) w->scroll += heights[i];  // (the rows in front pushed the place down)
    w->prepended = 0;
    float max_scroll = fmaxf(0, total - area.h), page = area.h * 0.9f;
    if (!w->away) w->scroll = max_scroll;
    if (w->show) {  // a row asked for: its middle at the area's middle, as far as the ends allow
        float top = 0;
        int row = SDL_min(w->show, count) - 1;
        for (int i = 0; i < row; i++) top += heights[i];
        if (row >= 0) w->scroll = top + heights[row] / 2 - area.h / 2;
        w->show = 0;
    }
    bool over = gs_ui_hovered(ui, area);
    if (over && ui->in.wheel && max_scroll > 0) w->scroll -= ui->in.wheel * ui->style.row * 1.5f, ui->in.wheel = 0;
    if (over || focused) {
        while (gs_ui_key(ui, SDLK_PAGEUP, 0)) w->scroll -= page;
        while (gs_ui_key(ui, SDLK_PAGEDOWN, 0)) w->scroll += page;
    }
    if (focused) {
        if (gs_ui_key(ui, SDLK_HOME, 0)) w->scroll = 0;
        if (gs_ui_key(ui, SDLK_END, 0)) w->scroll = max_scroll;
    }
    w->scroll = fminf(max_scroll, fmaxf(0, w->scroll));
    w->away = w->scroll < max_scroll - 0.5f;  // scrolled off the end: new rows no longer carry it along
    float y = 0;
    while (rows.first < count && y + heights[rows.first] <= w->scroll) y += heights[rows.first++];
    rows.y = area.y + y - w->scroll + fmaxf(0, area.h - total);  // (rows fewer than fill the area sit at its bottom)
    rows.end = rows.first;
    for (float bottom = y; rows.end < count && bottom < w->scroll + area.h; bottom += heights[rows.end++]) {}
    rows.above = w->scroll, rows.below = max_scroll - w->scroll;
    list_begin(ui, area, focused);
    return rows;
}

void gs_ui_flow_prepended(gs_ui *ui, const char *id, int n) { state(ui, hash(id))->prepended = n; }
void gs_ui_flow_show(gs_ui *ui, const char *id, int row) { state(ui, hash(id))->show = SDL_max(0, row) + 1; }



// ---- An image view ----

void gs_ui_image_view(gs_ui *ui, gs_rect r, const char *id, SDL_Texture *t) {
    uint32_t h = hash(id);
    widget *w = state(ui, h);
    bool focused = stop_at(ui, h, r);
    float tw, th, s = ui->scale;
    if (!t || !SDL_GetTextureSize(t, &tw, &th) || tw <= 0 || th <= 0 || r.w <= 0 || r.h <= 0) return;
    // Zoom is screen pixels to an image pixel: 1 shows the image at its own size. Fitted, it is as
    // large as the area lets it be, and no larger than its own size.
    float fit = fminf(1, fminf(r.w * s / tw, r.h * s / th)), most = fmaxf(8, fit);
    if (!w->zoom) w->zoom = fit, w->fitted = true, w->cx = tw / 2, w->cy = th / 2;
    float before = w->zoom, px = (ui->in.x - (r.x + r.w / 2)) * s, py = (ui->in.y - (r.y + r.h / 2)) * s;  // the pointer, in screen pixels from the centre
    bool over = gs_ui_hovered(ui, r);
    if (over && ui->in.wheel) w->zoom *= powf(1.2f, ui->in.wheel), ui->in.wheel = 0;
    if (pressed(ui, r)) {
        ui->drag = h;
        // A second press soon after the first, in the same place: between fitted and the image's own size.
        if (ui->press_before && ui->press_at - ui->press_before < 400 && fabsf(ui->press_x - ui->before_x) < 4 && fabsf(ui->press_y - ui->before_y) < 4)
            w->zoom = w->zoom > fit * 1.01f ? fit : 1, ui->press_at = 0;
        w->want_x = ui->in.x, w->grab = ui->in.y;  // (where the drag last was)
    }
    if (focused) {  // + and - zoom about the centre, 0 fits, 1 is its own size, and the arrows move it
        bool keyed = false;
        while (gs_ui_key(ui, SDLK_EQUALS, 0) || gs_ui_key(ui, SDLK_PLUS, 0) || gs_ui_key(ui, SDLK_KP_PLUS, 0)) w->zoom *= 1.2f, keyed = true;
        while (gs_ui_key(ui, SDLK_MINUS, 0) || gs_ui_key(ui, SDLK_KP_MINUS, 0)) w->zoom /= 1.2f, keyed = true;
        if (gs_ui_key(ui, SDLK_0, 0)) w->zoom = fit, keyed = true;
        if (gs_ui_key(ui, SDLK_1, 0)) w->zoom = 1, keyed = true;
        float step = 60 * s / w->zoom;
        while (gs_ui_key(ui, SDLK_LEFT, 0)) w->cx -= step;
        while (gs_ui_key(ui, SDLK_RIGHT, 0)) w->cx += step;
        while (gs_ui_key(ui, SDLK_UP, 0)) w->cy -= step;
        while (gs_ui_key(ui, SDLK_DOWN, 0)) w->cy += step;
        if (keyed) px = py = 0;
    }
    if (w->fitted && w->zoom == before) w->zoom = fit;  // (the area changed size)
    w->zoom = SDL_clamp(w->zoom, fit, most);
    if (w->zoom != before) {  // the image's point under the pointer stays under it
        w->cx += px / before - px / w->zoom, w->cy += py / before - py / w->zoom;
        w->fitted = w->zoom <= fit * 1.001f;
    }
    if (ui->drag == h && ui->in.down) {
        w->cx -= (ui->in.x - w->want_x) * s / w->zoom, w->cy -= (ui->in.y - w->grab) * s / w->zoom;
        w->want_x = ui->in.x, w->grab = ui->in.y;
    }
    // Held so that the image fills the area where it is larger than it, and is centred where it is smaller.
    float half_w = r.w * s / w->zoom / 2, half_h = r.h * s / w->zoom / 2;
    w->cx = half_w >= tw / 2 ? tw / 2 : SDL_clamp(w->cx, half_w, tw - half_w);
    w->cy = half_h >= th / 2 ? th / 2 : SDL_clamp(w->cy, half_h, th - half_h);
    if (over && w->zoom > fit * 1.001f) ui->cursor_want = 3;
    clip_state outer = clip_push(ui, r);
    SDL_FRect dst = { (r.x + r.w / 2) * s - w->cx * w->zoom, (r.y + r.h / 2) * s - w->cy * w->zoom, tw * w->zoom, th * w->zoom };
    float alpha = 1, fade = ui->opacity * (ui->dim ? DIM : 1);
    SDL_GetTextureAlphaModFloat(t, &alpha);
    if (fade < 1) SDL_SetTextureAlphaModFloat(t, alpha * fade);
    SDL_RenderTexture(ui->r, t, NULL, &dst);
    if (fade < 1) SDL_SetTextureAlphaModFloat(t, alpha);
    clip_pop(ui, outer);
    if (focused && ui->marked) gs_ui_outline(ui, r, ui->style.radius, 2, ui->style.focus);
}

float gs_ui_image_zoom(gs_ui *ui, const char *id) { return state(ui, hash(id))->zoom; }

// ---- A hotkey field ----

// Electron's name for a key, or NULL for one that cannot be part of a hotkey.
static const char *accelerator_key(SDL_Scancode sc, char *buf, size_t size) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return snprintf(buf, size, "%c", 'A' + (int)(sc - SDL_SCANCODE_A)), buf;
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return snprintf(buf, size, "%c", '1' + (int)(sc - SDL_SCANCODE_1)), buf;
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return snprintf(buf, size, "num%c", '1' + (int)(sc - SDL_SCANCODE_KP_1)), buf;
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return snprintf(buf, size, "F%d", 1 + (int)(sc - SDL_SCANCODE_F1)), buf;
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) return snprintf(buf, size, "F%d", 13 + (int)(sc - SDL_SCANCODE_F13)), buf;
    switch (sc) {
    case SDL_SCANCODE_0: return "0";
    case SDL_SCANCODE_KP_0: return "num0";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "Enter";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_KP_PERIOD: return "numdec";
    case SDL_SCANCODE_KP_PLUS: return "numadd";
    case SDL_SCANCODE_KP_MINUS: return "numsub";
    case SDL_SCANCODE_KP_MULTIPLY: return "nummult";
    case SDL_SCANCODE_KP_DIVIDE: return "numdiv";
    case SDL_SCANCODE_SEMICOLON: return ";";
    case SDL_SCANCODE_APOSTROPHE: return "'";
    case SDL_SCANCODE_COMMA: return ",";
    case SDL_SCANCODE_PERIOD: return ".";
    case SDL_SCANCODE_SLASH: return "/";
    case SDL_SCANCODE_BACKSLASH: return "\\";
    case SDL_SCANCODE_LEFTBRACKET: return "[";
    case SDL_SCANCODE_RIGHTBRACKET: return "]";
    case SDL_SCANCODE_GRAVE: return "`";
    case SDL_SCANCODE_MINUS: return "-";
    case SDL_SCANCODE_EQUALS: return "=";
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Down";
    case SDL_SCANCODE_LEFT: return "Left";
    case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_END: return "End";
    case SDL_SCANCODE_PAGEUP: return "PageUp";
    case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    case SDL_SCANCODE_INSERT: return "Insert";
    case SDL_SCANCODE_DELETE: return "Delete";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    default: return NULL;
    }
}

bool gs_ui_accelerator(SDL_Scancode key, SDL_Keymod mods, char *out, size_t size) {
    char buf[16];
    const char *name = accelerator_key(key, buf, sizeof buf);
    bool ctrl = mods & SDL_KMOD_CTRL, gui = mods & SDL_KMOD_GUI, alt = mods & SDL_KMOD_ALT, shift = mods & SDL_KMOD_SHIFT;
    if (!name || !(ctrl || gui || alt || shift)) return false;
#ifdef __APPLE__
    const char *ctrl_name = "Control+", *gui_name = "Command+";
#else
    const char *ctrl_name = "CommandOrControl+", *gui_name = "Super+";
#endif
    snprintf(out, size, "%s%s%s%s%s", ctrl ? ctrl_name : "", gui ? gui_name : "", alt ? "Alt+" : "", shift ? "Shift+" : "", name);
    return true;
}

bool gs_ui_capturing(const gs_ui *ui) { return ui->capture != 0; }

bool gs_ui_hotkey(gs_ui *ui, gs_rect r, const char *id, const char *shown, const char *placeholder, char *out, size_t size) {
    uint32_t h = hash(id);
    bool focused = stop_at(ui, h, r), got = false;
    if (ui->capture == h && (ui->focus != h || inert(ui) || (ui->in.pressed && !gs_rect_contains(r, ui->in.x, ui->in.y)))) ui->capture = 0;  // the keyboard has gone elsewhere, or a click has
    if (ui->capture == h && ui->captured[0]) SDL_strlcpy(out, ui->captured, size), got = true, ui->capture = 0;
    else if (ui->capture != h && (gs_ui_clicked(ui, r) || act(ui, focused))) set_focus(ui, h), ui->capture = h, focused = true;
    bool waiting = ui->capture == h;
    if (waiting) ui->capture_drawn = true;
    const char *text = waiting ? "Press keys\xE2\x80\xA6" : got ? out : shown && *shown ? shown : placeholder ? placeholder : "";
    bool set = waiting || got || (shown && *shown);
    gs_ui_round(ui, r, ui->style.radius, ui->style.raised);
    gs_ui_outline(ui, r, ui->style.radius, 1, waiting ? ui->style.accent : gs_ui_hovered(ui, r) ? ui->style.muted : ui->style.border);
    gs_ui_text(ui, (gs_rect){ r.x + 10, r.y, fmaxf(0, r.w - 20), r.h }, text, ui->style.text_px, waiting ? ui->style.accent : set ? ui->style.text : ui->style.muted, -1);
    if (!waiting) mark(ui, focused, r, ui->style.radius);
    return got;
}

// ---- A page ----

#define PAGE_BAR 10  // the width of the strip at a page's right edge that takes the pointer for its scroll thumb

static gs_rect page_thumb(const widget *w) {
    gs_rect v = w->view;
    float h = fmaxf(28, v.h * v.h / w->extent), max = w->extent - v.h;
    return (gs_rect){ v.x + v.w - 7, v.y + (v.h - h) * (max > 0 ? w->scroll / max : 0), 4, h };
}

gs_rect gs_ui_page_begin(gs_ui *ui, gs_rect area, const char *id) {
    uint32_t h = hash(id);
    widget *w = state(ui, h);
    w->view = area;
    float max = fmaxf(0, w->extent - area.h);
    if (max > 0) {  // the thumb, dragged
        gs_rect thumb = page_thumb(w), bar = { area.x + area.w - PAGE_BAR, area.y, PAGE_BAR, area.h };
        if (pressed(ui, bar)) ui->drag = h, w->grab = gs_rect_contains((gs_rect){ bar.x, thumb.y, bar.w, thumb.h }, ui->in.x, ui->in.y) ? ui->in.y - thumb.y : thumb.h / 2;
        if (ui->drag == h && area.h > thumb.h) w->scroll = (ui->in.y - w->grab - area.y) / (area.h - thumb.h) * max, w->eased = false;
    }
    if (w->eased) {  // gliding to where gs_ui_page_scroll_to sent it
        float to = SDL_clamp(w->to, 0, max), d = to - w->scroll;
        if (fabsf(d) < 0.5f) w->scroll = to, w->eased = false;
        else w->scroll += d * (1 - expf(-(float)(ui->now - w->since) / 70)), wake_within(ui, 16);
        w->since = ui->now;
    }
    w->scroll = SDL_clamp(w->scroll, 0, max);
    ui->page = h;
    ui->page_clip = clip_push(ui, area);
    return (gs_rect){ area.x, area.y - floorf(w->scroll), area.w, 1e6f };
}

void gs_ui_page_end(gs_ui *ui, float bottom) {
    widget *w = state(ui, ui->page);
    gs_rect area = w->view;
    float extent = fmaxf(0, bottom - (area.y - floorf(w->scroll))), before = w->scroll;
    if (fabsf(extent - w->extent) > 0.5f) w->extent = extent, wake_within(ui, 0);  // (the next frame knows how far it scrolls)
    float max = fmaxf(0, w->extent - area.h);
    // What its content left of the wheel and the keys is the page's, from the next frame on.
    if (gs_ui_hovered(ui, area)) {
        if (ui->in.wheel) w->scroll -= ui->in.wheel * 48, ui->in.wheel = 0, w->eased = false;
        while (gs_ui_key(ui, SDLK_PAGEDOWN, 0)) w->scroll += area.h * 0.9f, w->eased = false;
        while (gs_ui_key(ui, SDLK_PAGEUP, 0)) w->scroll -= area.h * 0.9f, w->eased = false;
    }
    w->scroll = SDL_clamp(w->scroll, 0, max);
    if (w->scroll != before) wake_within(ui, 0);
    if (max > 0) {
        gs_rect thumb = page_thumb(w), bar = { area.x + area.w - PAGE_BAR, area.y, PAGE_BAR, area.h };
        SDL_FColor c = ui->drag == ui->page || gs_ui_hovered(ui, bar) ? ui->style.text : ui->style.muted;
        c.a *= 0.5f;
        gs_ui_round(ui, thumb, 2, c);
    }
    clip_pop(ui, ui->page_clip);
    ui->page = 0;
}

float gs_ui_page_scroll(gs_ui *ui, const char *id) { return state(ui, hash(id))->scroll; }

void gs_ui_page_scroll_to(gs_ui *ui, const char *id, float y, bool glide) {
    widget *w = state(ui, hash(id));
    if (glide) w->to = y, w->eased = true, w->since = ui->now;
    else w->scroll = y, w->eased = false;
    wake_within(ui, 0);
}

// ---- A modal ----

gs_rect gs_ui_modal_begin(gs_ui *ui, float width, float height) {
    bool first = !ui->modal_last;
    gs_ui_clip(ui, NULL);
    ui->opacity = 1, ui->dim = false, ui->page = 0;
    ui->in_modal = ui->modal_now = true;
    ui->disabled = first;  // the click or key that opened it is not an answer to it
    if (first) ui->modal_focus = ui->focus, set_focus(ui, 0), wake_within(ui, 0);
    fill_now(ui, ui->area, (SDL_FColor){ 0, 0, 0, 0.55f });
    width = fminf(width, ui->area.w - 32), height = fminf(height, ui->area.h - 32);
    gs_rect box = { (ui->area.w - width) / 2, (ui->area.h - height) / 2, width, height };
    gs_ui_round(ui, box, ui->style.radius * 2, ui->style.panel);
    gs_ui_outline(ui, box, ui->style.radius * 2, 1, ui->style.border);
    return box;
}

void gs_ui_modal_end(gs_ui *ui) { ui->in_modal = ui->disabled = false; }

int gs_ui_confirm(gs_ui *ui, const char *question, const char *yes, const char *no, bool danger) {
    float size = ui->style.text_px, pad = 20, row_h = ui->style.row;
    float width = fminf(420, ui->area.w - 32), text_h = gs_ui_text_height(ui, question, size, width - 2 * pad);
    uint32_t yes_id = hash("gs_ui confirm yes"), no_id = hash("gs_ui confirm no");
    bool first = !ui->modal_last;
    gs_rect box = gs_ui_modal_begin(ui, width, text_h + row_h + 3 * pad);
    if (first) set_focus(ui, no_id), ui->marked = true;  // Enter on a question just asked declines it
    gs_ui_text_wrapped(ui, (gs_rect){ box.x + pad, box.y + pad, box.w - 2 * pad, text_h }, question, size, ui->style.text);
    gs_rect row = { box.x + pad, box.y + box.h - pad - row_h, box.w - 2 * pad, row_h };
    int answer = 0;
    if (button(ui, gs_cut_right(&row, gs_ui_text_width(ui, yes, size) + 28), yes_id, yes, danger ? &ui->style.warning : &ui->style.accent)) answer = 1;
    gs_cut_right(&row, 10);
    if (button(ui, gs_cut_right(&row, gs_ui_text_width(ui, no, size) + 28), no_id, no, NULL)) answer = -1;
    // The question has the keyboard whether or not the mark shows: Enter or Space answers with the
    // button that has the focus, and Esc declines.
    if (!first && !answer && (gs_ui_key(ui, SDLK_RETURN, 0) || gs_ui_key(ui, SDLK_KP_ENTER, 0) || gs_ui_key(ui, SDLK_SPACE, 0))) answer = ui->focus == yes_id ? 1 : -1;
    if (!first && gs_ui_key(ui, SDLK_ESCAPE, 0)) answer = -1;
    gs_ui_modal_end(ui);
    if (answer) ui->modal_now = false, wake_within(ui, 0);  // answered: the window is live again in the next frame
    return answer;
}

void gs_ui_end(gs_ui *ui) {
    if (ui->tip_wanted) {
        uint64_t rest = ui->now - ui->tip_since;
        if (rest >= TOOLTIP_MS) {
            // One line, or wrapped at TOOLTIP_W; below what it describes, or above when there is no room.
            float size = ui->style.small_px, lh = gs_ui_line_height(ui, size), one = gs_ui_text_width(ui, ui->tip_text, size);
            const gs_wrap *wrap = one > TOOLTIP_W ? wrap_of(ui, ui->tip_text, size, TOOLTIP_W) : NULL;
            int lines = wrap ? wrap->count : 1;
            float w = (wrap ? TOOLTIP_W : one) + 16, h = lines * lh + 8;
            gs_rect r = { fmaxf(4, fminf(ui->tip_rect.x, ui->area.w - w - 4)), ui->tip_rect.y + ui->tip_rect.h + 4, w, h };
            if (r.y + r.h > ui->area.h - 4) r.y = ui->tip_rect.y - r.h - 4;
            fl_item *f = defer(ui, FL_ROUND, r, ui->style.raised, NULL, 0, 0);
            f->radius = ui->style.radius;
            f = defer(ui, FL_OUTLINE, r, ui->style.border, NULL, 0, 0);
            f->radius = ui->style.radius, f->width = 1;
            for (int i = 0; i < lines; i++) {
                const char *line = wrap ? piece(ui, ui->tip_text, wrap->lines[i].start, wrap->lines[i].end) : ui->tip_text;
                defer(ui, FL_TEXT, (gs_rect){ r.x + 8, r.y + 4 + i * lh, w - 16, lh }, ui->style.text, line, size, -1);
            }
        } else if (ui->wait_ms < 0 || (int)(TOOLTIP_MS - rest) < ui->wait_ms) {
            ui->wait_ms = (int)(TOOLTIP_MS - rest);
        }
    } else {
        ui->tip = 0;
    }
    gs_ui_clip(ui, NULL);
    for (int i = 0; i < ui->nfl; i++) {
        fl_item *f = &ui->fl[i];
        if (f->kind == FL_FILL) fill_now(ui, f->r, f->c);
        else if (f->kind == FL_FRAME) frame_now(ui, f->r, f->c);
        else if (f->kind == FL_ROUND) round_now(ui, f->r, f->radius, f->c);
        else if (f->kind == FL_OUTLINE) outline_now(ui, f->r, f->radius, f->width, f->c);
        else ui->fonts = f->fonts, text_now(ui, f->r, f->text, f->px, f->c, f->align);
    }
    if (gs_glyphs_waiting(ui->glyphs)) ui->wait_ms = 0;  // a glyph is missing from this frame: the next one has it
    if (ui->modal_last && !ui->modal_now) ui->wait_ms = 0;   // a modal has gone: the next frame takes input again
    if (ui->focus_moved) ui->wait_ms = 0, ui->focus_moved = false;  // a control given the keyboard takes it when it is next drawn: text input starts then, before the first key
    if (ui->had_input) ui->wait_ms = 0;  // what a click or a key changed shows from the next frame, in the controls drawn before the one that took it
    if (!ui->capture_drawn) ui->capture = 0;  // (a hotkey field no longer drawn waits for nothing)
    if (ui->cursor_want != ui->cursor_now) {  // the pointer's shape, for a link or a field under it
        static const SDL_SystemCursor shapes[4] = { SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_POINTER, SDL_SYSTEM_CURSOR_TEXT, SDL_SYSTEM_CURSOR_MOVE };
        int want = ui->cursor_now = ui->cursor_want;
        if (want && !ui->cursors[want]) ui->cursors[want] = SDL_CreateSystemCursor(shapes[want]);
        SDL_SetCursor(want ? ui->cursors[want] : SDL_GetDefaultCursor());
    }
}

// ---- The window between runs ----

bool gs_window_state_load(const char *path, gs_window_state *ws) {
    char *text = SDL_LoadFile(path, NULL);
    int max = 0, full = 0;
    gs_window_state s = *ws;
    int n = text ? sscanf(text, "%d %d %d %d %d %d", &s.x, &s.y, &s.w, &s.h, &max, &full) : 0;
    SDL_free(text);
    if (n < 5 || s.w < 200 || s.h < 150) return false;
    s.maximized = max != 0, s.fullscreen = n >= 6 && full != 0;
    *ws = s;
    return true;
}

bool gs_window_state_save(const char *path, const gs_window_state *ws) {
    char text[120], tmp[1200];
    int n = snprintf(text, sizeof text, "%d %d %d %d %d %d\n", ws->x, ws->y, ws->w, ws->h, ws->maximized, ws->fullscreen);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    return SDL_SaveFile(tmp, text, (size_t)n) && SDL_RenamePath(tmp, path);
}

bool gs_window_state_track(gs_window_state *ws, SDL_Window *win, const SDL_Event *e) {
    if (e->type != SDL_EVENT_WINDOW_MOVED && e->type != SDL_EVENT_WINDOW_RESIZED && e->type != SDL_EVENT_WINDOW_MAXIMIZED &&
        e->type != SDL_EVENT_WINDOW_RESTORED && e->type != SDL_EVENT_WINDOW_ENTER_FULLSCREEN && e->type != SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
        return false;
    SDL_WindowFlags f = SDL_GetWindowFlags(win);
    if (!(f & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN))) {  // the size to come back to
        SDL_GetWindowPosition(win, &ws->x, &ws->y);
        SDL_GetWindowSize(win, &ws->w, &ws->h);
    }
    ws->maximized = f & SDL_WINDOW_MAXIMIZED, ws->fullscreen = f & SDL_WINDOW_FULLSCREEN;
    return true;
}

void gs_window_state_apply(const gs_window_state *ws, SDL_Window *win) {
    int n;
    bool shown = false;
    SDL_DisplayID *ids = ws->x != (int)SDL_WINDOWPOS_CENTERED ? SDL_GetDisplays(&n) : NULL;
    for (int i = 0; ids && i < n && !shown; i++) {
        SDL_Rect r;  // a window whose title bar would be off every display goes back to the centre
        shown = SDL_GetDisplayBounds(ids[i], &r) && ws->x + 40 >= r.x && ws->x + 40 < r.x + r.w && ws->y + 10 >= r.y && ws->y + 10 < r.y + r.h;
    }
    SDL_free(ids);
    SDL_SetWindowSize(win, ws->w, ws->h);
    if (shown) SDL_SetWindowPosition(win, ws->x, ws->y);
    if (ws->maximized) SDL_MaximizeWindow(win);
    if (ws->fullscreen) SDL_SetWindowFullscreen(win, true);
}
