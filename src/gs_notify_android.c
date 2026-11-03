// gs_notify on Android: android/java/dev/wakamex/gesso/Notify.java, which posts to the NotificationManager.
#include "gs_notify.h"

#include <SDL3/SDL.h>

#include "gs_android.h"

static jclass notify;
static bool ready;  // gs_notify_init succeeded
static void (*on_click)(const char *tag);

static void JNICALL clicked(JNIEnv *env, jclass c, jstring tag) {
    const char *t = (*env)->GetStringUTFChars(env, tag, NULL);
    if (!t) return;
    if (on_click) on_click(t);
    (*env)->ReleaseStringUTFChars(env, tag, t);
}

static void SDLCALL answered(void *user, const char *permission, bool granted) {}

bool gs_notify_init(const char *name, const char *id, const char *icon, void (*click)(const char *tag)) {
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (!notify && !(notify = gs_android_class(env, "dev.wakamex.gesso.Notify"))) return SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "gs_notify: %s", SDL_GetError()), false;
    on_click = click;
    JNINativeMethod native = { "clicked", "(Ljava/lang/String;)V", (void *)clicked };
    if ((*env)->RegisterNatives(env, notify, &native, 1) != 0) return gs_android_threw(env), false;
    // From Android 13 the user is asked, once; notifications shown before the answer are dropped.
    if (SDL_GetAndroidSDKVersion() >= 33) SDL_RequestAndroidPermission("android.permission.POST_NOTIFICATIONS", answered, NULL);
    jobject activity = SDL_GetAndroidActivity();
    jstring jname = (*env)->NewStringUTF(env, name), jid = (*env)->NewStringUTF(env, id), jicon = icon ? (*env)->NewStringUTF(env, icon) : NULL;
    jmethodID init = (*env)->GetStaticMethodID(env, notify, "init", "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
    bool ok = (*env)->CallStaticBooleanMethod(env, notify, init, activity, jname, jid, jicon) && !gs_android_threw(env);
    (*env)->DeleteLocalRef(env, jname), (*env)->DeleteLocalRef(env, jid), (*env)->DeleteLocalRef(env, jicon), (*env)->DeleteLocalRef(env, activity);
    return ready = ok;
}

bool gs_notify_show(const char *tag, const char *title, const char *body) {
    if (!ready) return false;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jstring a = (*env)->NewStringUTF(env, tag), b = (*env)->NewStringUTF(env, title), c = (*env)->NewStringUTF(env, body ? body : "");
    jmethodID show = (*env)->GetStaticMethodID(env, notify, "show", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
    bool ok = (*env)->CallStaticBooleanMethod(env, notify, show, a, b, c) && !gs_android_threw(env);
    (*env)->DeleteLocalRef(env, a), (*env)->DeleteLocalRef(env, b), (*env)->DeleteLocalRef(env, c);
    return ok;
}

void gs_notify_close(const char *tag) {
    if (!ready) return;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jstring t = (*env)->NewStringUTF(env, tag);
    (*env)->CallStaticVoidMethod(env, notify, (*env)->GetStaticMethodID(env, notify, "close", "(Ljava/lang/String;)V"), t);
    gs_android_threw(env);
    (*env)->DeleteLocalRef(env, t);
}
