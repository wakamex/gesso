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
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
#endif
    SDL_free(raw);
    CHECK(gs_secret_save(path, "x", 1));  // replaced whole
    back = gs_secret_load(path, &n);
    CHECK(back && n == 1 && back[0] == 'x');
    SDL_free(back);
    gs_secret_erase(path);
    CHECK(gs_secret_load(path, &n) == NULL);
}
