// The system's media controls for an app that plays something: the play/pause, next, previous and
// stop keys reach it while its window is in the background, as they reach a music player, and the
// system's media overlay shows what is playing. The keys are shared the way the system shares
// them, with the app that played last; nothing is taken from other apps.
//
// - Windows 10 and 11: the System Media Transport Controls of the app's window. The media card names
//   an app by its app ID, which it finds among the Start menu's shortcuts, so gs_nowplaying_init gives
//   the process and its window `id` as their app ID and keeps a shortcut named `name` to the program,
//   with that app ID and the program's icon, in the current user's Start menu (written again when the
//   program has moved). A taskbar pin made before the app had its app ID no longer matches its window:
//   pin it again.
// - macOS: the remote command centre and the now-playing information of the MediaPlayer framework.
//   The system gives the keys to the app that last played sound.
// - Linux: MPRIS, a player on the session D-Bus named org.mpris.MediaPlayer2.<id>, which the
//   desktop's media keys and its media panel talk to. libdbus is opened at run time.
//
// A command arrives as an SDL event of the type gs_nowplaying_init returns, with the command in
// event.user.code, from whatever thread the system uses: it wakes an app waiting in SDL_WaitEvent.
// SDL still reports the media keys to the focused window as key events (SDL_SCANCODE_MEDIA_*), and
// the system may also route the same press here: an app with the controls on takes its commands from
// these events only.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

enum { GS_NOWPLAYING_PLAY = 1, GS_NOWPLAYING_PAUSE, GS_NOWPLAYING_TOGGLE, GS_NOWPLAYING_NEXT, GS_NOWPLAYING_PREVIOUS, GS_NOWPLAYING_STOP };
enum { GS_NOWPLAYING_STOPPED, GS_NOWPLAYING_PLAYING, GS_NOWPLAYING_PAUSED };

// Turns the controls on for the app's window. `name` is the app's name as the system shows it and
// `id` a short identifier made of letters and digits ("gtube"), the app's app ID on Windows. Returns the event type the commands
// come as, or 0 where the system has no such controls (no session D-Bus, say).
Uint32 gs_nowplaying_init(SDL_Window *win, const char *name, const char *id);
// What is playing, as the overlay shows it (either may be NULL), and whether it plays: playing or
// paused shows the controls, stopped hides them.
void gs_nowplaying_set(const char *title, const char *artist, int state);
void gs_nowplaying_quit(void);
