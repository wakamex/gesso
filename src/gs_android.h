// What gesso's Android parts share: gesso's Java classes, found through the app's class loader so any
// thread can use them. An app compiles them beside SDL's: the android_java path of the gesso
// dependency, as of the sdl one. gs_notify, gs_secret's Keystore and gs_background need them.
#pragma once
#include <jni.h>
#include <stdbool.h>

// A global reference to the class, such as "dev.wakamex.gesso.Notify", or NULL with SDL's error set
// when the app was built without gesso's Java.
jclass gs_android_class(JNIEnv *env, const char *name);
// True, with the exception logged and cleared, when the last call threw.
bool gs_android_threw(JNIEnv *env);
// The on-screen keyboard's height in pixels from the bottom of the window; 0 when it is hidden.
int gs_android_keyboard_height(void);
// Starts the on-screen keyboard's input again at a cursor after the first n bytes of text, which it
// takes as what comes before it (SDLActivity.restartTextInput).
void gs_android_keyboard_restart(const char *text, size_t n);
// The floating toolbar over a selection, at x, y, w, h in window pixels: Copy when something is
// selected, and Cut, Paste and Select all when it is in a field. A choice arrives in `window` as its
// keyboard shortcut (Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+A). gs_android_toolbar_closed says, once, that the
// user dismissed it or chose from it.
void gs_android_toolbar_show(unsigned window, int x, int y, int w, int h, bool editable, bool selected);
void gs_android_toolbar_hide(void);
bool gs_android_toolbar_closed(void);
