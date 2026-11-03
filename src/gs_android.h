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
