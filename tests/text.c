#include <string.h>

#include "gs_text.h"
#include "test.h"

void test_text(void) {
    // Grapheme steps: a letter with a combining accent, an emoji family joined by ZWJ, a flag.
    const char *t = "e\xCC\x81x\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xF0\x9F\x87\xA8\xF0\x9F\x87\xA6!";
    int a = gs_utf8_grapheme_next(t, 0);
    CHECK(a == 3);  // "e" and U+0301 together
    int b = gs_utf8_grapheme_next(t, a);
    CHECK(b == 4);
    int c = gs_utf8_grapheme_next(t, b);
    CHECK(c == 4 + 18);  // the whole family
    int d = gs_utf8_grapheme_next(t, c);
    CHECK(d == c + 8);  // both regional indicators
    CHECK(gs_utf8_grapheme_next(t, d) == (int)strlen(t));
    CHECK(gs_utf8_grapheme_prev(t, d) == c && gs_utf8_grapheme_prev(t, c) == b && gs_utf8_grapheme_prev(t, a) == 0);

    // Shaping with the system's fonts, where it has them.
    gs_fontset *fs = gs_fontset_system();
    gs_line *l = gs_fontset_shape(fs, 20, "Office");
    if (l && l->count) {
        CHECK(l->width > 0 && !l->rtl);
        for (int i = 1; i < l->count; i++) CHECK(l->glyphs[i].x >= l->glyphs[i - 1].x && l->glyphs[i].cluster > l->glyphs[i - 1].cluster);
        CHECK(gs_line_caret(l, 0) == 0 && gs_line_caret(l, 6) == l->width);
        CHECK(gs_line_hit(l, l->width + 5, "Office") == 6 && gs_line_hit(l, -5, "Office") == 0);
    }
    gs_line_free(l);
    // Arabic, where a font has it: a right-to-left line, its first letter drawn rightmost.
    l = gs_fontset_shape(fs, 20, "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 abc");
    if (l && l->count && l->glyphs[0].glyph) {
        CHECK(l->rtl);
        int rightmost = 0;
        for (int i = 0; i < l->count; i++) if (l->glyphs[i].x > l->glyphs[rightmost].x) rightmost = i;
        CHECK(l->glyphs[rightmost].cluster == 0);
    }
    gs_line_free(l);

    // Wrapping: lines break after spaces, at newlines and inside a word too long for one.
    float unit = gs_fontset_width(fs, 20, "hello");
    if (unit > 0) {
        gs_wrap *w = gs_fontset_wrap(fs, 20, "hello world foo", unit * 1.5f);
        CHECK(w && w->count == 3);
        if (w && w->count == 3) {
            CHECK(w->lines[0].start == 0 && w->lines[0].end == 5 && w->lines[1].start == 6 && w->lines[1].end == 11 && w->lines[2].start == 12 && w->lines[2].end == 15);
            CHECK(w->lines[0].width > 0 && w->lines[0].width <= unit * 1.5f);
        }
        gs_wrap_free(w);
        w = gs_fontset_wrap(fs, 20, "a\n\nb\n", 1e6f);
        CHECK(w && w->count == 4 && w->lines[1].start == 2 && w->lines[1].end == 2 && w->lines[2].start == 3 && w->lines[3].start == 5);  // (a line after the final newline)
        gs_wrap_free(w);
        const char *word = "abcdefghijklmnopqrstuvwxyz";
        w = gs_fontset_wrap(fs, 20, word, unit * 2);
        CHECK(w && w->count > 2);
        int at = 0;
        for (int i = 0; w && i < w->count; i++) {
            CHECK(w->lines[i].start == at && w->lines[i].end > at && w->lines[i].width <= unit * 2.01f);
            at = w->lines[i].end;
        }
        CHECK(at == (int)strlen(word));
        gs_wrap_free(w);
        // Emoji stay whole when a line breaks inside a run of them.
        const char *family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
        w = gs_fontset_wrap(fs, 20, family, 1);
        CHECK(w && w->count == 2 && w->lines[0].end == 18 && w->lines[1].start == 18);
        gs_wrap_free(w);
    }
    // A paragraph has one direction, from its first strong character, for all its lines.
    gs_wrap *dir = gs_fontset_wrap(fs, 20, "abc \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85\n\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 abc\n123", 1e6f);
    CHECK(dir && dir->count == 3 && !dir->lines[0].rtl && dir->lines[1].rtl && !dir->lines[2].rtl);
    gs_wrap_free(dir);
    if (unit > 0) {
        dir = gs_fontset_wrap(fs, 20, "abc \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85", unit);
        CHECK(dir && dir->count > 1);
        for (int i = 0; dir && i < dir->count; i++) CHECK(!dir->lines[i].rtl);
        gs_wrap_free(dir);
    }
    // Held to two lines, a longer text is cut and its last line leaves room for an ellipsis.
    if (unit > 0) {
        const char *longer = "one two three four five six seven eight nine ten eleven twelve";
        gs_wrap *all = gs_fontset_wrap(fs, 20, longer, unit * 4), *two = gs_fontset_wrap_lines(fs, 20, longer, unit * 4, 2);
        CHECK(all && two && all->count > 2 && !all->cut && two->count == 2 && two->cut);
        if (all && two && all->count > 2) {
            CHECK(two->lines[0].end == all->lines[0].end && two->lines[1].start == all->lines[1].start);
            CHECK(two->lines[1].end <= all->lines[1].end && two->lines[1].end > two->lines[1].start && two->lines[1].width <= unit * 4);
        }
        gs_wrap_free(all), gs_wrap_free(two);
        two = gs_fontset_wrap_lines(fs, 20, "short", unit * 4, 2);
        CHECK(two && two->count == 1 && !two->cut);
        gs_wrap_free(two);
    }
    // The system's face at a heavier weight is wider, and its monospace face gives every character one width.
    gs_fontset *regular = gs_fontset_system_weight(400), *bold = gs_fontset_system_weight(700), *mono = gs_fontset_system_mono(400);
    float w400 = gs_fontset_width(regular, 40, "Hamburgefonstiv"), w700 = gs_fontset_width(bold, 40, "Hamburgefonstiv");
    if (w400 > 0) CHECK(w700 > w400);
    float narrow = gs_fontset_width(mono, 40, "iii111"), wide = gs_fontset_width(mono, 40, "mmm000");
    if (narrow > 0 && wide > 0 && gs_fontset_width(mono, 40, "i") != gs_fontset_width(fs, 40, "i")) CHECK_NEAR(narrow, wide, 0.01);  // (where there is a monospace face)
    gs_fontset_free(regular), gs_fontset_free(bold), gs_fontset_free(mono);
    // An emoji font put first takes the emoji and leaves digits and spaces, which it often has too,
    // to the text font. Tried with the system's emoji font, or the one GS_TEST_EMOJI_FONT names.
    static const char *const emoji_fonts[] = { "/usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf", "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
                                               "C:\\Windows\\Fonts\\seguiemj.ttf", "/System/Library/Fonts/Apple Color Emoji.ttc" };
    const char *emoji_font = SDL_getenv("GS_TEST_EMOJI_FONT");
    for (size_t i = 0; !emoji_font && i < SDL_arraysize(emoji_fonts); i++)
        if (SDL_GetPathInfo(emoji_fonts[i], NULL)) emoji_font = emoji_fonts[i];
    gs_line *plain = gs_fontset_shape(fs, 20, "12:34 56");
    if (emoji_font && plain && plain->count) {
        const gs_font *text_font = plain->glyphs[0].font;
        float width = plain->width;
        gs_fontset_prepend(fs, emoji_font);
        gs_line *after = gs_fontset_shape(fs, 20, "12:34 56");
        CHECK(after && after->count == plain->count && after->width == width);
        for (int i = 0; after && i < after->count; i++) CHECK(after->glyphs[i].font == text_font);
        gs_line_free(after);
        // The marks keycaps are built on stay text too, and a keycap sequence is still the emoji font's.
        after = gs_fontset_shape(fs, 20, "#*");
        for (int i = 0; after && i < after->count; i++) CHECK(after->glyphs[i].font == text_font);
        gs_line_free(after);
        after = gs_fontset_shape(fs, 20, "1\xEF\xB8\x8F\xE2\x83\xA3");
        if (after && after->count && SDL_getenv("GS_TEST_EMOJI_FONT")) CHECK(after->glyphs[0].font != text_font);  // (a font known to have keycaps)
        gs_line_free(after);
    }
    gs_line_free(plain);
    gs_fontset_prepend(fs, "/no/such/font.ttf");  // (a missing file changes nothing)
    gs_wrap *empty = gs_fontset_wrap(fs, 20, "", 100);
    CHECK(empty && empty->count == 1 && empty->lines[0].start == 0 && empty->lines[0].end == 0);
    gs_wrap_free(empty);
    gs_fontset_free(fs);
}
