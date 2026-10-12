// A small secret, such as a sign-in token or a database key, kept for the current user under a
// file's path. Where the system has a place for secrets, the secret goes there and no file holds it:
// - Windows: the file is encrypted with the Data Protection API, so only this user on this machine can
//   read it.
// - macOS: the login Keychain, as an item named by the file. The system may ask the user's leave when
//   a different build of the app reads it; a stable signature avoids that (tools/bundle-macos.sh).
// - Linux: the desktop's Secret Service (GNOME Keyring, KDE's wallet, KeePassXC), through libsecret
//   opened at run time. It may ask the user to unlock or create a keyring the first time.
// Where there is no such place (a Linux session without a Secret Service, a Mac's keychain locked
// from a remote shell), the secret is a plain file readable only by its owner (mode 0600), as SSH
// keys are. A secret saved that way before is still read, and moves to the system's place the next
// time it is saved. Writes replace the secret whole, so a crash leaves the old one or the new.
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool gs_secret_save(const char *path, const void *data, size_t len);
void *gs_secret_load(const char *path, size_t *len);  // SDL_free it; zero-terminated; NULL when missing or unreadable
void gs_secret_erase(const char *path);
// True when the last save or load on this thread went through the system's place for secrets, false
// when it used a plain file: for an app that tells its user how a key is kept.
bool gs_secret_protected(void);

// A file only the user can read, written plainly on every system: for a short-lived copy a child
// process must read, such as a cookie file for curl.
bool gs_secret_write_private(const char *path, const void *data, size_t len);
