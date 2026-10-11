// An immediate-mode interface for apps: layout by cutting rectangles, a few widgets, hit testing,
// one floating layer for dropdowns, menus and tooltips, and the window's state between runs.
// Everything is in points, which gs_ui turns into the display's pixels (SDL_GetWindowDisplayScale),
// so the interface keeps its size across displays and text is drawn at full resolution.
//
// Each frame: pass every event to gs_ui_event, then call gs_ui_begin, lay out and draw with the
// functions below, and gs_ui_end. Widgets that keep state between frames (a text field's cursor, a
// list's scroll, an open dropdown) take an id string, unique among those drawn in a frame.
//
// It provides rows and columns, scrolling lists (of equal or differing row heights), wrapped text,
// single-line and multi-line text fields with input-method composition, buttons, toggles, sliders,
// dropdowns, menus and tooltips. It does not provide rich text editing, menu bars, drag and drop, or
// themes beyond the colours in gs_ui_style.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#include "gs_text.h"

typedef struct { float x, y, w, h; } gs_rect;

// Cutting: each returns the part cut off and leaves the rest in *r.
gs_rect gs_cut_left(gs_rect *r, float w);
gs_rect gs_cut_right(gs_rect *r, float w);
gs_rect gs_cut_top(gs_rect *r, float h);
gs_rect gs_cut_bottom(gs_rect *r, float h);
gs_rect gs_inset(gs_rect r, float d);
bool gs_rect_contains(gs_rect r, float x, float y);

typedef struct {
    SDL_FColor background, panel, raised, text, muted, accent, accent_text, border, warning;
    float text_px, small_px, row;  // text sizes and a control's height, in points
} gs_ui_style;

typedef struct gs_ui gs_ui;

gs_ui *gs_ui_new(SDL_Window *win, SDL_Renderer *r, gs_fontset *fonts);
void gs_ui_free(gs_ui *ui);
gs_ui_style *gs_ui_style_of(gs_ui *ui);  // change its colours and sizes as wanted
gs_glyphs *gs_ui_glyphs(gs_ui *ui);

bool gs_ui_event(gs_ui *ui, const SDL_Event *e);  // true when the interface took the event (a key for a focused field, say)
gs_rect gs_ui_begin(gs_ui *ui);  // the window, in points
void gs_ui_end(gs_ui *ui);
// Milliseconds until the interface wants to be drawn again on its own (a blinking cursor, a tooltip
// about to show), or -1 when it waits for events; for apps that draw only when something changes.
// It is 0 after a frame that drew a glyph new to the atlas, which shows from the next frame on.
int gs_ui_wait_ms(const gs_ui *ui);
float gs_ui_scale(const gs_ui *ui);  // pixels per point

// ---- Drawing ----

void gs_ui_fill(gs_ui *ui, gs_rect r, SDL_FColor c);
void gs_ui_frame(gs_ui *ui, gs_rect r, SDL_FColor c);  // a one-point outline
// One line of text in r, vertically centred and cut with an ellipsis to fit; align -1 left, 0 centre,
// 1 right (a right-to-left line aligns to the right when align is -1). Returns its width.
float gs_ui_text(gs_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c, int align);
float gs_ui_text_width(gs_ui *ui, const char *text, float px);
// Text wrapped to the width of r, from the top of r down, in the base layer (not in a menu or
// dropdown). It breaks lines where Unicode allows (after spaces, between CJK characters), at each
// newline, and inside a word wider than the line; a right-to-left line aligns to the right. Returns
// its height, which gs_ui_text_height gives without drawing, for laying rows out before the frame
// draws them. Both keep a text's lines for the frames that ask again, so a few hundred texts can be
// drawn each frame without being broken into lines each time.
float gs_ui_text_wrapped(gs_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c);
float gs_ui_text_height(gs_ui *ui, const char *text, float px, float width);
float gs_ui_line_height(const gs_ui *ui, float px);  // the height of one line of wrapped text at size px
void gs_ui_texture(gs_ui *ui, gs_rect r, SDL_Texture *t, const SDL_FRect *src);  // fills r, cropping the source to keep its aspect
void gs_ui_clip(gs_ui *ui, const gs_rect *r);  // NULL ends clipping
void gs_ui_arrow(gs_ui *ui, gs_rect r, SDL_FColor c);  // a small downward triangle centred in r, as on a dropdown

// ---- Input ----

bool gs_ui_hovered(gs_ui *ui, gs_rect r);
bool gs_ui_clicked(gs_ui *ui, gs_rect r);  // the left button released over r after being pressed over it
bool gs_ui_key(gs_ui *ui, SDL_Keycode key, SDL_Keymod mods);  // pressed this frame and not taken by a focused field
void gs_ui_focus(gs_ui *ui, const char *id);  // gives a field or list the keyboard
bool gs_ui_focused(gs_ui *ui, const char *id);

// ---- Widgets ----

bool gs_ui_button(gs_ui *ui, gs_rect r, const char *label);
bool gs_ui_toggle(gs_ui *ui, gs_rect r, const char *label, bool *on);  // true when changed
bool gs_ui_slider(gs_ui *ui, gs_rect r, const char *id, float *value, float min, float max);  // true while it changes
// A single-line text field editing buf (UTF-8). True when the text changed this frame.
bool gs_ui_field(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder);
// A text field of several lines, editing buf (UTF-8) with the keys of gs_ui_field and more: the text
// wraps to r; Shift+Enter inserts a newline and Enter is left to the app (query it after drawing the
// field, which takes Shift+Enter first); Up and Down keep their column; Home and End go to the
// line's ends, with Ctrl to the text's; Ctrl+Left, Ctrl+Right and Ctrl+Backspace/Delete go by word;
// Shift extends a selection, as does dragging with the mouse; Ctrl+A, C, X and V work on the
// selection (a paste keeps its newlines); Page Up and Page Down are the app's. It scrolls to keep the
// cursor in view when its text is taller than r, and gs_ui_textarea_height gives the height that
// fits the text up to max_lines lines for a given width, so a compose box can grow with what is typed.
// True when the text changed this frame.
bool gs_ui_textarea(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder);
float gs_ui_textarea_height(gs_ui *ui, const char *buf, float width, int max_lines);
// A button showing items[*selected] that opens a list of the items. True when the choice changed.
bool gs_ui_dropdown(gs_ui *ui, gs_rect r, const char *id, const char *const *items, int count, int *selected);
void gs_ui_tooltip(gs_ui *ui, gs_rect r, const char *text);  // shown after the pointer rests on r

// A menu in the floating layer below `anchor`: gs_ui_menu_toggle opens or closes it; while open,
// gs_ui_menu_begin returns true and gs_ui_menu_item adds rows (true when chosen, which closes it).
void gs_ui_menu_toggle(gs_ui *ui, const char *id);
bool gs_ui_menu_begin(gs_ui *ui, const char *id, gs_rect anchor, float width);
bool gs_ui_menu_item(gs_ui *ui, const char *label, bool enabled);
void gs_ui_menu_end(gs_ui *ui);

// A scrolling list of `count` rows of equal height in `area`. Scrolls with the wheel; with the
// keyboard (when focused) Up, Down, Page Up, Page Down, Home and End move *selected and keep it in view,
// and Enter activates it, as does a click on a row (which selects it). The rows to draw are
// [first, end), row i at y + i * row_h; draw them clipped to area, then call gs_ui_list_end.
// (`above` and `below` are the points of the rows hidden above and below the area, which gs_ui_flow
// fills in.)
typedef struct { int first, end; float y; bool activated; float above, below; } gs_ui_rows;
gs_ui_rows gs_ui_list(gs_ui *ui, gs_rect area, const char *id, int count, float row_h, int *selected);
void gs_ui_list_end(gs_ui *ui);

// A scrolling list of `count` rows whose heights differ, heights[i] points for row i from the top
// (for wrapped text, from gs_ui_text_height). It scrolls with the wheel, and with Page Up and Page
// Down while the pointer is over it or it has the keyboard (gs_ui_focus), and Home and End when it
// has. Rows fewer than fill the area sit at its bottom. While it is scrolled to the end it stays
// there as rows are added, and elsewhere it keeps what it shows, so a chat gets new messages at the
// end without moving a reader up in the history. Rows added at the top move what is shown down:
// call gs_ui_flow_prepended with how many, before the gs_ui_flow call whose heights include them, to
// keep the view where it was. Give each list that should remember its own place an id of its own (a
// chat per conversation, say). Draw rows [first, end) from y down, then call gs_ui_list_end. When
// `above` is small the reader has reached the top of the rows held: time to load older ones.
gs_ui_rows gs_ui_flow(gs_ui *ui, gs_rect area, const char *id, int count, const float *heights);
void gs_ui_flow_prepended(gs_ui *ui, const char *id, int n);

// ---- The window between runs ----

typedef struct {
    int x, y, w, h;  // the size and position when neither maximised nor full screen
    bool maximized, fullscreen;
} gs_window_state;

bool gs_window_state_load(const char *path, gs_window_state *ws);  // a text file "x y w h maximized fullscreen"
bool gs_window_state_save(const char *path, const gs_window_state *ws);
// Follows the window's moves and size changes; true when *ws changed and is worth saving.
bool gs_window_state_track(gs_window_state *ws, SDL_Window *win, const SDL_Event *e);
// Moves and sizes the window as it was, if its position is on a display now, and maximises it or
// makes it full screen as it was. Call before showing the window.
void gs_window_state_apply(const gs_window_state *ws, SDL_Window *win);
