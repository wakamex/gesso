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
    gs_wrap *empty = gs_fontset_wrap(fs, 20, "", 100);
    CHECK(empty && empty->count == 1 && empty->lines[0].start == 0 && empty->lines[0].end == 0);
    gs_wrap_free(empty);
    gs_fontset_free(fs);
}
