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

int gs_android_keyboard_height(void) {
    static jclass keyboard;
    static bool missing;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (missing || (!keyboard && !(keyboard = gs_android_class(env, "dev.wakamex.gesso.Keyboard")))) return missing = true, 0;
    jobject activity = SDL_GetAndroidActivity();
    int height = (*env)->CallStaticIntMethod(env, keyboard, (*env)->GetStaticMethodID(env, keyboard, "height", "(Landroid/app/Activity;)I"), activity);
    if (gs_android_threw(env)) height = 0;
    (*env)->DeleteLocalRef(env, activity);
    return height;
}

void gs_android_keyboard_restart(const char *text, size_t n) {
    static jclass keyboard;
    static bool missing;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (missing || (!keyboard && !(keyboard = gs_android_class(env, "dev.wakamex.gesso.Keyboard")))) { missing = true; return; }
    const size_t most = 2000;  // (the context a keyboard reads is the last few words)
    size_t from = n > most ? n - most : 0;
    while (from < n && ((unsigned char)text[from] & 0xC0) == 0x80) from++;  // (not from the middle of a character)
    jbyteArray before = (*env)->NewByteArray(env, (jsize)(n - from));
    if (!before) { gs_android_threw(env); return; }
    (*env)->SetByteArrayRegion(env, before, 0, (jsize)(n - from), (const jbyte *)text + from);
    (*env)->CallStaticVoidMethod(env, keyboard, (*env)->GetStaticMethodID(env, keyboard, "restart", "([B)V"), before);
    gs_android_threw(env);
    (*env)->DeleteLocalRef(env, before);
}

// ---- The selection toolbar ----

static jclass selection;
static SDL_AtomicInt toolbar_window, toolbar_closed;

static void JNICALL toolbar_chosen(JNIEnv *env, jclass c, jint action) {
    static const SDL_Keycode keys[] = { 0, SDLK_C, SDLK_X, SDLK_V, SDLK_A };
    if (action < 1 || action > 4) return;
    SDL_Event e = { .key = { .type = SDL_EVENT_KEY_DOWN, .windowID = (SDL_WindowID)SDL_GetAtomicInt(&toolbar_window), .key = keys[action], .mod = SDL_KMOD_LCTRL, .down = true } };
    SDL_PushEvent(&e);
    e.key.type = SDL_EVENT_KEY_UP, e.key.down = false;
    SDL_PushEvent(&e);
}

static void JNICALL toolbar_gone(JNIEnv *env, jclass c) { SDL_SetAtomicInt(&toolbar_closed, 1); }

static bool toolbar_class(JNIEnv *env) {
    static bool missing;
    if (selection || missing) return selection != NULL;
    if (!(selection = gs_android_class(env, "dev.wakamex.gesso.Selection"))) return !(missing = true);
    JNINativeMethod natives[] = { { "chosen", "(I)V", (void *)toolbar_chosen }, { "closed", "()V", (void *)toolbar_gone } };
    if ((*env)->RegisterNatives(env, selection, natives, 2) != 0) gs_android_threw(env), selection = NULL, missing = true;
    return selection != NULL;
}

void gs_android_toolbar_show(unsigned window, int x, int y, int w, int h, bool editable, bool selected) {
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (!toolbar_class(env)) return;
    SDL_SetAtomicInt(&toolbar_window, (int)window);
    jobject activity = SDL_GetAndroidActivity();
    (*env)->CallStaticVoidMethod(env, selection, (*env)->GetStaticMethodID(env, selection, "show", "(Landroid/app/Activity;IIIIZZ)V"), activity, x, y, w, h, (jboolean)editable, (jboolean)selected);
    gs_android_threw(env);
    (*env)->DeleteLocalRef(env, activity);
}

void gs_android_toolbar_hide(void) {
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (!toolbar_class(env)) return;
    jobject activity = SDL_GetAndroidActivity();
    (*env)->CallStaticVoidMethod(env, selection, (*env)->GetStaticMethodID(env, selection, "hide", "(Landroid/app/Activity;)V"), activity);
    gs_android_threw(env);
    (*env)->DeleteLocalRef(env, activity);
}

bool gs_android_toolbar_closed(void) { return SDL_SetAtomicInt(&toolbar_closed, 0) != 0; }

void gs_android_magnify(bool show, float x, float y) {
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (!toolbar_class(env)) return;
    jobject activity = SDL_GetAndroidActivity();
    (*env)->CallStaticVoidMethod(env, selection, (*env)->GetStaticMethodID(env, selection, "magnify", "(Landroid/app/Activity;ZFF)V"), activity, (jboolean)show, x, y);
    gs_android_threw(env);
    (*env)->DeleteLocalRef(env, activity);
}

// ---- The keyboard's log ----

enum { LOG_LINES = 128, LOG_LINE = 512 };
static struct { SDL_Mutex *lock; char lines[LOG_LINES][LOG_LINE]; int start, count; } heard_log;

static void JNICALL keyboard_heard(JNIEnv *env, jclass c, jstring line) {
    const char *s = (*env)->GetStringUTFChars(env, line, NULL);
    if (!s) return;
    SDL_LockMutex(heard_log.lock);
    if (heard_log.count == LOG_LINES) heard_log.start = (heard_log.start + 1) % LOG_LINES, heard_log.count--;  // (the oldest go)
    SDL_strlcpy(heard_log.lines[(heard_log.start + heard_log.count++) % LOG_LINES], s, LOG_LINE);
    SDL_UnlockMutex(heard_log.lock);
    (*env)->ReleaseStringUTFChars(env, line, s);
}

bool gs_android_keyboard_log(char *line, size_t size) {
    static SDL_InitState once;
    if (SDL_ShouldInit(&once)) {
        JNIEnv *env = SDL_GetAndroidJNIEnv();
        jclass keyboard = gs_android_class(env, "dev.wakamex.gesso.Keyboard");
        heard_log.lock = SDL_CreateMutex();
        JNINativeMethod native = { "heard", "(Ljava/lang/String;)V", (void *)keyboard_heard };
        if (keyboard && heard_log.lock && (*env)->RegisterNatives(env, keyboard, &native, 1) == 0)
            (*env)->CallStaticVoidMethod(env, keyboard, (*env)->GetStaticMethodID(env, keyboard, "watch", "()V"));
        gs_android_threw(env);
        SDL_SetInitialized(&once, true);
    }
    if (!heard_log.lock) return false;
    SDL_LockMutex(heard_log.lock);
    bool any = heard_log.count > 0;
    if (any) SDL_strlcpy(line, heard_log.lines[heard_log.start], size), heard_log.start = (heard_log.start + 1) % LOG_LINES, heard_log.count--;
    SDL_UnlockMutex(heard_log.lock);
    return any;
}
