#include "gs_android.h"

#include <SDL3/SDL.h>

bool gs_android_threw(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return false;
    (*env)->ExceptionDescribe(env);  // (to the log)
    (*env)->ExceptionClear(env);
    return true;
}

// FindClass sees only the system's classes from threads Java did not start, so the class is loaded
// through the activity's class loader.
jclass gs_android_class(JNIEnv *env, const char *name) {
    jobject activity = SDL_GetAndroidActivity();
    if (!env || !activity) return NULL;
    jclass global = NULL;
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    jobject loader = (*env)->CallObjectMethod(env, activity, (*env)->GetMethodID(env, activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass loader_class = (*env)->FindClass(env, "java/lang/ClassLoader");
    jstring jname = (*env)->NewStringUTF(env, name);
    jclass c = loader && !gs_android_threw(env) ? (*env)->CallObjectMethod(env, loader, (*env)->GetMethodID(env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), jname) : NULL;
    if (gs_android_threw(env) || !c) SDL_SetError("%s is missing: compile gesso's android_java into the app", name);
    else global = (*env)->NewGlobalRef(env, c);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, jname);
    (*env)->DeleteLocalRef(env, loader_class);
    (*env)->DeleteLocalRef(env, loader);
    (*env)->DeleteLocalRef(env, activity_class);
    (*env)->DeleteLocalRef(env, activity);
    return global;
}
