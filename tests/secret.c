#include <SDL3/SDL.h>
#include <string.h>

#include "gs_secret.h"
#include "test.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

void test_secret(void) {
    char path[1024];
    SDL_snprintf(path, sizeof path, "%sgesso-test-secret", SDL_GetBasePath());
    const char token[] = "{\"access_token\":\"abc\",\"refresh_token\":\"def\"}";
    CHECK(gs_secret_save(path, token, strlen(token)));
    size_t n = 0;
    char *back = gs_secret_load(path, &n);
    CHECK(back && n == strlen(token) && !strcmp(back, token));
    SDL_free(back);
    size_t raw_n;
    char *raw = SDL_LoadFile(path, &raw_n);
#ifdef _WIN32
    CHECK(raw && !strstr(raw, "access_token"));  // encrypted at rest
#else
    // In the system's place for secrets no file holds it; without one, a file only its owner reads does.
    // GS_TEST_SECRET_STORE says the run has such a place (an unlocked Keychain, a Secret Service) and must use it.
    struct stat st;
    if (gs_secret_protected()) CHECK(!raw);
    else CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600 && !SDL_getenv("GS_TEST_SECRET_STORE"));
#endif
    SDL_free(raw);
    CHECK(gs_secret_save(path, "x", 1));  // replaced whole
    back = gs_secret_load(path, &n);
    CHECK(back && n == 1 && back[0] == 'x');
    SDL_free(back);
    // A secret with zero bytes in it, such as a key, comes back whole.
    const unsigned char key[] = { 0x00, 0xff, 0x10, 0x00, 0x7f, 0x80 };
    CHECK(gs_secret_save(path, key, sizeof key));
    back = gs_secret_load(path, &n);
    CHECK(back && n == sizeof key && !memcmp(back, key, sizeof key));
    SDL_free(back);
#ifndef _WIN32
    // One saved as a plain file by an earlier version is still read, and moves on the next save.
    gs_secret_erase(path);
    CHECK(gs_secret_write_private(path, "legacy", 6));
    back = gs_secret_load(path, &n);
    CHECK(back && n == 6 && !strcmp(back, "legacy") && !gs_secret_protected());
    SDL_free(back);
    CHECK(gs_secret_save(path, "moved", 5));
    back = gs_secret_load(path, &n);
    CHECK(back && !strcmp(back, "moved"));
    SDL_free(back);
#endif
    gs_secret_erase(path);
    CHECK(gs_secret_load(path, &n) == NULL);
}
