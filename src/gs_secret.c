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
#ifdef __APPLE__
#include <Security/Security.h>
#endif

static _Thread_local bool protected_last;

bool gs_secret_protected(void) { return protected_last; }

// ---- The system's place for secrets, where there is one besides an encrypted file ----
//
// store_put, store_get and store_remove keep a secret under the file's path. store_put and store_get
// say false when the store cannot be used, and the callers fall back to the file.

#if defined(__APPLE__)

// A generic password item in the login Keychain: the service says whose it is, the account is the path.
static CFMutableDictionaryRef item(const char *path) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef account = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8), service = CFSTR("gesso secret");
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    if (account) CFDictionarySetValue(q, kSecAttrAccount, account), CFRelease(account);
    return q;
}

static bool store_put(const char *path, const void *data, size_t len) {
    CFMutableDictionaryRef q = item(path);
    CFDataRef value = CFDataCreate(NULL, data, (CFIndex)len);
    const char *slash = strrchr(path, '/');
    CFStringRef label = CFStringCreateWithCString(NULL, slash ? slash + 1 : path, kCFStringEncodingUTF8);  // the name Keychain Access shows
    SecItemDelete(q);
    CFDictionarySetValue(q, kSecValueData, value);
    if (label) CFDictionarySetValue(q, kSecAttrLabel, label), CFRelease(label);
    OSStatus status = SecItemAdd(q, NULL);
    CFRelease(value), CFRelease(q);
    return status == errSecSuccess;
}

static void *store_get(const char *path, size_t *len) {
    CFMutableDictionaryRef q = item(path);
    CFTypeRef found = NULL;
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    OSStatus status = SecItemCopyMatching(q, &found);
    CFRelease(q);
    if (status != errSecSuccess || !found) return NULL;
    size_t n = (size_t)CFDataGetLength(found);
    char *out = SDL_malloc(n + 1);
    if (out) memcpy(out, CFDataGetBytePtr(found), n), out[n] = 0, *len = n;
    CFRelease(found);
    return out;
}

static void store_remove(const char *path) {
    CFMutableDictionaryRef q = item(path);
    SecItemDelete(q);
    CFRelease(q);
}

#elif !defined(_WIN32) && !defined(__EMSCRIPTEN__)

// libsecret's simple calls, which take a schema and the attributes that name an item.
typedef struct { const char *name; int flags; struct { const char *name; int type; } attributes[32]; int reserved; void *more[7]; } secret_schema;
static const secret_schema schema = { "dev.wakamex.gesso.Secret", 0 /* no flags */, { { "path", 0 /* a string */ } } };
static struct {
    bool tried, ok;
    int (*store)(const secret_schema *, const char *, const char *, const char *, void *, void **, ...);
    char *(*lookup)(const secret_schema *, void *, void **, ...);
    int (*clear)(const secret_schema *, void *, void **, ...);
    void (*free_password)(char *);
} S;

static bool libsecret(void) {
    if (S.tried) return S.ok;
    S.tried = true;
    SDL_SharedObject *lib = SDL_LoadObject("libsecret-1.so.0");
    if (!lib) return false;
    S.store = (int (*)(const secret_schema *, const char *, const char *, const char *, void *, void **, ...))SDL_LoadFunction(lib, "secret_password_store_sync");
    S.lookup = (char *(*)(const secret_schema *, void *, void **, ...))SDL_LoadFunction(lib, "secret_password_lookup_sync");
    S.clear = (int (*)(const secret_schema *, void *, void **, ...))SDL_LoadFunction(lib, "secret_password_clear_sync");
    S.free_password = (void (*)(char *))SDL_LoadFunction(lib, "secret_password_free");
    return S.ok = S.store && S.lookup && S.clear && S.free_password;
}

// The service keeps text: a secret with a zero byte in it is kept as hexadecimal behind a mark.
#define HEX_MARK "gesso-hex:"

static bool store_put(const char *path, const void *data, size_t len) {
    if (!libsecret()) return false;
    const char *slash = strrchr(path, '/');
    char *text = SDL_malloc(memchr(data, 0, len) ? sizeof HEX_MARK + 2 * len : len + 1);
    if (!text) return false;
    if (memchr(data, 0, len)) {
        size_t n = (size_t)SDL_snprintf(text, sizeof HEX_MARK, "%s", HEX_MARK);
        for (size_t i = 0; i < len; i++) n += (size_t)SDL_snprintf(text + n, 3, "%02x", ((const unsigned char *)data)[i]);
    } else memcpy(text, data, len), text[len] = 0;
    bool ok = S.store(&schema, NULL /* the default keyring */, slash ? slash + 1 : path, text, NULL, NULL, "path", path, NULL);
    SDL_memset(text, 0, SDL_strlen(text));
    SDL_free(text);
    return ok;
}

static void *store_get(const char *path, size_t *len) {
    if (!libsecret()) return NULL;
    char *text = S.lookup(&schema, NULL, NULL, "path", path, NULL);
    if (!text) return NULL;
    size_t n = SDL_strlen(text), mark = sizeof HEX_MARK - 1;
    char *out = SDL_malloc(n + 1);
    if (out && !SDL_strncmp(text, HEX_MARK, mark)) {
        n = (n - mark) / 2;
        for (size_t i = 0; i < n; i++) {
            unsigned byte = 0;
            SDL_sscanf(text + mark + 2 * i, "%2x", &byte);
            out[i] = (char)byte;
        }
        out[n] = 0;
    } else if (out) memcpy(out, text, n + 1);
    if (out) *len = n;
    S.free_password(text);
    return out;
}

static void store_remove(const char *path) {
    if (libsecret()) S.clear(&schema, NULL, NULL, "path", path, NULL);
}

#else
static bool store_put(const char *path, const void *data, size_t len) { return false; }
static void *store_get(const char *path, size_t *len) { return NULL; }
static void store_remove(const char *path) {}
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
    protected_last = ok;
    return ok;
#else
    if ((protected_last = store_put(path, data, len))) return SDL_RemovePath(path), true;  // (no file holds it now)
    return replace_file(path, data, len);
#endif
}

void *gs_secret_load(const char *path, size_t *len) {
    size_t n = 0;
#ifndef _WIN32
    void *kept = store_get(path, &n);
    if ((protected_last = kept != NULL)) {
        if (len) *len = n;
        return kept;
    }
#endif
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
    protected_last = text != NULL;
    return text;
#else
    if (len) *len = n;
    return data;  // (SDL_LoadFile adds a terminating zero)
#endif
}

void gs_secret_erase(const char *path) {
#ifndef _WIN32
    store_remove(path);
#endif
    SDL_RemovePath(path);
}
