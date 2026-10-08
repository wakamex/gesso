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
}
