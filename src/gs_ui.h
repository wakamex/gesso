// An immediate-mode interface for apps: layout by cutting rectangles, a few widgets, hit testing,
// one floating layer for dropdowns, menus and tooltips, a Tab order with a visible focus mark, and
// the window's state between runs.
// Everything is in points, which gs_ui turns into the display's pixels (SDL_GetWindowDisplayScale),
// so the interface keeps its size across displays and text is drawn at full resolution.
//
// Each frame: pass every event to gs_ui_event, then call gs_ui_begin, lay out and draw with the
// functions below, and gs_ui_end. Widgets that keep state between frames (a text field's cursor, a
// list's scroll, an open dropdown) take an id string, unique among those drawn in a frame.
//
// A process can have several windows, each with a gs_ui of its own on that window's renderer, sharing
// one gs_fontset: pass every event to every gs_ui, and each takes those of its own window and leaves
// the rest (an event that names no window goes to all). Each gs_ui keeps a glyph atlas for its
// renderer. Use them from one thread.
//
// It provides rows and columns, scrolling lists (of equal or differing row heights), a scrolling
// page of controls, wrapped text, single-line and multi-line text fields with input-method
// composition, masked and number fields, buttons, links, toggles, switches, radio buttons, sliders,
// dropdowns, menus, tooltips, modal questions, a field that captures a hotkey and a view of a
// picture that zooms and moves. It does not provide rich text editing, menu bars,
// drag and drop, or themes beyond the colours in gs_ui_style.
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
    float radius;                  // of the controls' corners, in points; 0 (the default) for square ones
    float button_radius;           // of buttons' corners where they differ from the other controls' (a large value for pills); below 0 (the default), the radius above
    SDL_FColor focus;              // the ring around the control the keyboard has reached; the accent by default
    float line_height;             // from one line of wrapped text to the next, as a multiple of the text's size; 0 (the default) for the font's own
} gs_ui_style;

typedef struct gs_ui gs_ui;

// The window comes back where it was and at its size: gs_ui_new moves it as it was left, gs_ui_event
// follows its moves, and its place is saved when it has rested half a second and when the interface
// is freed. Create the window hidden and show it after gs_ui_new, so it does not jump. The place is
// kept in window.txt in the app's data folder (SDL_GetPrefPath for the creator and name given to
// SDL_SetAppMetadata, the program's name by default), for the first interface the program makes. Set
// the window's GS_UI_PLACE string property before gs_ui_new to name another file in that folder (one
// per window, for an app with several), to give a path (a name with a slash), or to "" for none.
// Windows under SDL's offscreen and dummy drivers are not placed unless the property names a file.
#define GS_UI_PLACE "gesso.place"
gs_ui *gs_ui_new(SDL_Window *win, SDL_Renderer *r, gs_fontset *fonts);
void gs_ui_free(gs_ui *ui);
gs_ui_style *gs_ui_style_of(gs_ui *ui);  // change its colours and sizes as wanted
gs_glyphs *gs_ui_glyphs(gs_ui *ui);

// True when the interface took the event (text typed into a focused field, say). Events of another
// window are left alone, and a pointer that leaves the window is over nothing in it.
bool gs_ui_event(gs_ui *ui, const SDL_Event *e);
gs_rect gs_ui_begin(gs_ui *ui);  // the window, in points
void gs_ui_end(gs_ui *ui);
// Milliseconds until the interface wants to be drawn again on its own (a blinking cursor, a tooltip
// about to show), or -1 when it waits for events; for apps that draw only when something changes.
// It is 0 after a frame that drew a glyph new to the atlas, which shows from the next frame on.
// It is also 0 after a frame that was handed a click, the wheel, a key or text: what that changed is
// drawn in full from the next frame, so an app that draws on events draws once more after each.
// And 0 after the keyboard was given to another control (gs_ui_focus, Tab, a click), which takes it
// when it is next drawn: without that frame, a field focused by the app would miss the first key typed.
int gs_ui_wait_ms(const gs_ui *ui);
// Asks for another frame within `ms`, for something the app draws that changes by itself (a
// progress bar, a clock): gs_ui_wait_ms after this frame is no more than that.
void gs_ui_wake(gs_ui *ui, int ms);
float gs_ui_scale(const gs_ui *ui);  // pixels per point

// ---- Drawing ----

void gs_ui_fill(gs_ui *ui, gs_rect r, SDL_FColor c);
void gs_ui_frame(gs_ui *ui, gs_rect r, SDL_FColor c);  // a one-point outline
// Smooth shapes (gs_draw.h, here in points): a rounded rectangle filled, its outline `width` points
// thick inside its edge, and a disc.
void gs_ui_round(gs_ui *ui, gs_rect r, float radius, SDL_FColor c);
void gs_ui_outline(gs_ui *ui, gs_rect r, float radius, float width, SDL_FColor c);
void gs_ui_circle(gs_ui *ui, float cx, float cy, float radius, SDL_FColor c);
// Everything drawn from here on, controls included, is faded to this opacity, until it is set again
// or the frame ends (each frame starts at 1): for a panel that fades in, or a part shown dimmed.
void gs_ui_opacity(gs_ui *ui, float opacity);
// A bar filled to `fraction` (0 to 1) in the accent colour, with round ends.
void gs_ui_progress(gs_ui *ui, gs_rect r, float fraction);
// A value that moves to `target` over `ms` milliseconds, fast at first and settling (gs_ease), and
// keeps frames coming while it moves. Call it each frame with where the value should be and draw
// with what it returns: a panel's height, an opacity, a knob's position. The first call for an id
// returns the target at once.
float gs_ui_ease(gs_ui *ui, const char *id, float target, int ms);
// One line of text in r, vertically centred and cut with an ellipsis to fit; align -1 left, 0 centre,
// 1 right (a right-to-left line aligns to the right when align is -1). Returns its width.
float gs_ui_text(gs_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c, int align);
float gs_ui_text_width(gs_ui *ui, const char *text, float px);
// Text drawn and measured from here on uses `fonts` (a heavier weight for a heading, a monospace
// face for a timer: gs_fontset_system_weight, gs_fontset_system_mono), until it is set again or the
// frame ends; NULL goes back to the fonts the gs_ui was made with. The controls' own text follows it
// too, so set it back before drawing them. The font set must outlive the frames that use it.
void gs_ui_font(gs_ui *ui, gs_fontset *fonts);
// Text wrapped to the width of r, from the top of r down, in the base layer (not in a menu or
// dropdown). It breaks lines where Unicode allows (after spaces, between CJK characters), at each
// newline, and inside a word wider than the line; a right-to-left line aligns to the right. Returns
// its height, which gs_ui_text_height gives without drawing, for laying rows out before the frame
// draws them. Both keep a text's lines for the frames that ask again, so a few hundred texts can be
// drawn each frame without being broken into lines each time.
float gs_ui_text_wrapped(gs_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c);
// The same text, which the pointer can select in and which has ranges in a colour of their own: a
// link, a mention. Ranges are byte offsets [start, end) into the text; where two overlap the first
// counts. The app keeps what is selected: *from is where the drag began and *to where the pointer is,
// in either order, and nothing is selected when they are equal. A press on the text sets both to
// the place pressed and a drag moves *to, so a text whose pair changed is the one being selected in
// and the app can empty the others'. A double click selects the word pressed on (gs_utf8_word) and a
// triple click its paragraph, between newlines; a drag from either grows the selection by whole words
// or paragraphs. The system's double-click time and distance apply. What is between them is drawn on the style's border colour, in a
// line that mixes directions too. *clicked is the range marked `link` that the button went down and
// came up on with nothing selected, or -1; the pointer is a hand over one and a text beam elsewhere.
// Copying what is selected and opening a link are the app's. The id tells this text's drag from
// another's; an id per row drawn is enough.
typedef struct { int start, end; SDL_FColor color; bool link; } gs_ui_text_range;
float gs_ui_text_select(gs_ui *ui, gs_rect r, const char *id, const char *text, float px, SDL_FColor c, const gs_ui_text_range *ranges, int count, int *from, int *to, int *clicked);
float gs_ui_text_height(gs_ui *ui, const char *text, float px, float width);
// Wrapped text held to `max_lines` lines: when it has more, the last line shown ends with an
// ellipsis and *cut (which may be NULL) is set, for showing the whole text some other way, in a
// tooltip, say. Returns the height of what it drew.
float gs_ui_text_lines(gs_ui *ui, gs_rect r, const char *text, float px, SDL_FColor c, int max_lines, bool *cut);
// The height of one line of wrapped text at size px: what the text font asks for (its ascent, descent
// and line gap; 1.4 times the size for a font set with no text font), or px times the style's
// line_height where the app has set one.
float gs_ui_line_height(const gs_ui *ui, float px);
void gs_ui_texture(gs_ui *ui, gs_rect r, SDL_Texture *t, const SDL_FRect *src);  // fills r, cropping the source to keep its aspect
// A picture to look at closely: the texture shown whole in r, then zoomed with the wheel about the
// pointer (to 8 times its own size at most), moved by dragging, and switched between fitted and its
// own size by a double click. With the keyboard, + and - zoom, 0 fits, 1 is its own size and the
// arrows move it. It starts fitted, never enlarged past its own size, and stays fitted as r changes
// until the user zooms. Give each picture an id of its own so each keeps its own place; a texture of
// the whole image comes from gs_images_full. gs_ui_image_zoom is screen pixels to an image pixel (1
// at its own size; 0 before the view has been drawn), for showing a percentage.
void gs_ui_image_view(gs_ui *ui, gs_rect r, const char *id, SDL_Texture *t);
float gs_ui_image_zoom(gs_ui *ui, const char *id);
// Clips drawing to r, and the pointer with it: a control cut off by the clip cannot be clicked where
// it does not show. NULL ends clipping. A list ends its own clip with gs_ui_list_end and puts back
// the one that was in force before it.
void gs_ui_clip(gs_ui *ui, const gs_rect *r);
void gs_ui_arrow(gs_ui *ui, gs_rect r, SDL_FColor c);  // a small downward triangle centred in r, as on a dropdown

// ---- Input ----

bool gs_ui_hovered(gs_ui *ui, gs_rect r);
bool gs_ui_clicked(gs_ui *ui, gs_rect r);  // the left button released over r after being pressed over it
bool gs_ui_key(gs_ui *ui, SDL_Keycode key, SDL_Keymod mods);  // pressed this frame and not taken by a focused field
void gs_ui_focus(gs_ui *ui, const char *id);  // gives a control the keyboard; NULL takes it from all
bool gs_ui_focused(gs_ui *ui, const char *id);

// ---- The keyboard ----
//
// Tab and Shift+Tab move the focus through every control in the order drawn, round and round, and
// gs_ui takes those keys. The control the keyboard has reached is marked with a ring in
// gs_ui_style.focus (a text field by its accent edge, a list by a ring inside its edge), and is
// worked with the keys: Space or Enter presses a button, a toggle, or opens a dropdown; the arrows
// move a slider (Home and End to its ends), change a dropdown's choice and move through an open
// menu's rows; a list and a text field take the keys they always did. A click hides the mark and
// gives Space and Enter back to the app until Tab is pressed again, so an app's own shortcuts on
// those keys keep working for someone using the mouse. Buttons and toggles are told apart by their
// labels, and those with the same label by the order they are drawn in.
//
// A control of the app's own design joins in with gs_ui_activated, which is all of a button but its
// look: true when r is clicked, or when Space or Enter is pressed while it has the keyboard. Draw the
// control, then its focus mark with gs_ui_focus_mark, a ring `radius` round at its corners.
bool gs_ui_activated(gs_ui *ui, gs_rect r, const char *id);
void gs_ui_focus_mark(gs_ui *ui, gs_rect r, const char *id, float radius);
// Where a control was drawn in the last frame, by its id (a button's or toggle's id is its label):
// false for one that was not drawn, or was disabled or under a modal. For a test that drives an app
// by clicking its controls by name, and for an app that points at one (a coach mark, a scroll to it).
bool gs_ui_where(gs_ui *ui, const char *id, gs_rect *r);
// Controls drawn while disabled are shown and take no pointer or keys, and Tab passes them by; with
// `dim` they are faded too. It lasts until it is set again or the frame ends.
void gs_ui_disable(gs_ui *ui, bool disabled, bool dim);

// ---- Widgets ----

bool gs_ui_button(gs_ui *ui, gs_rect r, const char *label);
// The same filled in the accent colour, for a window's main action, and in the warning colour, for
// one that cannot be undone.
bool gs_ui_button_primary(gs_ui *ui, gs_rect r, const char *label);
bool gs_ui_button_danger(gs_ui *ui, gs_rect r, const char *label);
bool gs_ui_toggle(gs_ui *ui, gs_rect r, const char *label, bool *on);  // true when changed
// A switch, r being the switch itself (40 by 22 points, say), whose knob slides. True when changed.
bool gs_ui_switch(gs_ui *ui, gs_rect r, const char *id, bool *on);
// One choice of several: a ring, filled when `selected`, and its label. True when it is chosen, which
// the app records by passing `selected` for it and not for the others from then on.
bool gs_ui_radio(gs_ui *ui, gs_rect r, const char *id, const char *label, bool selected);
// A slider of `count` positions, each a dot on its track; *value is 0 to count - 1. True when it changes.
bool gs_ui_stops(gs_ui *ui, gs_rect r, const char *id, int *value, int count);
// Text that is clicked, in the accent colour and underlined, with a hand for a pointer over it. True
// when clicked; opening what it links to (SDL_OpenURL) is the app's.
bool gs_ui_link(gs_ui *ui, gs_rect r, const char *label);
bool gs_ui_slider(gs_ui *ui, gs_rect r, const char *id, float *value, float min, float max);  // true while it changes
// A single-line text field editing buf (UTF-8). True when the text changed this frame.
//
// A field and a text area undo their own edits with Ctrl+Z (Cmd+Z on macOS) and redo them with
// Ctrl+Shift+Z or Ctrl+Y (Cmd+Shift+Z): typing a word at a time, or after a pause of a second; a run
// of Backspace or Delete as one; a paste, a cut, a selection deleted or typed over and Esc's clearing
// each as one. The cursor and selection go back with the text. A text the app puts in the buffer (a
// draft, a cleared box after sending) starts a fresh history, so what was there is not brought back.
// The last 4 controls edited keep 100 steps each; a secret field keeps none.
// gs_ui_edited makes an edit the app made on purpose (a name put in for a mention) a step of the
// control's own: call it after changing buf, with where the cursor goes; Ctrl+Z then brings back the
// text, cursor and selection the control had before, and goes on through the typing before that.
bool gs_ui_field(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder);
void gs_ui_edited(gs_ui *ui, const char *id, const char *buf, int cursor);
// The same with flags. A masked field shows a dot for each character and does not copy or cut, for
// a password or an API key; with GS_UI_REVEAL it shows its text while it has the keyboard. Esc empties
// a field and a second Esc is the app's; with GS_UI_KEEP it gives up the keyboard and keeps the text,
// as a field in a form should.
enum { GS_UI_MASKED = 1, GS_UI_REVEAL = 2, GS_UI_KEEP = 4 };
bool gs_ui_input(gs_ui *ui, gs_rect r, const char *id, char *buf, size_t size, const char *placeholder, int flags);
// A field for a number from min to max, written with `decimals` digits after the point. It takes
// digits, a minus sign and a point (a comma is read as one); Up and Down change it by `step`; and
// *value follows what is typed, held within the range. True when *value changed.
bool gs_ui_number(gs_ui *ui, gs_rect r, const char *id, double *value, double min, double max, double step, int decimals);
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
// What is selected in a text area, as byte offsets into its text: true with [*start, *end) when
// there is a selection, false with both at the cursor when there is none. gs_ui_textarea_select
// selects a range (the cursor at its end), or places the cursor when start and end are the same.
// For acting on the words a user picked: replacing them, looking them up.
bool gs_ui_textarea_selection(gs_ui *ui, const char *id, int *start, int *end);
void gs_ui_textarea_select(gs_ui *ui, const char *id, int start, int end);
// A button showing items[*selected] that opens a list of the items. True when the choice changed.
bool gs_ui_dropdown(gs_ui *ui, gs_rect r, const char *id, const char *const *items, int count, int *selected);
// Shown after the pointer rests on r, below it or above where there is no room: on one line, or
// wrapped when it is longer than 280 points, so a line cut short can show its whole text.
void gs_ui_tooltip(gs_ui *ui, gs_rect r, const char *text);

// A menu in the floating layer below `anchor`, or above it when there is no room below and there is
// above: gs_ui_menu_toggle opens or closes it; while open,
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
// Brings a row into view the next time the list with that id is drawn: its middle at the middle of
// the area, with rows above and below it, or as near as the list's ends allow. Call it before the
// gs_ui_flow call of the frame, which may be the first for that id. The list then counts as
// scrolled off its end, unless the row is in its last screenful, so rows added later do not pull it
// back down: for opening a conversation at a search result, or jumping to the message a reply answers.
void gs_ui_flow_show(gs_ui *ui, const char *id, int row);

// A field that takes a key combination, for choosing a hotkey. It shows `shown` (how the app writes
// the combination it holds, or the placeholder when that is empty) until it is clicked, or Space or
// Enter is pressed on it. Then it waits, showing "Press keys…", and every key pressed in the window
// is its own: a combination of at least one modifier and a key is written to `out` as an accelerator
// and the call returns true; Esc, Tab, a click elsewhere and the window losing the keyboard end the
// wait with nothing taken. While gs_ui_capturing is true the app should release the global hotkeys
// it holds, or the system delivers the user's presses to them and not to the window.
//
// An accelerator is modifiers and a key joined by +, as Electron writes them: "CommandOrControl+Shift+Space",
// "Alt+F5", "CommandOrControl+num3". Ctrl is CommandOrControl and the Windows or Super key is Super; on macOS
// Control is Control and Command is Command. gs_ui_accelerator writes one from a key press (false
// for a key alone, a modifier alone, or a key with no such name).
bool gs_ui_hotkey(gs_ui *ui, gs_rect r, const char *id, const char *shown, const char *placeholder, char *out, size_t size);
bool gs_ui_capturing(const gs_ui *ui);
bool gs_ui_accelerator(SDL_Scancode key, SDL_Keymod mods, char *out, size_t size);

// A page taller than its area: controls and text scrolled by the wheel, by Page Up and Page Down
// while the pointer is over it, and by a thumb at its right edge. gs_ui_page_begin returns the page,
// as wide as the area with its top where the scroll puts it, and taller than anything: cut the
// content from its top, draw it, then pass gs_ui_page_end the y where the content ended, which is
// how the page learns its height. What the area cuts off is not drawn and cannot be clicked, and Tab
// brings a control outside the area into it. A list or text area inside the page takes the wheel
// while the pointer is over it and it has further to scroll that way, and leaves it to the page once
// it is at its end. One page at a time: they do not nest.
//
// gs_ui_page_scroll is how far the page is scrolled, in points from its top, for an index beside it
// that follows the reader; gs_ui_page_scroll_to sets it, at once or gliding there.
gs_rect gs_ui_page_begin(gs_ui *ui, gs_rect area, const char *id);
void gs_ui_page_end(gs_ui *ui, float bottom);
float gs_ui_page_scroll(gs_ui *ui, const char *id);
void gs_ui_page_scroll_to(gs_ui *ui, const char *id, float y, bool glide);

// A modal: a panel in the middle of the window, which is darkened under it. While the app draws one
// each frame, everything outside it takes no pointer and no keys and is out of the Tab order.
// Draw it last, after the rest of the frame: gs_ui_modal_begin returns the panel to lay controls out
// in, and gs_ui_modal_end ends it. The keyboard goes back to what had it when the modal is no longer
// drawn; the frame after that is the first to take input again.
gs_rect gs_ui_modal_begin(gs_ui *ui, float width, float height);
void gs_ui_modal_end(gs_ui *ui);
// A question with two answers, as a modal. Call it each frame while the question stands, after the
// rest of the frame: 1 when `yes` is chosen, -1 for `no` or Esc, 0 until then. The keyboard starts on
// `no`, so Enter on a question just asked declines it. With `danger` the `yes` button is in the
// warning colour, for something that cannot be undone.
int gs_ui_confirm(gs_ui *ui, const char *question, const char *yes, const char *no, bool danger);
#define GS_UI_CONFIRM_YES "gs_ui confirm yes"  // the ids of its two buttons, for gs_ui_where
#define GS_UI_CONFIRM_NO "gs_ui confirm no"

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
