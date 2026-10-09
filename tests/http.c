// Needs the network, so it runs only with GS_TEST_NETWORK=1; GS_HTTP_CURL_PROGRAM=1 tests the curl
// program's path instead of the platform's library.
#include <string.h>

#include "gs_http.h"
#include "gs_json.h"
#include "test.h"

static bool count(void *user, const uint8_t *data, size_t len) {
    (void)data;
    *(size_t *)user += len;
    return *(size_t *)user < 20000;  // ends the transfer early
}

typedef struct { size_t calls, last, stop_after; long long total; } progress_log;

static bool note_progress(void *user, size_t received, long long total) {
    progress_log *p = user;
    p->calls++, p->last = received, p->total = total;
    return !p->stop_after || received < p->stop_after;
}

extern const char *test_streams;

void test_http(void) {
    char *body = NULL;
    size_t len = 0;
#ifndef _WIN32
    // The progress hook, on a local file (WinHTTP has no file URLs): it hears the bytes arrive, and
    // giving up makes the request return 0.
    if (test_streams && !SDL_GetHintBoolean("GS_HTTP_CURL_PROGRAM", false)) {  // (the curl program gives a file no status)
        char url[1200];
        SDL_snprintf(url, sizeof url, "file://%s/beep.ts", test_streams);
        progress_log p = { 0 };
        CHECK(gs_http_fetch(&(gs_http_request){ .url = url, .progress = note_progress, .progress_user = &p }, &body, &len) == 200);
        CHECK(p.calls > 0 && p.last == len);
        SDL_free(body), body = NULL;
        p = (progress_log){ .stop_after = 1 };
        CHECK(gs_http_fetch(&(gs_http_request){ .url = url, .progress = note_progress, .progress_user = &p }, &body, &len) == 0);
        CHECK(body == NULL);
    }
#endif
    if (!SDL_GetHintBoolean("GS_TEST_NETWORK", false)) return;

    // A POST with headers, read back from the echo.
    const char *headers[] = { "Authorization: Bearer secret-token", "Content-Type: application/x-www-form-urlencoded", NULL };
    gs_http_request post = { .url = "https://httpbin.org/post", .headers = headers, .body = "a=1&b=two", .agent = "gesso-test" };
    CHECK(gs_http_fetch(&post, &body, &len) == 200);
    gs_json *doc = body ? gs_json_parse(body, len) : NULL;
    gs_jv root = gs_json_root(doc);
    CHECK(!strcmp(gs_json_str(gs_json_path(root, "form.b"), ""), "two"));
    CHECK(!strcmp(gs_json_str(gs_json_path(root, "headers.Authorization"), ""), "Bearer secret-token"));
    CHECK(!strcmp(gs_json_str(gs_json_path(root, "headers.User-Agent"), ""), "gesso-test"));
    gs_json_free(doc), SDL_free(body), body = NULL;

    // Statuses, redirects followed or not, and a timeout.
    CHECK(gs_http_fetch(&(gs_http_request){ .url = "https://httpbin.org/status/404" }, NULL, NULL) == 404);
    CHECK(gs_http_fetch(&(gs_http_request){ .url = "https://httpbin.org/redirect/2", .follow = true }, NULL, NULL) == 200);
    CHECK(gs_http_fetch(&(gs_http_request){ .url = "https://httpbin.org/redirect/1" }, NULL, NULL) == 302);
    uint64_t start = SDL_GetTicks();
    CHECK(gs_http_fetch(&(gs_http_request){ .url = "https://httpbin.org/delay/10", .timeout_ms = 2000 }, NULL, NULL) == 0);
    CHECK(SDL_GetTicks() - start < 6000);

    // A cookie set by the server comes back on the next request.
    char jar[512];
    SDL_snprintf(jar, sizeof jar, "%sgesso-test-cookies.txt", SDL_GetBasePath());
    SDL_RemovePath(jar);
    CHECK(gs_http_fetch(&(gs_http_request){ .url = "https://httpbin.org/cookies/set?k=v1", .cookies = jar, .save_cookies = true }, NULL, NULL) == 302);
    CHECK(gs_http_fetch(&(gs_http_request){ .url = "https://httpbin.org/cookies", .cookies = jar }, &body, &len) == 200);
    CHECK(body && strstr(body, "\"k\": \"v1\""));
    SDL_free(body), body = NULL;
    SDL_RemovePath(jar);

    // A stream ended early by its taker.
    size_t got = 0;
    SDL_AtomicInt stop = { 0 };
    CHECK(gs_http_stream(&(gs_http_request){ .url = "https://httpbin.org/bytes/100000" }, 0, count, &got, &stop) == 200);
    CHECK(got >= 20000 && got < 100000);

    // The progress hook over the network: the whole length is known, and giving up returns 0.
    progress_log p = { 0 };
    CHECK(gs_http_fetch(&(gs_http_request){ .url = "https://httpbin.org/bytes/100000", .progress = note_progress, .progress_user = &p }, &body, &len) == 200);
    CHECK(p.last == 100000 && (p.total == 100000 || p.total == -1) && len == 100000);  // (the curl program does not pass the length on)
    SDL_free(body), body = NULL;
    p = (progress_log){ .stop_after = 20000 };
    CHECK(gs_http_fetch(&(gs_http_request){ .url = "https://httpbin.org/bytes/100000", .progress = note_progress, .progress_user = &p }, &body, &len) == 0);
}
