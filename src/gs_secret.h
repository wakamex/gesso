// A small secret, such as a sign-in token, kept in a file for the current user. On Windows the file
// is encrypted with the Data Protection API, so only this user on this machine can read it. On Linux
// and macOS it is plain but readable only by its owner (mode 0600), as SSH keys are. Writes replace
// the file whole, so a crash leaves the old secret or the new one.
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool gs_secret_save(const char *path, const void *data, size_t len);
void *gs_secret_load(const char *path, size_t *len);  // SDL_free it; zero-terminated; NULL when missing or unreadable
void gs_secret_erase(const char *path);

// A file only the user can read, written plainly on every system: for a short-lived copy a child
// process must read, such as a cookie file for curl.
bool gs_secret_write_private(const char *path, const void *data, size_t len);
