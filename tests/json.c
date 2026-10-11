#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "gs_json.h"
#include "test.h"

void test_json(void) {
    const char *text = "{\"data\":{\"streams\":[{\"user_login\":\"lirik\",\"viewer_count\":12345,\"title\":\"caf\\u00e9 \\ud83d\\ude00\"}]},\"ok\":true}";
    gs_json *doc = gs_json_parse(text, strlen(text));
    CHECK(doc != NULL);
    gs_jv root = gs_json_root(doc);
    CHECK(!strcmp(gs_json_str(gs_json_path(root, "data.streams.0.user_login"), ""), "lirik"));
    CHECK(gs_json_num(gs_json_path(root, "data.streams.0.viewer_count"), 0) == 12345);
    CHECK(!strcmp(gs_json_str(gs_json_path(root, "data.streams.0.title"), ""), "caf\xc3\xa9 \xf0\x9f\x98\x80"));
    CHECK(gs_json_bool(gs_json_get(root, "ok"), false));
    CHECK(gs_json_path(root, "data.streams.1") == NULL);
    CHECK(gs_json_count(gs_json_path(root, "data.streams")) == 1);
    gs_json_free(doc);
    CHECK(gs_json_parse("{\"a\":", 5) == NULL);

    // A value built, changed and written, compact and indented.
    gs_jval *v = gs_jval_object();
    gs_jval_set(v, "name", gs_jval_str("caf\xc3\xa9 \"q\"\n\x01"));
    gs_jval_set_path(v, "overlay.x", gs_jval_num(12));
    gs_jval_set_path(v, "overlay.scale", gs_jval_num(0.1));
    gs_jval *list = gs_jval_array();
    gs_jval_push(list, gs_jval_bool(true)), gs_jval_push(list, gs_jval_null()), gs_jval_push(list, gs_jval_num(-1e21));
    gs_jval_set(v, "list", list);
    gs_jval_set(v, "gone", gs_jval_num(1)), gs_jval_remove(v, "gone");
    char *out = gs_jval_write(v, false);
    CHECK(out && !strcmp(out, "{\"name\":\"caf\xc3\xa9 \\\"q\\\"\\n\\u0001\",\"overlay\":{\"x\":12,\"scale\":0.1},\"list\":[true,null,-1e+21]}"));
    gs_jval *back = out ? gs_jval_parse(out, strlen(out)) : NULL;  // what is written reads back the same
    CHECK(back && gs_jval_equal(v, back));
    free(out);
    out = gs_jval_write(gs_jval_get(v, "overlay"), true);
    CHECK(out && !strcmp(out, "{\n  \"x\": 12,\n  \"scale\": 0.1\n}"));
    free(out);
    CHECK(gs_jval_number(gs_jval_path(v, "overlay.x"), 0) == 12 && gs_jval_count(v) == 3 && !strcmp(gs_jval_key(v, 1), "overlay"));
    CHECK(gs_jval_kind(gs_jval_at(list, 1)) == GS_JSON_NULL && gs_jval_boolean(gs_jval_at(list, 0), false) && gs_jval_path(v, "overlay.none") == NULL);

    // Saved settings laid over their defaults: a wrong kind gives way, an unknown key is kept.
    const char *defaults = "{\"volume\":5,\"theme\":{\"dark\":true,\"size\":14},\"keys\":[1,2]}", *saved = "{\"volume\":\"loud\",\"theme\":{\"size\":16},\"keys\":[3],\"extra\":1}";
    gs_jval *base = gs_jval_parse(defaults, strlen(defaults)), *over = gs_jval_parse(saved, strlen(saved)), *merged = gs_jval_merge(base, over);
    out = gs_jval_write(merged, false);
    CHECK(out && !strcmp(out, "{\"volume\":5,\"theme\":{\"dark\":true,\"size\":16},\"keys\":[3],\"extra\":1}"));
    free(out);

    // To a file and back: the folder is made, the file replaced whole, and no temporary file is left.
    char *dir = SDL_GetPrefPath("gesso", "tests");
    if (dir) {
        char folder[1024], path[1100];
        SDL_snprintf(folder, sizeof folder, "%sjson-%llu", dir, (unsigned long long)SDL_GetTicksNS());
        SDL_snprintf(path, sizeof path, "%s/settings.json", folder);
        CHECK(gs_jval_save(path, v, true) && gs_jval_save(path, merged, true));
        gs_jval *loaded = gs_jval_load(path);
        CHECK(loaded && gs_jval_equal(loaded, merged));
        int count = 0;
        char **names = SDL_GlobDirectory(folder, NULL, 0, &count);
        CHECK(count == 1);
        SDL_free(names);
        CHECK(gs_jval_load(folder) == NULL);
        gs_jval_free(loaded);
        SDL_RemovePath(path), SDL_RemovePath(folder);
        SDL_free(dir);
    }
    gs_jval_free(v), gs_jval_free(back), gs_jval_free(base), gs_jval_free(over), gs_jval_free(merged);
}
