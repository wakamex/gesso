#include "gs_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATES 128     // widgets with state between frames (ids hashed into a small table)
#define FLOATING 64    // rows drawn in the floating layer per frame
#define KEYS 32        // key presses kept per frame
#define TOOLTIP_MS 500
#define BLINK_MS 530

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
} widget;

typedef enum { FL_FILL, FL_TEXT, FL_FRAME } fl_kind;
typedef struct { fl_kind kind; gs_rect r; SDL_FColor c; char text[160]; float px; int align; } fl_item;

typedef struct {
    float x, y, wheel;
    bool down, pressed, released;
    SDL_Keycode keys[KEYS];
    SDL_Keymod mods[KEYS];
    int nkeys;
    char text[256];
} input;

struct gs_ui {
    SDL_Window *win;
    SDL_Renderer *r;
    gs_glyphs *glyphs;
    gs_fontset *fonts;
    gs_ui_style style;
    float scale;
    gs_rect area;
    uint64_t frame, now;
    input pending, in;
    bool taken[KEYS];
    float press_x, press_y;  // where the left button went down
    char composition[128];   // the input method's text being composed
    uint32_t focus, drag;    // the widget with the keyboard; the slider being dragged
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
    uint32_t tip;            // the tooltip's anchor
    uint64_t tip_since;
    gs_rect tip_rect;
    char tip_text[160];
    bool tip_wanted;
    int wait_ms;
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
    ui->win = win, ui->r = r, ui->fonts = fonts;
    ui->glyphs = gs_glyphs_new(r, 2048);
    ui->style = (gs_ui_style){
        .background = { 0.067f, 0.075f, 0.071f, 1 }, .panel = { 0.098f, 0.110f, 0.106f, 1 }, .raised = { 0.153f, 0.169f, 0.165f, 1 },
        .text = { 0.92f, 0.93f, 0.92f, 1 }, .muted = { 0.59f, 0.61f, 0.60f, 1 }, .accent = { 0.208f, 0.761f, 0.647f, 1 },
        .accent_text = { 0.04f, 0.07f, 0.06f, 1 }, .border = { 0.22f, 0.24f, 0.235f, 1 }, .warning = { 0.95f, 0.60f, 0.29f, 1 },
        .text_px = 14, .small_px = 12, .row = 30,
    };
    return ui;
}

void gs_ui_free(gs_ui *ui) {
    if (!ui) return;
    gs_glyphs_free(ui->glyphs);
    free(ui);
}

gs_ui_style *gs_ui_style_of(gs_ui *ui) { return &ui->style; }
gs_glyphs *gs_ui_glyphs(gs_ui *ui) { return ui->glyphs; }
float gs_ui_scale(const gs_ui *ui) { return ui->scale; }
int gs_ui_wait_ms(const gs_ui *ui) { return ui->wait_ms; }

bool gs_ui_event(gs_ui *ui, const SDL_Event *e) {
    input *p = &ui->pending;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: p->x = e->motion.x, p->y = e->motion.y; return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) p->down = p->pressed = true, p->x = e->button.x, p->y = e->button.y;
        return false;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) p->down = false, p->released = true, p->x = e->button.x, p->y = e->button.y;
        return false;
    case SDL_EVENT_MOUSE_WHEEL: p->wheel += e->wheel.y; return false;
    case SDL_EVENT_KEY_DOWN:  // (whether a key is taken is settled while drawing: see gs_ui_key)
        if (p->nkeys < KEYS) p->keys[p->nkeys] = e->key.key, p->mods[p->nkeys++] = e->key.mod;
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
    if (ui->in.pressed) ui->press_x = ui->in.x, ui->press_y = ui->in.y;
    memset(ui->taken, 0, sizeof ui->taken);
    ui->float_last = ui->float_now, ui->float_last_on = ui->float_now_on;
    ui->float_now_on = false, ui->nfl = 0, ui->tip_wanted = false, ui->wait_ms = -1;
    if (!down) ui->drag = 0;
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

static void defer(gs_ui *ui, fl_kind kind, gs_rect r, SDL_FColor c, const char *text, float size, int align) {
    if (ui->nfl == FLOATING) return;
    fl_item *f = &ui->fl[ui->nfl++];
    *f = (fl_item){ kind, r, c, "", size, align };
    if (text) SDL_strlcpy(f->text, text, sizeof f->text);
}

void gs_ui_fill(gs_ui *ui, gs_rect r, SDL_FColor c) { ui->in_float ? defer(ui, FL_FILL, r, c, NULL, 0, 0) : fill_now(ui, r, c); }
void gs_ui_frame(gs_ui *ui, gs_rect r, SDL_FColor c) { ui->in_float ? defer(ui, FL_FRAME, r, c, NULL, 0, 0) : frame_now(ui, r, c); }

float gs_ui_text(gs_ui *ui, gs_rect r, const char *text, float size, SDL_FColor c, int align) {
    if (ui->in_float) return defer(ui, FL_TEXT, r, c, text, size, align), gs_ui_text_width(ui, text, size);
    return text_now(ui, r, text, size, c, align);
}

void gs_ui_texture(gs_ui *ui, gs_rect r, SDL_Texture *t, const SDL_FRect *src) {
    float tw, th;
    if (!t || !SDL_GetTextureSize(t, &tw, &th)) return;
    SDL_FRect s = src ? *src : (SDL_FRect){ 0, 0, tw, th };
    float want = r.w / r.h, have = s.w / s.h;  // crop the source to the target's aspect
    if (have > want) s.x += (s.w - s.h * want) / 2, s.w = s.h * want;
    else if (have < want) s.y += (s.h - s.w / want) / 2, s.h = s.w / want;
    SDL_FRect d = px(ui, r);
    SDL_RenderTexture(ui->r, t, &s, &d);
}

void gs_ui_clip(gs_ui *ui, const gs_rect *r) {
    if (!r) { SDL_SetRenderClipRect(ui->r, NULL); return; }
    SDL_FRect f = px(ui, *r);
    SDL_Rect c = { (int)floorf(f.x), (int)floorf(f.y), (int)ceilf(f.w), (int)ceilf(f.h) };
    SDL_SetRenderClipRect(ui->r, &c);
}

// ---- Input ----

// The pointer is free for a widget unless the floating layer covered it last frame (widgets in that
// layer, drawn while in_float, are the ones it belongs to).
static bool pointer_over(gs_ui *ui, gs_rect r, float x, float y) {
    if (!ui->in_float && ui->float_last_on && gs_rect_contains(ui->float_last, x, y)) return false;
    return gs_rect_contains(r, x, y);
}

bool gs_ui_hovered(gs_ui *ui, gs_rect r) { return pointer_over(ui, r, ui->in.x, ui->in.y); }
bool gs_ui_clicked(gs_ui *ui, gs_rect r) { return ui->in.released && pointer_over(ui, r, ui->in.x, ui->in.y) && pointer_over(ui, r, ui->press_x, ui->press_y); }
static bool pressed(gs_ui *ui, gs_rect r) { return ui->in.pressed && pointer_over(ui, r, ui->in.x, ui->in.y); }

bool gs_ui_key(gs_ui *ui, SDL_Keycode key, SDL_Keymod mods) {
    for (int i = 0; i < ui->in.nkeys; i++)
        if (!ui->taken[i] && ui->in.keys[i] == key && (mods ? (ui->in.mods[i] & mods) != 0 : !(ui->in.mods[i] & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))))
            return ui->taken[i] = true;
    return false;
}

void gs_ui_focus(gs_ui *ui, const char *id) {
    uint32_t h = id ? hash(id) : 0;
    if (h == ui->focus) return;
    ui->focus = h, ui->caret_since = ui->now;
    if (SDL_TextInputActive(ui->win)) SDL_StopTextInput(ui->win);
    ui->composition[0] = 0;
}

bool gs_ui_focused(gs_ui *ui, const char *id) { return ui->focus == hash(id); }

// ---- Widgets ----

bool gs_ui_button(gs_ui *ui, gs_rect r, const char *label) {
    bool hot = gs_ui_hovered(ui, r);
    gs_ui_fill(ui, r, hot ? ui->style.border : ui->style.raised);
    gs_ui_text(ui, gs_inset(r, 6), label, ui->style.text_px, ui->style.text, 0);
    return gs_ui_clicked(ui, r);
}

bool gs_ui_toggle(gs_ui *ui, gs_rect r, const char *label, bool *on) {
    gs_rect box = { r.x, r.y + (r.h - 16) / 2, 16, 16 };
    gs_ui_fill(ui, box, *on ? ui->style.accent : ui->style.raised);
    gs_ui_frame(ui, box, gs_ui_hovered(ui, r) ? ui->style.text : ui->style.border);
    if (*on) gs_ui_fill(ui, gs_inset(box, 4), ui->style.accent_text);
    gs_rect rest = r;
    gs_cut_left(&rest, 22);
    gs_ui_text(ui, rest, label, ui->style.text_px, ui->style.text, -1);
    bool hit = gs_ui_clicked(ui, r);
    if (hit) *on = !*on;
    return hit;
}

bool gs_ui_slider(gs_ui *ui, gs_rect r, const char *id, float *value, float min, float max) {
    uint32_t h = hash(id);
    if (pressed(ui, r)) ui->drag = h;
    float before = *value;
    if (ui->drag == h && max > min) *value = fminf(max, fmaxf(min, min + (ui->in.x - r.x) / r.w * (max - min)));
    float t = max > min ? (*value - min) / (max - min) : 0;
    gs_rect track = { r.x, r.y + r.h / 2 - 2, r.w, 4 };
    gs_ui_fill(ui, track, ui->style.raised);
    gs_ui_fill(ui, (gs_rect){ track.x, track.y, track.w * t, track.h }, ui->style.accent);
    gs_ui_fill(ui, (gs_rect){ r.x + r.w * t - 5, r.y + r.h / 2 - 7, 10, 14 }, gs_ui_hovered(ui, r) || ui->drag == h ? ui->style.text : ui->style.muted);
    return *value != before;
}

// The field's edits: typed text, and the keys that move the cursor or change the text.
static bool edit(gs_ui *ui, widget *w, char *buf, size_t size) {
    bool changed = false;
    size_t len = strlen(buf);
    if (w->cursor > (int)len) w->cursor = (int)len;
    char clean[512];
    const char *typed = ui->in.text;
    for (int i = 0; i < ui->in.nkeys; i++) {
        SDL_Keycode k = ui->in.keys[i];
        bool ctrl = ui->in.mods[i] & (SDL_KMOD_CTRL | SDL_KMOD_GUI);
        if (ctrl && k == SDLK_V) {
            char *paste = SDL_GetClipboardText();
            size_t n = 0;
            for (const char *p = paste ? paste : ""; *p && n + 1 < sizeof clean; p++) clean[n++] = *p == '\n' || *p == '\r' || *p == '\t' ? ' ' : *p;
            clean[n] = 0;
            SDL_free(paste);
            typed = clean;
        } else if (ctrl && k == SDLK_A) {
            w->all = true;
        } else if (ctrl && (k == SDLK_C || k == SDLK_X) && w->all && len) {
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

bool gs_ui_field(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder) {
    uint32_t h = hash(id);
    widget *w = state(ui, h);
    float s = ui->scale, pad = 8, text_px = ui->style.text_px;
    gs_rect inner = gs_inset(r, pad);
    inner.y = r.y, inner.h = r.h;
    if (pressed(ui, r)) {
        if (ui->focus != h) gs_ui_focus(ui, id);
        const gs_line *l = gs_fontset_line(ui->fonts, text_px * s, buf);
        w->cursor = l ? gs_line_hit(l, (ui->in.x - inner.x) * s, buf) : 0, w->all = false;
        ui->caret_since = ui->now;
    }
    if (ui->focus == h && ui->in.pressed && !gs_rect_contains(r, ui->in.x, ui->in.y)) gs_ui_focus(ui, NULL);  // a click elsewhere leaves it
    bool focused = ui->focus == h, changed = false;
    if (focused) {
        if (!SDL_TextInputActive(ui->win)) SDL_StartTextInput(ui->win);
        changed = edit(ui, w, buf, size);
    }
    gs_ui_fill(ui, r, ui->style.raised);
    gs_ui_frame(ui, r, focused ? ui->style.accent : gs_ui_hovered(ui, r) ? ui->style.muted : ui->style.border);
    gs_ui_clip(ui, &r);
    if (w->all && buf[0]) gs_ui_fill(ui, (gs_rect){ inner.x, r.y + 5, fminf(gs_ui_text_width(ui, buf, text_px), inner.w), r.h - 10 }, ui->style.border);
    if (buf[0] || ui->composition[0]) gs_ui_text(ui, (gs_rect){ inner.x, r.y, 1e6f, r.h }, buf, text_px, ui->style.text, -1);
    else if (placeholder) gs_ui_text(ui, inner, placeholder, text_px, ui->style.muted, -1);
    if (focused) {
        const gs_line *l = gs_fontset_line(ui->fonts, text_px * s, buf);
        float cx = inner.x + (l ? gs_line_caret(l, w->cursor) / s : 0);
        if (ui->composition[0]) {  // what the input method is composing, underlined at the cursor
            float cw = gs_ui_text(ui, (gs_rect){ cx, r.y, 1e6f, r.h }, ui->composition, text_px, ui->style.accent, -1);
            gs_ui_fill(ui, (gs_rect){ cx, r.y + r.h - 7, cw, 1 }, ui->style.accent);
            cx += cw;
        }
        uint64_t t = ui->now - ui->caret_since;
        if (t % (2 * BLINK_MS) < BLINK_MS) gs_ui_fill(ui, (gs_rect){ cx, r.y + 6, 1.5f, r.h - 12 }, ui->style.text);
        ui->wait_ms = (int)(BLINK_MS - t % BLINK_MS);
        SDL_Rect area = { (int)r.x, (int)r.y, (int)r.w, (int)r.h };  // where the input method's candidates go
        SDL_SetTextInputArea(ui->win, &area, (int)(cx - r.x));
    }
    gs_ui_clip(ui, NULL);
    return changed;
}

void gs_ui_arrow(gs_ui *ui, gs_rect r, SDL_FColor c) {
    float s = ui->scale, w = fminf(r.w, 8), cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    SDL_Vertex v[3] = { { { (cx - w / 2) * s, (cy - w / 4) * s }, c, { 0 } }, { { (cx + w / 2) * s, (cy - w / 4) * s }, c, { 0 } },
                        { { cx * s, (cy + w / 4) * s }, c, { 0 } } };
    SDL_RenderGeometry(ui->r, NULL, v, 3, NULL, 0);
}

bool gs_ui_dropdown(gs_ui *ui, gs_rect r, const char *id, const char *const *items, int count, int *selected) {
    widget *w = state(ui, hash(id));
    bool hot = gs_ui_hovered(ui, r);
    gs_ui_fill(ui, r, hot || w->open ? ui->style.border : ui->style.raised);
    gs_rect label = gs_inset(r, 8);
    gs_ui_arrow(ui, gs_cut_right(&label, 12), ui->style.muted);
    gs_ui_text(ui, label, *selected >= 0 && *selected < count ? items[*selected] : "", ui->style.text_px, ui->style.text, -1);
    if (gs_ui_clicked(ui, r)) w->open = !w->open;
    if (!w->open) return false;
    float row = ui->style.row;
    gs_rect list = { r.x, r.y + r.h + 2, r.w, row * count };
    if (list.y + list.h > ui->area.h) list.y = r.y - list.h - 2;  // above when there is no room below
    bool changed = false;
    ui->in_float = true;
    gs_ui_fill(ui, list, ui->style.panel);
    gs_ui_frame(ui, list, ui->style.border);
    for (int i = 0; i < count; i++) {
        gs_rect item = { list.x, list.y + i * row, list.w, row };
        if (gs_ui_hovered(ui, item) || i == *selected) gs_ui_fill(ui, gs_inset(item, 1), i == *selected ? ui->style.raised : ui->style.border);
        gs_ui_text(ui, gs_inset(item, 8), items[i], ui->style.text_px, ui->style.text, -1);
        if (gs_ui_clicked(ui, item)) changed = *selected != i, *selected = i, w->open = false;
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
    w->open = !w->open, w->opened = ui->frame;
}

bool gs_ui_menu_begin(gs_ui *ui, const char *id, gs_rect anchor, float width) {
    widget *w = state(ui, hash(id));
    if (!w->open) return false;
    ui->menu = hash(id);
    ui->menu_rect = (gs_rect){ fminf(anchor.x, ui->area.w - width - 4), anchor.y + anchor.h + 2, width, 0 };
    ui->menu_y = ui->menu_rect.y;
    ui->in_float = true;
    // Its background goes first; its height is known once the items are in.
    ui->menu_background = ui->nfl;
    defer(ui, FL_FILL, ui->menu_rect, ui->style.panel, NULL, 0, 0);
    return true;
}

bool gs_ui_menu_item(gs_ui *ui, const char *label, bool enabled) {
    gs_rect item = { ui->menu_rect.x, ui->menu_y, ui->menu_rect.w, ui->style.row };
    ui->menu_y += item.h;
    if (enabled && gs_ui_hovered(ui, item)) gs_ui_fill(ui, gs_inset(item, 1), ui->style.border);
    gs_ui_text(ui, gs_inset(item, 10), label, ui->style.text_px, enabled ? ui->style.text : ui->style.muted, -1);
    bool chosen = enabled && gs_ui_clicked(ui, item);
    if (chosen) state(ui, ui->menu)->open = false;
    return chosen;
}

void gs_ui_menu_end(gs_ui *ui) {
    ui->menu_rect.h = ui->menu_y - ui->menu_rect.y;
    if (ui->menu_background < ui->nfl) ui->fl[ui->menu_background].r.h = ui->menu_rect.h;
    defer(ui, FL_FRAME, ui->menu_rect, ui->style.border, NULL, 0, 0);
    ui->in_float = false;
    ui->float_now = ui->menu_rect, ui->float_now_on = true;
    widget *w = state(ui, ui->menu);
    // A click elsewhere or Esc closes it; not the click that opened it, in this same frame.
    if (w->opened != ui->frame && ui->in.released && !gs_rect_contains(ui->menu_rect, ui->in.x, ui->in.y)) w->open = false;
    if (gs_ui_key(ui, SDLK_ESCAPE, 0)) w->open = false;
}

gs_ui_rows gs_ui_list(gs_ui *ui, gs_rect area, const char *id, int count, float row_h, int *selected) {
    uint32_t h = hash(id);
    widget *w = state(ui, h);
    gs_ui_rows rows = { 0 };
    float max_scroll = fmaxf(0, count * row_h - area.h);
    if (gs_ui_hovered(ui, area) && ui->in.wheel) w->scroll -= ui->in.wheel * row_h * 1.5f;
    bool moved = false;
    if (ui->focus == h && count) {
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
    gs_ui_clip(ui, &area);
    return rows;
}

void gs_ui_list_end(gs_ui *ui) { gs_ui_clip(ui, NULL); }

void gs_ui_end(gs_ui *ui) {
    if (ui->tip_wanted) {
        uint64_t rest = ui->now - ui->tip_since;
        if (rest >= TOOLTIP_MS) {
            float w = gs_ui_text_width(ui, ui->tip_text, ui->style.small_px) + 16;
            gs_rect r = { fminf(ui->tip_rect.x, ui->area.w - w - 4), ui->tip_rect.y + ui->tip_rect.h + 4, w, 24 };
            defer(ui, FL_FILL, r, ui->style.raised, NULL, 0, 0);
            defer(ui, FL_FRAME, r, ui->style.border, NULL, 0, 0);
            defer(ui, FL_TEXT, gs_inset(r, 8), ui->style.text, ui->tip_text, ui->style.small_px, -1);
        } else if (ui->wait_ms < 0 || (int)(TOOLTIP_MS - rest) < ui->wait_ms) {
            ui->wait_ms = (int)(TOOLTIP_MS - rest);
        }
    } else {
        ui->tip = 0;
    }
    SDL_SetRenderClipRect(ui->r, NULL);
    for (int i = 0; i < ui->nfl; i++) {
        fl_item *f = &ui->fl[i];
        if (f->kind == FL_FILL) fill_now(ui, f->r, f->c);
        else if (f->kind == FL_FRAME) frame_now(ui, f->r, f->c);
        else text_now(ui, f->r, f->text, f->px, f->c, f->align);
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
