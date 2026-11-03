// Keeping the app running and online while another app is in front, for as long as it asks: while a
// sign-in waits for a code scanned on the same phone, or while an account receives messages. Desktop
// systems keep a background app running, so there gs_background_keep only returns true.
//
// Android stops a background app's network within seconds and may stop the app. There the app is
// kept by a foreground service with an ongoing notification of `title` and `text` (a tap opens the
// app), through gesso's Java (gs_android.h). Call gs_background_keep while the app is in front:
// Android 12 and later refuse to start the service from the background. Calling it again changes the
// notification's text. The manifest declares the service with the foreground service type that fits
// what the app does, and the permissions for it; for a messaging app:
//     <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
//     <uses-permission android:name="android.permission.FOREGROUND_SERVICE_REMOTE_MESSAGING" />
//     <service android:name="dev.wakamex.gesso.Background" android:exported="false"
//              android:foregroundServiceType="remoteMessaging" />
// (dataSync is limited to six hours a day from Android 15; specialUse needs a reason given to Google
// Play.) The notification shows only with the POST_NOTIFICATIONS permission (gs_notify.h), though the
// service keeps the app either way. SDL still pauses its own main loop in the background unless
// SDL_HINT_ANDROID_BLOCK_ON_PAUSE is "0"; other threads keep running and online.
#pragma once
#include <stdbool.h>

// False when Android refused the service (not declared in the manifest, or asked from the background).
bool gs_background_keep(const char *title, const char *text);
void gs_background_release(void);
