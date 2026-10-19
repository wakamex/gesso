// Linux: an MPRIS player (specifications.freedesktop.org/mpris-spec) on the session D-Bus, through
// libdbus opened at run time. The desktop's media keys and its media panel call the player's
// methods and read its properties; a thread of its own answers them.
#include <SDL3/SDL.h>
#include <poll.h>
#include <unistd.h>

#include "gs_nowplaying.h"

typedef struct { void *room[16]; } dbus_iter;
typedef struct { const char *name, *message; unsigned flags[2]; void *pad; } dbus_error;
enum { T_STRING = 's', T_BOOL = 'b', T_DOUBLE = 'd', T_INT64 = 'x', T_PATH = 'o', T_ARRAY = 'a', T_VARIANT = 'v', T_DICT = 'e', T_INVALID = 0 };
enum { METHOD_CALL = 1 };

static struct {
    SDL_SharedObject *lib;
    void *conn;
    SDL_Mutex *lock;
    Uint32 event;
    char name[128], title[512], artist[512];
    int state;
    void (*error_init)(dbus_error *);
    void (*error_free)(dbus_error *);
    void *(*bus_get_private)(int, dbus_error *);
    void (*set_exit_on_disconnect)(void *, int);
    int (*request_name)(void *, const char *, unsigned, dbus_error *);
    int (*read_write)(void *, int);
    void *(*pop_message)(void *);
    int (*get_unix_fd)(void *, int *);
    int (*message_get_type)(void *);
    const char *(*get_interface)(void *);
    const char *(*get_member)(void *);
    void *(*new_method_return)(void *);
    void *(*new_error)(void *, const char *, const char *);
    void *(*new_signal)(const char *, const char *, const char *);
    void (*iter_init_append)(void *, dbus_iter *);
    int (*iter_append_basic)(dbus_iter *, int, const void *);
    int (*iter_open_container)(dbus_iter *, int, const char *, dbus_iter *);
    int (*iter_close_container)(dbus_iter *, dbus_iter *);
    int (*send)(void *, void *, unsigned *);
    void (*flush)(void *);
    int (*get_args)(void *, dbus_error *, int, ...);
    void (*message_unref)(void *);
    void (*close)(void *);
    void (*connection_unref)(void *);
} D;

#define PATH "/org/mpris/MediaPlayer2"
#define ROOT "org.mpris.MediaPlayer2"
#define PLAYER "org.mpris.MediaPlayer2.Player"
#define PROPS "org.freedesktop.DBus.Properties"
static const char *introspection =
    "<node><interface name='" ROOT "'><method name='Raise'/><method name='Quit'/><property name='CanQuit' type='b' access='read'/>"
    "<property name='CanRaise' type='b' access='read'/><property name='HasTrackList' type='b' access='read'/><property name='Identity' type='s' access='read'/>"
    "<property name='SupportedUriSchemes' type='as' access='read'/><property name='SupportedMimeTypes' type='as' access='read'/></interface>"
    "<interface name='" PLAYER "'><method name='Next'/><method name='Previous'/><method name='Pause'/><method name='PlayPause'/><method name='Stop'/>"
    "<method name='Play'/><method name='Seek'><arg type='x' direction='in'/></method><method name='SetPosition'><arg type='o' direction='in'/>"
    "<arg type='x' direction='in'/></method><method name='OpenUri'><arg type='s' direction='in'/></method>"
    "<property name='PlaybackStatus' type='s' access='read'/><property name='Rate' type='d' access='read'/><property name='Metadata' type='a{sv}' access='read'/>"
    "<property name='Volume' type='d' access='read'/><property name='Position' type='x' access='read'/><property name='MinimumRate' type='d' access='read'/>"
    "<property name='MaximumRate' type='d' access='read'/><property name='CanGoNext' type='b' access='read'/><property name='CanGoPrevious' type='b' access='read'/>"
    "<property name='CanPlay' type='b' access='read'/><property name='CanPause' type='b' access='read'/><property name='CanSeek' type='b' access='read'/>"
    "<property name='CanControl' type='b' access='read'/></interface>"
    "<interface name='" PROPS "'><method name='Get'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
    "<method name='GetAll'><arg type='s' direction='in'/><arg type='a{sv}' direction='out'/></method></interface>"
    "<interface name='org.freedesktop.DBus.Introspectable'><method name='Introspect'><arg type='s' direction='out'/></method></interface></node>";

// A property's value, as a variant: what `name` of `iface` is now. False for one there is not.
static bool put_value(dbus_iter *it, const char *iface, const char *name) {
    static const char *const yes[] = { "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanControl" };
    static const char *const no_root[] = { "CanQuit", "CanRaise", "HasTrackList" };
    dbus_iter v, a, e, ev, aa;
    unsigned b;
    double d = 1;
    long long x = 0;
    bool root = !SDL_strcmp(iface, ROOT), player = !SDL_strcmp(iface, PLAYER);
    for (int i = 0; player && i < 5; i++)
        if (!SDL_strcmp(name, yes[i])) return b = 1, D.iter_open_container(it, T_VARIANT, "b", &v) && D.iter_append_basic(&v, T_BOOL, &b) && D.iter_close_container(it, &v);
    for (int i = 0; root && i < 3; i++)
        if (!SDL_strcmp(name, no_root[i])) return b = 0, D.iter_open_container(it, T_VARIANT, "b", &v) && D.iter_append_basic(&v, T_BOOL, &b) && D.iter_close_container(it, &v);
    if ((player && !SDL_strcmp(name, "CanSeek"))) return b = 0, D.iter_open_container(it, T_VARIANT, "b", &v) && D.iter_append_basic(&v, T_BOOL, &b) && D.iter_close_container(it, &v);
    if (root && !SDL_strcmp(name, "Identity")) {
        const char *s = D.name;
        return D.iter_open_container(it, T_VARIANT, "s", &v) && D.iter_append_basic(&v, T_STRING, &s) && D.iter_close_container(it, &v);
    }
    if (root && (!SDL_strcmp(name, "SupportedUriSchemes") || !SDL_strcmp(name, "SupportedMimeTypes")))
        return D.iter_open_container(it, T_VARIANT, "as", &v) && D.iter_open_container(&v, T_ARRAY, "s", &a) && D.iter_close_container(&v, &a) && D.iter_close_container(it, &v);
    if (player && !SDL_strcmp(name, "PlaybackStatus")) {
        const char *s = D.state == GS_NOWPLAYING_PLAYING ? "Playing" : D.state == GS_NOWPLAYING_PAUSED ? "Paused" : "Stopped";
        return D.iter_open_container(it, T_VARIANT, "s", &v) && D.iter_append_basic(&v, T_STRING, &s) && D.iter_close_container(it, &v);
    }
    if (player && (!SDL_strcmp(name, "Rate") || !SDL_strcmp(name, "Volume") || !SDL_strcmp(name, "MinimumRate") || !SDL_strcmp(name, "MaximumRate")))
        return D.iter_open_container(it, T_VARIANT, "d", &v) && D.iter_append_basic(&v, T_DOUBLE, &d) && D.iter_close_container(it, &v);
    if (player && !SDL_strcmp(name, "Position"))
        return D.iter_open_container(it, T_VARIANT, "x", &v) && D.iter_append_basic(&v, T_INT64, &x) && D.iter_close_container(it, &v);
    if (player && !SDL_strcmp(name, "Metadata")) {  // the track's id, title and artists
        const char *k_id = "mpris:trackid", *track = D.state == GS_NOWPLAYING_STOPPED ? "/org/mpris/MediaPlayer2/TrackList/NoTrack" : PATH "/Track/1";
        const char *k_title = "xesam:title", *title = D.title, *k_artist = "xesam:artist", *artist = D.artist;
        bool ok = D.iter_open_container(it, T_VARIANT, "a{sv}", &v) && D.iter_open_container(&v, T_ARRAY, "{sv}", &a);
        ok = ok && D.iter_open_container(&a, T_DICT, NULL, &e) && D.iter_append_basic(&e, T_STRING, &k_id) && D.iter_open_container(&e, T_VARIANT, "o", &ev) &&
             D.iter_append_basic(&ev, T_PATH, &track) && D.iter_close_container(&e, &ev) && D.iter_close_container(&a, &e);
        if (ok && title[0])
            ok = D.iter_open_container(&a, T_DICT, NULL, &e) && D.iter_append_basic(&e, T_STRING, &k_title) && D.iter_open_container(&e, T_VARIANT, "s", &ev) &&
                 D.iter_append_basic(&ev, T_STRING, &title) && D.iter_close_container(&e, &ev) && D.iter_close_container(&a, &e);
        if (ok && artist[0])
            ok = D.iter_open_container(&a, T_DICT, NULL, &e) && D.iter_append_basic(&e, T_STRING, &k_artist) && D.iter_open_container(&e, T_VARIANT, "as", &ev) &&
                 D.iter_open_container(&ev, T_ARRAY, "s", &aa) && D.iter_append_basic(&aa, T_STRING, &artist) && D.iter_close_container(&ev, &aa) &&
                 D.iter_close_container(&e, &ev) && D.iter_close_container(&a, &e);
        return ok && D.iter_close_container(&v, &a) && D.iter_close_container(it, &v);
    }
    return false;
}

// All of an interface's properties as a{sv}, or those named.
static bool put_all(dbus_iter *it, const char *iface, const char *const *only, int n) {
    static const char *const root[] = { "CanQuit", "CanRaise", "HasTrackList", "Identity", "SupportedUriSchemes", "SupportedMimeTypes" };
    static const char *const player[] = { "PlaybackStatus", "Rate", "Metadata", "Volume", "Position", "MinimumRate", "MaximumRate",
                                          "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", "CanControl" };
    const char *const *names = only ? only : !SDL_strcmp(iface, ROOT) ? root : player;
    int count = only ? n : !SDL_strcmp(iface, ROOT) ? 6 : !SDL_strcmp(iface, PLAYER) ? 13 : 0;
    dbus_iter a, e;
    bool ok = D.iter_open_container(it, T_ARRAY, "{sv}", &a);
    for (int i = 0; ok && i < count; i++) ok = D.iter_open_container(&a, T_DICT, NULL, &e) && D.iter_append_basic(&e, T_STRING, &names[i]) && put_value(&e, iface, names[i]) && D.iter_close_container(&a, &e);
    return ok && D.iter_close_container(it, &a);
}

static void answer(void *m) {
    const char *iface = D.get_interface(m), *member = D.get_member(m);
    if (!member) return;
    if (!iface) iface = "";
    void *reply = NULL;
    dbus_iter it;
    int command = 0;
    if (!SDL_strcmp(iface, PLAYER) || !iface[0]) {
        static const struct { const char *name; int command; } calls[] = {
            { "Play", GS_NOWPLAYING_PLAY }, { "Pause", GS_NOWPLAYING_PAUSE }, { "PlayPause", GS_NOWPLAYING_TOGGLE },
            { "Next", GS_NOWPLAYING_NEXT }, { "Previous", GS_NOWPLAYING_PREVIOUS }, { "Stop", GS_NOWPLAYING_STOP },
        };
        for (size_t i = 0; i < SDL_arraysize(calls); i++)
            if (!SDL_strcmp(member, calls[i].name)) command = calls[i].command;
    }
    bool known = command || ((!SDL_strcmp(iface, PLAYER) || !iface[0]) && (!SDL_strcmp(member, "Seek") || !SDL_strcmp(member, "SetPosition") || !SDL_strcmp(member, "OpenUri")))
              || (!SDL_strcmp(iface, ROOT) && (!SDL_strcmp(member, "Raise") || !SDL_strcmp(member, "Quit"))) || (!SDL_strcmp(iface, PROPS) && !SDL_strcmp(member, "Set"))
              || (!SDL_strcmp(iface, "org.freedesktop.DBus.Peer") && !SDL_strcmp(member, "Ping"));
    if (known) reply = D.new_method_return(m);
    else if (!SDL_strcmp(member, "Introspect")) {
        if ((reply = D.new_method_return(m))) D.iter_init_append(reply, &it), D.iter_append_basic(&it, T_STRING, &introspection);
    } else if (!SDL_strcmp(iface, PROPS) && !SDL_strcmp(member, "GetAll")) {
        const char *of = "";
        if (D.get_args(m, NULL, T_STRING, &of, T_INVALID) && (reply = D.new_method_return(m))) D.iter_init_append(reply, &it), put_all(&it, of, NULL, 0);
    } else if (!SDL_strcmp(iface, PROPS) && !SDL_strcmp(member, "Get")) {
        const char *of = "", *name = "";
        if (D.get_args(m, NULL, T_STRING, &of, T_STRING, &name, T_INVALID) && (reply = D.new_method_return(m))) {
            D.iter_init_append(reply, &it);
            if (!put_value(&it, of, name)) D.message_unref(reply), reply = D.new_error(m, "org.freedesktop.DBus.Error.UnknownProperty", name);
        }
    }
    if (!reply) reply = D.new_error(m, "org.freedesktop.DBus.Error.UnknownMethod", member);
    if (reply) D.send(D.conn, reply, NULL), D.message_unref(reply);
    if (command) {
        SDL_Event e = { .user = { .type = D.event, .code = command } };
        SDL_PushEvent(&e);
    }
}

static int listener(void *user) {
    int fd = -1;
    D.get_unix_fd(D.conn, &fd);
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, 500);
        SDL_LockMutex(D.lock);
        if (!D.conn) { SDL_UnlockMutex(D.lock); break; }
        D.read_write(D.conn, 0);
        for (void *m; (m = D.pop_message(D.conn)); D.message_unref(m))
            if (D.message_get_type(m) == METHOD_CALL) answer(m);
        D.flush(D.conn);
        SDL_UnlockMutex(D.lock);
    }
    return 0;
}

Uint32 gs_nowplaying_init(SDL_Window *win, const char *name, const char *id) {
    if (D.conn) return D.event;
    D.lib = D.lib ? D.lib : SDL_LoadObject("libdbus-1.so.3");
    struct { void *fn; const char *name; } syms[] = {
        { &D.error_init, "dbus_error_init" }, { &D.error_free, "dbus_error_free" }, { &D.bus_get_private, "dbus_bus_get_private" },
        { &D.set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect" }, { &D.request_name, "dbus_bus_request_name" },
        { &D.read_write, "dbus_connection_read_write" }, { &D.pop_message, "dbus_connection_pop_message" }, { &D.get_unix_fd, "dbus_connection_get_unix_fd" },
        { &D.message_get_type, "dbus_message_get_type" }, { &D.get_interface, "dbus_message_get_interface" }, { &D.get_member, "dbus_message_get_member" },
        { &D.new_method_return, "dbus_message_new_method_return" }, { &D.new_error, "dbus_message_new_error" }, { &D.new_signal, "dbus_message_new_signal" },
        { &D.iter_init_append, "dbus_message_iter_init_append" }, { &D.iter_append_basic, "dbus_message_iter_append_basic" },
        { &D.iter_open_container, "dbus_message_iter_open_container" }, { &D.iter_close_container, "dbus_message_iter_close_container" },
        { &D.send, "dbus_connection_send" }, { &D.flush, "dbus_connection_flush" }, { &D.get_args, "dbus_message_get_args" },
        { &D.message_unref, "dbus_message_unref" }, { &D.close, "dbus_connection_close" }, { &D.connection_unref, "dbus_connection_unref" },
    };
    bool ok = D.lib != NULL;
    for (size_t i = 0; ok && i < SDL_arraysize(syms); i++) ok = (*(SDL_FunctionPointer *)syms[i].fn = SDL_LoadFunction(D.lib, syms[i].name)) != NULL;
    if (!ok) return 0;
    dbus_error e;
    D.error_init(&e);
    void *conn = D.bus_get_private(0 /* the session bus */, &e);
    if (!conn) return D.error_free(&e), 0;
    D.set_exit_on_disconnect(conn, 0);
    // Its name on the bus, or with this process's number when another copy has it, as the specification says.
    char bus_name[256];
    SDL_snprintf(bus_name, sizeof bus_name, ROOT ".%s", id);
    if (D.request_name(conn, bus_name, 4 /* do not queue */, NULL) != 1) {
        SDL_snprintf(bus_name, sizeof bus_name, ROOT ".%s.instance%d", id, (int)getpid());
        if (D.request_name(conn, bus_name, 4, NULL) != 1) return D.close(conn), D.connection_unref(conn), 0;
    }
    SDL_strlcpy(D.name, name, sizeof D.name);
    D.event = SDL_RegisterEvents(1), D.lock = D.lock ? D.lock : SDL_CreateMutex(), D.conn = conn;
    SDL_DetachThread(SDL_CreateThread(listener, "nowplaying", NULL));
    return D.event;
}

void gs_nowplaying_set(const char *title, const char *artist, int state) {
    if (!D.conn) return;
    SDL_LockMutex(D.lock);
    SDL_strlcpy(D.title, title ? title : "", sizeof D.title), SDL_strlcpy(D.artist, artist ? artist : "", sizeof D.artist), D.state = state;
    // PropertiesChanged(interface s, changed a{sv}, invalidated as)
    void *s = D.new_signal(PATH, PROPS, "PropertiesChanged");
    static const char *const changed[] = { "PlaybackStatus", "Metadata" };
    const char *iface = PLAYER;
    dbus_iter it, none;
    if (s) {
        D.iter_init_append(s, &it);
        if (D.iter_append_basic(&it, T_STRING, &iface) && put_all(&it, PLAYER, changed, 2) && D.iter_open_container(&it, T_ARRAY, "s", &none) && D.iter_close_container(&it, &none))
            D.send(D.conn, s, NULL), D.flush(D.conn);
        D.message_unref(s);
    }
    SDL_UnlockMutex(D.lock);
}

void gs_nowplaying_quit(void) {
    SDL_LockMutex(D.lock);
    if (D.conn) D.close(D.conn), D.connection_unref(D.conn), D.conn = NULL;  // (the listener sees it gone, and ends)
    SDL_UnlockMutex(D.lock);
}
