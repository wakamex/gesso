#define _POSIX_C_SOURCE 200809L  // O_CLOEXEC, fchmod, fsync
#include "gs_secret.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dpapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Writes to a sibling file and renames it over the target, creating it readable only by the owner.
static bool replace_file(const char *path, const void *data, size_t len) {
    char tmp[4096];
    if ((size_t)SDL_snprintf(tmp, sizeof tmp, "%s.new", path) >= sizeof tmp) return false;
#ifdef _WIN32
    if (!SDL_SaveFile(tmp, data, len)) return false;
#else
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    fchmod(fd, 0600);  // (a file left by an earlier run may have other permissions)
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) break;
        p += n, left -= (size_t)n;
    }
    bool ok = !left && fsync(fd) == 0;
    if (close(fd) != 0 || !ok) return SDL_RemovePath(tmp), false;
#endif
    if (!SDL_RenamePath(tmp, path)) return SDL_RemovePath(tmp), false;
    return true;
}

bool gs_secret_write_private(const char *path, const void *data, size_t len) { return replace_file(path, data, len); }

bool gs_secret_save(const char *path, const void *data, size_t len) {
#ifdef _WIN32
    DATA_BLOB in = { (DWORD)len, (BYTE *)data }, out = { 0 };
    if (!CryptProtectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    bool ok = replace_file(path, out.pbData, out.cbData);
    LocalFree(out.pbData);
    return ok;
#else
    return replace_file(path, data, len);
#endif
}

void *gs_secret_load(const char *path, size_t *len) {
    size_t n;
    void *data = SDL_LoadFile(path, &n);
    if (!data) return NULL;
#ifdef _WIN32
    DATA_BLOB in = { (DWORD)n, data }, out = { 0 };
    bool ok = CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out);
    SDL_free(data);
    if (!ok) return NULL;
    char *text = SDL_malloc(out.cbData + 1);
    if (text) memcpy(text, out.pbData, out.cbData), text[out.cbData] = 0;
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (len) *len = out.cbData;
    return text;
#else
    if (len) *len = n;
    return data;  // (SDL_LoadFile adds a terminating zero)
#endif
}

void gs_secret_erase(const char *path) { SDL_RemovePath(path); }
