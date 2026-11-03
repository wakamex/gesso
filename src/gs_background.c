#include "gs_background.h"

#include <SDL3/SDL.h>

#if defined(__ANDROID__)
#include "gs_android.h"

static jclass background;

// Background.start(activity, title, text) or Background.stop(activity).
static bool call(const char *title, const char *text) {
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (!background && !(background = gs_android_class(env, "dev.wakamex.gesso.Background"))) return SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "gs_background: %s", SDL_GetError()), false;
    jobject activity = SDL_GetAndroidActivity();
    bool ok = true;
    if (title) {
        jstring a = (*env)->NewStringUTF(env, title), b = (*env)->NewStringUTF(env, text ? text : "");
        jmethodID start = (*env)->GetStaticMethodID(env, background, "start", "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;)Z");
        ok = (*env)->CallStaticBooleanMethod(env, background, start, activity, a, b) && !gs_android_threw(env);
        (*env)->DeleteLocalRef(env, a), (*env)->DeleteLocalRef(env, b);
    } else {
        (*env)->CallStaticVoidMethod(env, background, (*env)->GetStaticMethodID(env, background, "stop", "(Landroid/app/Activity;)V"), activity);
        gs_android_threw(env);
    }
    (*env)->DeleteLocalRef(env, activity);
    return ok;
}

bool gs_background_keep(const char *title, const char *text) { return call(title ? title : "", text); }
void gs_background_release(void) { call(NULL, NULL); }
#else
bool gs_background_keep(const char *title, const char *text) { return true; }
void gs_background_release(void) {}
#endif
