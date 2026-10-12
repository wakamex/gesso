// gs_notify. Nothing is shown unless GS_TEST_NOTIFY is set, since a test run should not put
// notifications on a developer's screen. Set to 1 it shows and takes back two real ones; set to the
// log of tests/tools/notification_service.py, running on the session bus, it also checks what the
// service was sent and that a click comes back.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_json.h"
#include "gs_notify.h"
#include "test.h"

static SDL_AtomicInt clicks;
static char clicked_tag[64];

static void on_click(const char *tag) {
    SDL_strlcpy(clicked_tag, tag, sizeof clicked_tag);
    SDL_AddAtomicInt(&clicks, 1);
}

void test_notify(void) {
    CHECK(!gs_notify_show("early", "Before gs_notify_init", "nothing is shown"));
    const char *mode = SDL_getenv("GS_TEST_NOTIFY");
    if (!mode) return;
    CHECK(gs_notify_init("gesso tests", "dev.wakamex.gesso.tests", NULL, on_click));
    CHECK(gs_notify_show("chat 1", "Ada", "First message [click]"));
    CHECK(gs_notify_show("chat 2", "Grace & <Hopper>", "Tom & <Jerry>"));
    for (int i = 0; i < 400 && strcmp(mode, "1") && !SDL_GetAtomicInt(&clicks); i++) SDL_Delay(5);
    CHECK(gs_notify_show("chat 1", "Ada", "Second message, in place of the first"));
    gs_notify_close("chat 2");
    gs_notify_close("no such tag");
    if (!strcmp(mode, "1")) return;

    CHECK(SDL_GetAtomicInt(&clicks) == 1 && !strcmp(clicked_tag, "chat 1"));
    SDL_Delay(300);  // (the service writes its log as the calls arrive)
    size_t len = 0;
    char *log = SDL_LoadFile(mode, &len);
    CHECK(log != NULL);
    int line = 0, first = 0;
    for (char *p = log, *end; p && *p; p = end ? end + 1 : NULL, line++) {
        end = strchr(p, '\n');
        gs_json *doc = gs_json_parse(p, end ? (size_t)(end - p) : strlen(p));
        gs_jv v = gs_json_root(doc);
        const char *call = gs_json_str(gs_json_get(v, "call"), "");
        int id = (int)gs_json_num(gs_json_get(v, "id"), 0), replaces = (int)gs_json_num(gs_json_get(v, "replaces_id"), -1);
        if (line == 0) {
            first = id;
            CHECK(!strcmp(call, "Notify") && replaces == 0 && !strcmp(gs_json_str(gs_json_get(v, "app_name"), ""), "gesso tests") && !strcmp(gs_json_str(gs_json_get(v, "summary"), ""), "Ada"));
            CHECK(!strcmp(gs_json_str(gs_json_path(v, "actions.0"), ""), "default") && gs_json_count(gs_json_get(v, "actions")) == 2 && gs_json_num(gs_json_get(v, "expire_timeout"), 0) == -1);
        }
        if (line == 1) CHECK(!strcmp(call, "Notify") && id != first && !strcmp(gs_json_str(gs_json_get(v, "summary"), ""), "Grace & <Hopper>"));
        // A body is escaped for a service that reads it as markup, and a title never is.
        if (line == 1) CHECK(!strcmp(gs_json_str(gs_json_get(v, "body"), ""), SDL_getenv("GS_TEST_NOTIFY_MARKUP") ? "Tom &amp; &lt;Jerry&gt;" : "Tom & <Jerry>"));
        if (line == 2) CHECK(!strcmp(call, "Notify") && replaces == first && id == first);  // in place of the first
        if (line == 3) CHECK(!strcmp(call, "CloseNotification") && id != first);
        gs_json_free(doc);
    }
    CHECK(line == 4);
    SDL_free(log);
}
