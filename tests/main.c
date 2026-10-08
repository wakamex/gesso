#include "test.h"

int test_failures;

static const struct { const char *name; void (*run)(void); } tests[] = {
    { "gs_hls", test_hls },
    { "gs_http", test_http },
    { "gs_json", test_json },
    { "gs_oauth", test_oauth },
    { "gs_secret", test_secret },
    { "gs_stream", test_stream },
    { "gs_text", test_text },
    { "gs_ui", test_ui },
};

int main(void) {
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) {
        int before = test_failures;
        tests[i].run();
        printf("%-12s %s\n", tests[i].name, test_failures == before ? "ok" : "FAILED");
    }
    return test_failures != 0;
}
