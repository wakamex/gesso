// Desktop notifications whose click reaches the app: a title and a line or two of text shown by
// the system (a toast on Windows, the Notification Centre on macOS, the desktop's notification
// service on Linux), which the user can click to come back to the app.
//
// Each notification has a tag of the app's choosing, such as a conversation's id: showing another
// with the same tag replaces the first, and a click hands the tag to the app.
//
// What each system needs:
// - Windows 10 and 11: nothing beforehand. gs_notify_init registers the app's id and name for the
//   current user (under HKCU\Software\Classes\AppUserModelId), which is what lets a program without
//   an installer show toasts. A click is heard while the program runs.
// - macOS: the program must be an app bundle with an identifier, signed (tools/bundle-macos.sh); a
//   bare executable has no notifications and gs_notify_init returns false. The first call has the
//   system ask the user's leave, once, and notifications shown before the user has answered are
//   dropped. An ad hoc signature is enough.
// - Linux: a session D-Bus and a notification service on it, which every desktop has and a bare
//   session may not. libdbus is opened at run time, so nothing is needed to build.
#pragma once
#include <stdbool.h>

// `name` is the app's name as shown beside its notifications, `id` its identifier in reverse-domain
// form ("com.example.app"), `icon` the path of a PNG for them or NULL, and `clicked` is called with a
// notification's tag when the user clicks it, on a thread of the notifications' own (the main thread
// on macOS). False where there is no way to notify, after which the other calls do nothing.
bool gs_notify_init(const char *name, const char *id, const char *icon, void (*clicked)(const char *tag));
// Shows a notification, in place of an earlier one with the same tag. False when it could not be handed over.
bool gs_notify_show(const char *tag, const char *title, const char *body);
// Takes back the notification with that tag, if it is still showing or listed.
void gs_notify_close(const char *tag);
