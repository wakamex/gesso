// Linux: the desktop's notification service (org.freedesktop.Notifications) over the session
// D-Bus, through libdbus opened at run time.
#include <SDL3/SDL.h>
#include <poll.h>

#include "gs_notify.h"

#define LIVE 64  // notifications remembered, for replacing, closing and naming the one clicked

// libdbus's types as far as they are used: an iterator and an error are caller-allocated structures
// (72 and 32 bytes on 64-bit systems), given room to spare.
typedef struct { void *room[16]; } dbus_iter;
typedef struct { const char *name, *message; unsigned flags[2]; void *pad; } dbus_error;
enum { TYPE_STRING = 's', TYPE_UINT32 = 'u', TYPE_INT32 = 'i', TYPE_ARRAY = 'a', TYPE_INVALID = 0 };

static struct {
    SDL_SharedObject *lib;
    void *conn;
    SDL_Mutex *lock;
    char name[128], icon[1024];
    void (*clicked)(const char *tag);
    struct { char tag[128]; unsigned id; } live[LIVE];
    void (*error_init)(dbus_error *);
    void (*error_free)(dbus_error *);
    void *(*bus_get_private)(int, dbus_error *);
    void (*set_exit_on_disconnect)(void *, int);
    void (*add_match)(void *, const char *, dbus_error *);
    int (*read_write)(void *, int);
    void *(*pop_message)(void *);
    int (*get_unix_fd)(void *, int *);
    void *(*new_method_call)(const char *, const char *, const char *, const char *);
    void (*iter_init_append)(void *, dbus_iter *);
    int (*iter_append_basic)(dbus_iter *, int, const void *);
    int (*iter_open_container)(dbus_iter *, int, const char *, dbus_iter *);
    int (*iter_close_container)(dbus_iter *, dbus_iter *);
    void *(*send_with_reply_and_block)(void *, void *, int, dbus_error *);
    int (*send)(void *, void *, unsigned *);
    void (*flush)(void *);
    int (*get_args)(void *, dbus_error *, int, ...);
    int (*is_signal)(void *, const char *, const char *);
    void (*message_unref)(void *);
    int (*iter_init)(void *, dbus_iter *);
    void (*iter_recurse)(dbus_iter *, dbus_iter *);
    int (*iter_get_arg_type)(dbus_iter *);
    void (*iter_get_basic)(dbus_iter *, void *);
    int (*iter_next)(dbus_iter *);
    bool markup;  // the service reads a body as markup, so its text is escaped
} D;

#define SERVICE "org.freedesktop.Notifications"
#define PATH "/org/freedesktop/Notifications"

// The service's signals: a click ("default" is the action a notification's body stands for), and a
// notification going away, by itself or by the user's hand.
static int listener(void *user) {
    int fd = -1;
    D.get_unix_fd(D.conn, &fd);
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, 500);
        char clicked[LIVE][128];
        int nclicked = 0;
        SDL_LockMutex(D.lock);
        D.read_write(D.conn, 0);
        for (void *m; (m = D.pop_message(D.conn)); D.message_unref(m)) {
            unsigned id = 0, reason = 0;
            const char *action = "";
            bool click = D.is_signal(m, SERVICE, "ActionInvoked") && D.get_args(m, NULL, TYPE_UINT32, &id, TYPE_STRING, &action, TYPE_INVALID);
            bool closed = !click && D.is_signal(m, SERVICE, "NotificationClosed") && D.get_args(m, NULL, TYPE_UINT32, &id, TYPE_UINT32, &reason, TYPE_INVALID);
            for (int i = 0; (click || closed) && i < LIVE; i++) {
                if (D.live[i].id != id || !D.live[i].tag[0]) continue;
                if (click && !SDL_strcmp(action, "default") && nclicked < LIVE) SDL_strlcpy(clicked[nclicked++], D.live[i].tag, sizeof clicked[0]);
                if (closed) D.live[i].tag[0] = 0, D.live[i].id = 0;
            }
        }
        SDL_UnlockMutex(D.lock);
        for (int i = 0; i < nclicked; i++) D.clicked(clicked[i]);
    }
    return 0;
}

bool gs_notify_init(const char *name, const char *id, const char *icon, void (*clicked)(const char *tag)) {
    if (D.conn) return true;
    D.lib = SDL_LoadObject("libdbus-1.so.3");
    struct { void *fn; const char *name; } syms[] = {
        { &D.error_init, "dbus_error_init" }, { &D.error_free, "dbus_error_free" }, { &D.bus_get_private, "dbus_bus_get_private" },
        { &D.set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect" }, { &D.add_match, "dbus_bus_add_match" },
        { &D.read_write, "dbus_connection_read_write" }, { &D.pop_message, "dbus_connection_pop_message" }, { &D.get_unix_fd, "dbus_connection_get_unix_fd" },
        { &D.new_method_call, "dbus_message_new_method_call" }, { &D.iter_init_append, "dbus_message_iter_init_append" },
        { &D.iter_append_basic, "dbus_message_iter_append_basic" }, { &D.iter_open_container, "dbus_message_iter_open_container" },
        { &D.iter_close_container, "dbus_message_iter_close_container" }, { &D.send_with_reply_and_block, "dbus_connection_send_with_reply_and_block" },
        { &D.send, "dbus_connection_send" }, { &D.flush, "dbus_connection_flush" }, { &D.get_args, "dbus_message_get_args" },
        { &D.is_signal, "dbus_message_is_signal" }, { &D.message_unref, "dbus_message_unref" }, { &D.iter_init, "dbus_message_iter_init" },
        { &D.iter_recurse, "dbus_message_iter_recurse" }, { &D.iter_get_arg_type, "dbus_message_iter_get_arg_type" },
        { &D.iter_get_basic, "dbus_message_iter_get_basic" }, { &D.iter_next, "dbus_message_iter_next" },
    };
    bool ok = D.lib != NULL;
    for (size_t i = 0; ok && i < SDL_arraysize(syms); i++) ok = (*(SDL_FunctionPointer *)syms[i].fn = SDL_LoadFunction(D.lib, syms[i].name)) != NULL;
    if (!ok) return false;
    dbus_error e;
    D.error_init(&e);
    void *conn = D.bus_get_private(0 /* the session bus */, &e);
    if (!conn) { D.error_free(&e); return false; }
    D.set_exit_on_disconnect(conn, 0);
    D.add_match(conn, "type='signal',interface='" SERVICE "'", NULL);
    D.flush(conn);
    // What the service can do: one that takes markup in a body (GNOME's, KDE's) would read "<" and "&" as such.
    void *ask = D.new_method_call(SERVICE, PATH, SERVICE, "GetCapabilities"), *caps = ask ? D.send_with_reply_and_block(conn, ask, 2000, NULL) : NULL;
    dbus_iter it, each;
    if (caps && D.iter_init(caps, &it) && D.iter_get_arg_type(&it) == TYPE_ARRAY) {
        for (D.iter_recurse(&it, &each); D.iter_get_arg_type(&each) == TYPE_STRING; D.iter_next(&each)) {
            const char *cap = "";
            D.iter_get_basic(&each, &cap);
            if (!SDL_strcmp(cap, "body-markup")) D.markup = true;
        }
    }
    if (caps) D.message_unref(caps);
    if (ask) D.message_unref(ask);
    SDL_strlcpy(D.name, name, sizeof D.name);
    SDL_strlcpy(D.icon, icon ? icon : "", sizeof D.icon);
    D.clicked = clicked, D.lock = SDL_CreateMutex(), D.conn = conn;
    SDL_DetachThread(SDL_CreateThread(listener, "notifications", NULL));
    return true;
}

static int find(const char *tag) {
    for (int i = 0; i < LIVE; i++)
        if (!SDL_strcmp(D.live[i].tag, tag)) return i;
    return -1;
}

bool gs_notify_show(const char *tag, const char *title, const char *body) {
    if (!D.conn || !tag || !*tag) return false;
    SDL_LockMutex(D.lock);
    int at = find(tag);
    for (int i = 0; at < 0 && i < LIVE; i++)
        if (!D.live[i].tag[0]) at = i;
    if (at < 0) at = 0;  // (more than are remembered: the oldest slot is given up)
    unsigned replaces = !SDL_strcmp(D.live[at].tag, tag) ? D.live[at].id : 0, id = 0;
    int timeout = -1;  // the service's own
    const char *name = D.name, *icon = D.icon, *actions[] = { "default", "Open" };
    void *m = D.new_method_call(SERVICE, PATH, SERVICE, "Notify"), *reply = NULL;
    dbus_iter it, sub;
    bool ok = m != NULL;
    if (ok) {
        // Notify(app_name s, replaces_id u, app_icon s, summary s, body s, actions as, hints a{sv}, expire_timeout i) gives id u
        char escaped[2048];
        if (!body) body = "";
        if (D.markup) {
            size_t n = 0;
            for (const char *p = body; *p && n + 6 < sizeof escaped; p++) {
                const char *as = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : NULL;
                if (as) n += SDL_strlcpy(escaped + n, as, sizeof escaped - n);
                else escaped[n++] = *p;
            }
            escaped[n] = 0, body = escaped;
        }
        D.iter_init_append(m, &it);
        ok = D.iter_append_basic(&it, TYPE_STRING, &name) && D.iter_append_basic(&it, TYPE_UINT32, &replaces) && D.iter_append_basic(&it, TYPE_STRING, &icon) &&
             D.iter_append_basic(&it, TYPE_STRING, &title) && D.iter_append_basic(&it, TYPE_STRING, &body) && D.iter_open_container(&it, TYPE_ARRAY, "s", &sub);
        for (int i = 0; ok && i < 2; i++) ok = D.iter_append_basic(&sub, TYPE_STRING, &actions[i]);
        ok = ok && D.iter_close_container(&it, &sub) && D.iter_open_container(&it, TYPE_ARRAY, "{sv}", &sub) && D.iter_close_container(&it, &sub) &&
             D.iter_append_basic(&it, TYPE_INT32, &timeout);
        if (ok) reply = D.send_with_reply_and_block(D.conn, m, 2000, NULL);
        ok = reply && D.get_args(reply, NULL, TYPE_UINT32, &id, TYPE_INVALID);
        if (reply) D.message_unref(reply);
        D.message_unref(m);
    }
    if (ok) SDL_strlcpy(D.live[at].tag, tag, sizeof D.live[at].tag), D.live[at].id = id;
    SDL_UnlockMutex(D.lock);
    return ok;
}

void gs_notify_close(const char *tag) {
    if (!D.conn || !tag) return;
    SDL_LockMutex(D.lock);
    int at = find(tag);
    void *m = at >= 0 ? D.new_method_call(SERVICE, PATH, SERVICE, "CloseNotification") : NULL;
    if (m) {
        dbus_iter it;
        D.iter_init_append(m, &it);
        if (D.iter_append_basic(&it, TYPE_UINT32, &D.live[at].id)) D.send(D.conn, m, NULL), D.flush(D.conn);
        D.message_unref(m);
        D.live[at].tag[0] = 0, D.live[at].id = 0;
    }
    SDL_UnlockMutex(D.lock);
}
