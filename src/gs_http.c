#include "gs_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TIMEOUT_MS 30000
#define CONNECT_TIMEOUT_MS 10000

SDL_Process *gs_http_spawn(SDL_PropertiesID props) {
    static SDL_SpinLock lock;
    SDL_LockSpinlock(&lock);
    SDL_Process *proc = SDL_CreateProcessWithProperties(props);
    SDL_UnlockSpinlock(&lock);
    return proc;
}

static size_t body_length(const gs_http_request *r) { return r->body ? (r->body_len ? r->body_len : strlen(r->body)) : 0; }

// A growing buffer for a response body, or a file it is written to; with `request` set, its
// progress hook hears of each addition (the backends without a progress callback of their own).
typedef struct { FILE *file; char *data; size_t len, cap, received; bool failed; const gs_http_request *request; long long total; } sink;

static bool sink_add(sink *s, const void *data, size_t n) {
    s->received += n;
    if (s->request && s->request->progress && !s->request->progress(s->request->progress_user, s->received, s->total)) return !(s->failed = true);
    if (s->file) return s->failed = s->failed || fwrite(data, 1, n, s->file) != n, !s->failed;
    if (s->len + n + 1 > s->cap) {
        size_t cap = (s->len + n + 1) * 2;
        char *grown = SDL_realloc(s->data, cap);
        if (!grown) return !(s->failed = true);
        s->data = grown, s->cap = cap;
    }
    memcpy(s->data + s->len, data, n);
    s->len += n;
    return true;
}

// Hands the body to the caller, or drops it; the status becomes 0 when the body could not be kept.
static int sink_finish(sink *s, int status, char **body, size_t *len) {
    if (s->file && fclose(s->file) != 0) s->failed = true;
    if (s->failed) status = 0;
    if (body && status) {
        *body = s->data ? s->data : SDL_strdup("");
        (*body)[s->len] = 0;
        if (len) *len = s->len;
    } else {
        SDL_free(s->data);
    }
    return status;
}

// ---- The curl program ----

static void curl_program(char *out, size_t size) {
#ifdef _WIN32
    const char *root = SDL_getenv("SystemRoot");
    snprintf(out, size, "%s\\System32\\curl.exe", root ? root : "C:\\Windows");
#else
    static const char *const dirs[] = { "/usr/local/bin", "/usr/bin", "/bin" };
    for (int i = 0; i < 3; i++) {
        SDL_PathInfo info;
        snprintf(out, size, "%s/curl", dirs[i]);
        if (SDL_GetPathInfo(out, &info) && info.type == SDL_PATHTYPE_FILE) return;
    }
    snprintf(out, size, "curl");
#endif
}

// Appends `name = "value"` to a curl config, escaped as curl's config files require.
static void config_line(sink *c, const char *name, const char *value, size_t n) {
    sink_add(c, name, strlen(name));
    sink_add(c, " = \"", 4);
    for (size_t i = 0; i < n; i++) {
        char ch = value[i];
        const char *esc = ch == '"' ? "\\\"" : ch == '\\' ? "\\\\" : ch == '\n' ? "\\n" : ch == '\r' ? "\\r" : ch == '\t' ? "\\t" : NULL;
        if (esc) sink_add(c, esc, 2);
        else sink_add(c, &ch, 1);
    }
    sink_add(c, "\"\n", 2);
}

// Starts curl with the request's secrets in a config read from its input (-K -). Its output is each
// response's headers (-D -, so the status can be read without a newer curl's %{stderr}), then the body.
// A fetch is limited to the request's timeout; a stream runs until it ends or is stopped.
static SDL_Process *start_curl(const gs_http_request *r, bool stream, long long from, bool follow) {
    char curl[512], range[32], timeout[16];
    curl_program(curl, sizeof curl);
    const char *args[32];
    int n = 0;
    args[n++] = curl, args[n++] = "-sS", args[n++] = "-K", args[n++] = "-";
    args[n++] = "--connect-timeout", args[n++] = "10";
    if (follow) args[n++] = "-L";
    if (r->compressed) args[n++] = "--compressed";
    if (r->cookies) args[n++] = "-b", args[n++] = r->cookies;
    if (r->cookies && r->save_cookies) args[n++] = "-c", args[n++] = r->cookies;
    if (r->method) args[n++] = "-X", args[n++] = r->method;
    if (from > 0) snprintf(range, sizeof range, "%lld-", from), args[n++] = "-r", args[n++] = range;
    if (!stream) {
        snprintf(timeout, sizeof timeout, "%d", (r->timeout_ms ? r->timeout_ms : DEFAULT_TIMEOUT_MS) / 1000 + 1);
        args[n++] = "--max-time", args[n++] = timeout;
    }
    args[n++] = "-D", args[n++] = "-", args[n] = NULL;

    sink config = { 0 };
    config_line(&config, "url", r->url, strlen(r->url));
    if (r->agent) config_line(&config, "user-agent", r->agent, strlen(r->agent));
    for (const char *const *h = r->headers; h && *h; h++) config_line(&config, "header", *h, strlen(*h));
    if (r->body) config_line(&config, "data-binary", r->body, body_length(r));

    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *proc = gs_http_spawn(p);
    SDL_DestroyProperties(p);
    SDL_IOStream *in = proc ? SDL_GetProcessInput(proc) : NULL;
    if (in) SDL_WriteIO(in, config.data, config.len), SDL_CloseIO(in);
    SDL_free(config.data);
    return proc;
}

// Reads curl's output: header blocks (a followed redirect's, then the final response's) and the body
// after the last one, passed to `take`. Returns the final status.
typedef bool body_fn(void *user, const uint8_t *data, size_t len);

static int read_curl(SDL_Process *proc, bool follow, body_fn *take, void *user, SDL_AtomicInt *stop) {
    SDL_IOStream *out = SDL_GetProcessOutput(proc);
    char head[8192];
    size_t nhead = 0;
    int status = 0;
    bool body = false, more = true;
    uint8_t buf[65536];
    while (more && !(stop && SDL_GetAtomicInt(stop))) {
        size_t got = SDL_ReadIO(out, buf, sizeof buf);
        if (!got) {
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
            SDL_Delay(5);
            continue;
        }
        size_t at = 0;
        while (!body && at < got) {  // header lines up to the blank line that ends a response's
            if (nhead < sizeof head - 1) head[nhead++] = (char)buf[at];
            at++;
            if (nhead >= 4 && !memcmp(head + nhead - 4, "\r\n\r\n", 4)) {
                head[nhead] = 0;
                const char *sp = strchr(head, ' ');
                status = sp ? atoi(sp + 1) : 0;
                nhead = 0;
                // An interim 100 response, or a redirect curl follows, is followed by another block.
                bool redirect = status >= 300 && status < 400 && (strstr(head, "\nlocation:") || strstr(head, "\nLocation:"));
                body = status != 100 && !(follow && redirect);
            }
        }
        if (body && at < got && !take(user, buf + at, got - at)) more = false;
    }
    if (!more || (stop && SDL_GetAtomicInt(stop))) SDL_KillProcess(proc, true);
    int code = -1;
    SDL_WaitProcess(proc, true, &code);
    return body && (code == 0 || !more) ? status : 0;
}

static bool to_sink(void *user, const uint8_t *data, size_t len) { return sink_add(user, data, len); }

static int fetch_with_curl(const gs_http_request *r, char **body, size_t *len) {
    SDL_Process *proc = start_curl(r, false, 0, r->follow);
    if (!proc) return 0;
    sink s = { .request = r, .total = -1 };
    if (r->to && !(s.file = fopen(r->to, "wb"))) return SDL_KillProcess(proc, true), SDL_DestroyProcess(proc), 0;
    int status = read_curl(proc, r->follow, to_sink, &s, NULL);
    SDL_DestroyProcess(proc);
    return sink_finish(&s, status, body, len);
}

static int stream_with_curl(const gs_http_request *r, long long from, gs_http_take *take, void *user, SDL_AtomicInt *stop) {
    SDL_Process *proc = start_curl(r, true, from, true);
    if (!proc) return 0;
    int status = read_curl(proc, true, take, user, stop);
    SDL_DestroyProcess(proc);
    return status;
}

// ---- libcurl (Linux and macOS) ----

#ifndef _WIN32
#include <dlfcn.h>

// The few libcurl functions and option numbers used; both are part of its stable ABI.
enum {
    URL = 10002, HTTPHEADER = 10023, POSTFIELDS = 10015, POSTFIELDSIZE_LARGE = 30120, COOKIEFILE = 10031,
    COOKIEJAR = 10082, FOLLOWLOCATION = 52, ACCEPT_ENCODING = 10102, USERAGENT = 10018, CUSTOMREQUEST = 10036,
    WRITEFUNCTION = 20011, WRITEDATA = 10001, NOSIGNAL = 99, RESPONSE_CODE = 0x200002, RANGE = 10007,
    NOPROGRESS = 43, XFERINFOFUNCTION = 20219, XFERINFODATA = 10057, CONNECTTIMEOUT_MS = 156, TIMEOUT_MS = 155,
    LOW_SPEED_LIMIT = 19, LOW_SPEED_TIME = 20, HTTPGET = 80,
};
static struct {
    int (*global_init)(long);
    void *(*easy_init)(void);
    void (*easy_reset)(void *);
    int (*easy_setopt)(void *, int, ...);
    int (*easy_perform)(void *);
    int (*easy_getinfo)(void *, int, ...);
    void *(*slist_append)(void *, const char *);
    void (*slist_free_all)(void *);
} lib;

// libcurl.so.4, or the same API built on GnuTLS that Ubuntu also installs; on macOS, the system's.
static bool load_libcurl(void) {
    static SDL_InitState state;
    static bool ok;
    if (!SDL_ShouldInit(&state)) return ok;
#ifdef __APPLE__
    void *so = dlopen("/usr/lib/libcurl.4.dylib", RTLD_NOW);
#else
    void *so = dlopen("libcurl.so.4", RTLD_NOW);
    if (!so) so = dlopen("libcurl-gnutls.so.4", RTLD_NOW);
#endif
    const struct { const char *name; void **fn; } fns[] = {
        { "curl_global_init", (void **)&lib.global_init }, { "curl_easy_init", (void **)&lib.easy_init },
        { "curl_easy_reset", (void **)&lib.easy_reset }, { "curl_easy_setopt", (void **)&lib.easy_setopt },
        { "curl_easy_perform", (void **)&lib.easy_perform }, { "curl_easy_getinfo", (void **)&lib.easy_getinfo },
        { "curl_slist_append", (void **)&lib.slist_append }, { "curl_slist_free_all", (void **)&lib.slist_free_all },
    };
    ok = so != NULL;
    for (size_t i = 0; ok && i < SDL_arraysize(fns); i++) ok = (*fns[i].fn = dlsym(so, fns[i].name)) != NULL;
    ok = ok && lib.global_init(3 /* CURL_GLOBAL_DEFAULT */) == 0;
    SDL_SetInitialized(&state, true);
    return ok;
}

// One handle per thread, kept between requests so its connections are reused.
static _Thread_local void *thread_handle;

static void *handle(void) {
    if (!thread_handle) thread_handle = lib.easy_init();
    else lib.easy_reset(thread_handle);
    return thread_handle;
}

static void *common_options(void *h, const gs_http_request *r) {
    void *headers = NULL;
    for (const char *const *x = r->headers; x && *x; x++) headers = lib.slist_append(headers, *x);
    lib.easy_setopt(h, URL, r->url);
    lib.easy_setopt(h, NOSIGNAL, 1L);  // (requests run on worker threads)
    lib.easy_setopt(h, CONNECTTIMEOUT_MS, (long)CONNECT_TIMEOUT_MS);
    if (r->agent) lib.easy_setopt(h, USERAGENT, r->agent);
    if (headers) lib.easy_setopt(h, HTTPHEADER, headers);
    if (r->body) lib.easy_setopt(h, POSTFIELDS, r->body), lib.easy_setopt(h, POSTFIELDSIZE_LARGE, (long long)body_length(r));
    if (r->method) lib.easy_setopt(h, CUSTOMREQUEST, r->method);
    if (r->cookies) lib.easy_setopt(h, COOKIEFILE, r->cookies);
    if (r->cookies && r->save_cookies) lib.easy_setopt(h, COOKIEJAR, r->cookies);  // (written when the handle resets)
    if (r->compressed) lib.easy_setopt(h, ACCEPT_ENCODING, "");  // (every encoding this libcurl has)
    return headers;
}

static size_t take_sink(const char *data, size_t size, size_t count, void *user) {
    return sink_add(user, data, size * count) ? size * count : 0;
}

// libcurl's progress callback, for a request's progress hook: nonzero gives the request up.
static int report(void *user, long long dltotal, long long dlnow, long long ultotal, long long ulnow) {
    const gs_http_request *r = user;
    (void)ultotal, (void)ulnow;
    return !r->progress(r->progress_user, (size_t)dlnow, dltotal > 0 ? dltotal : -1);
}

static int fetch_with_libcurl(const gs_http_request *r, char **body, size_t *len) {
    void *h = handle();
    if (!h) return 0;
    sink s = { 0 };
    if (r->progress) {
        lib.easy_setopt(h, NOPROGRESS, 0L);
        lib.easy_setopt(h, XFERINFOFUNCTION, report);
        lib.easy_setopt(h, XFERINFODATA, (void *)r);
    }
    if (r->to && !(s.file = fopen(r->to, "wb"))) return 0;
    void *headers = common_options(h, r);
    if (r->follow) lib.easy_setopt(h, FOLLOWLOCATION, 1L);
    lib.easy_setopt(h, TIMEOUT_MS, (long)(r->timeout_ms ? r->timeout_ms : DEFAULT_TIMEOUT_MS));
    lib.easy_setopt(h, WRITEFUNCTION, take_sink);
    lib.easy_setopt(h, WRITEDATA, &s);
    long status = 0;
    if (lib.easy_perform(h) == 0) {
        lib.easy_getinfo(h, RESPONSE_CODE, &status);
        if (!status && !SDL_strncasecmp(r->url, "file:", 5)) status = 200;  // (a local file read has no HTTP status)
    }
    if (r->cookies && r->save_cookies) lib.easy_reset(h);  // writes the cookie jar now
    lib.slist_free_all(headers);
    return sink_finish(&s, (int)status, body, len);
}

typedef struct { void *h; gs_http_take *take; void *user; SDL_AtomicInt *stop; } stream_sink;

static size_t pass(const char *data, size_t size, size_t count, void *user) {
    stream_sink *s = user;
    long status = 0;
    lib.easy_getinfo(s->h, RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) return 0;  // (an error's body is not the stream)
    return s->take(s->user, (const uint8_t *)data, size * count) ? size * count : 0;
}

// Called about once a second at least, and as data moves; nonzero ends the transfer.
static int check_stop(void *user, long long dltotal, long long dlnow, long long ultotal, long long ulnow) {
    (void)dltotal, (void)dlnow, (void)ultotal, (void)ulnow;
    return SDL_GetAtomicInt(((stream_sink *)user)->stop);
}

static int stream_with_libcurl(const gs_http_request *r, long long from, gs_http_take *take, void *user, SDL_AtomicInt *stop) {
    void *h = handle();
    if (!h) return 0;
    stream_sink s = { h, take, user, stop };
    char range[32];
    void *headers = common_options(h, r);
    lib.easy_setopt(h, FOLLOWLOCATION, 1L);
    lib.easy_setopt(h, LOW_SPEED_LIMIT, 1L), lib.easy_setopt(h, LOW_SPEED_TIME, 30L);  // a stalled stream ends
    if (from > 0) snprintf(range, sizeof range, "%lld-", from), lib.easy_setopt(h, RANGE, range);
    lib.easy_setopt(h, WRITEFUNCTION, pass);
    lib.easy_setopt(h, WRITEDATA, &s);
    lib.easy_setopt(h, NOPROGRESS, 0L);
    lib.easy_setopt(h, XFERINFOFUNCTION, check_stop);
    lib.easy_setopt(h, XFERINFODATA, &s);
    lib.easy_perform(h);  // (a break or a stop ends it early; what arrived has gone to take)
    long status = 0;
    lib.easy_getinfo(h, RESPONSE_CODE, &status);
    lib.slist_free_all(headers);
    return (int)status;
}
#endif

// ---- WinHTTP (Windows) ----

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

static wchar_t *wide(const char *s, int n) {
    int w = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    wchar_t *out = SDL_malloc((w + 1) * sizeof *out);
    if (out) MultiByteToWideChar(CP_UTF8, 0, s, n, out, w), out[w] = 0;
    return out;
}

// One session for the process: WinHTTP keeps its connections open for reuse across requests.
static HINTERNET session(void) {
    static SDL_InitState state;
    static HINTERNET s;
    if (!SDL_ShouldInit(&state)) return s;
    s = WinHttpOpen(L"gesso", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"gesso", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);  // before Windows 8.1
    SDL_SetInitialized(&state, true);
    return s;
}

static int fetch_with_winhttp(const gs_http_request *r, char **body, size_t *len) {
    HINTERNET s = session();
    wchar_t *url = s ? wide(r->url, -1) : NULL;
    URL_COMPONENTS u = { .dwStructSize = sizeof u, .dwHostNameLength = (DWORD)-1, .dwUrlPathLength = (DWORD)-1, .dwExtraInfoLength = (DWORD)-1 };
    if (!url || !WinHttpCrackUrl(url, 0, 0, &u)) return SDL_free(url), 0;
    wchar_t host[256];
    SDL_wcslcpy(host, u.lpszHostName, u.dwHostNameLength + 1 < 256 ? u.dwHostNameLength + 1 : 256);
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    const char *method = r->method ? r->method : r->body ? "POST" : "GET";
    wchar_t *wmethod = wide(method, -1);
    HINTERNET q = c ? WinHttpOpenRequest(c, wmethod, u.lpszUrlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    SDL_free(wmethod);
    sink s_body = { .request = r, .total = -1 };
    int status = 0;
    if (q && (!r->to || (s_body.file = fopen(r->to, "wb")))) {
        int total = r->timeout_ms ? r->timeout_ms : DEFAULT_TIMEOUT_MS;
        WinHttpSetTimeouts(q, CONNECT_TIMEOUT_MS, CONNECT_TIMEOUT_MS, total, total);
        DWORD policy = r->follow ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(q, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        if (r->compressed) {
            DWORD all = WINHTTP_DECOMPRESSION_FLAG_ALL;  // (Windows 8.1 and later; ignored before)
            WinHttpSetOption(q, WINHTTP_OPTION_DECOMPRESSION, &all, sizeof all);
        }
        sink head = { 0 };
        if (r->agent) sink_add(&head, "User-Agent: ", 12), sink_add(&head, r->agent, strlen(r->agent)), sink_add(&head, "\r\n", 2);
        for (const char *const *h = r->headers; h && *h; h++) sink_add(&head, *h, strlen(*h)), sink_add(&head, "\r\n", 2);
        wchar_t *headers = head.len ? wide(head.data, (int)head.len) : NULL;
        SDL_free(head.data);
        size_t blen = body_length(r);
        if (WinHttpSendRequest(q, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0,
                               (void *)r->body, (DWORD)blen, (DWORD)blen, 0) &&
            WinHttpReceiveResponse(q, NULL)) {
            DWORD code = 0, size = sizeof code;
            if (WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX))
                status = (int)code;
            wchar_t length[32];
            DWORD lsize = sizeof length;
            if (WinHttpQueryHeaders(q, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &lsize, WINHTTP_NO_HEADER_INDEX))
                s_body.total = (long long)wcstoll(length, NULL, 10);
            char buf[65536];
            DWORD got = 0;
            BOOL read = TRUE;
            while (status && (read = WinHttpReadData(q, buf, sizeof buf, &got)) && got)
                if (!sink_add(&s_body, buf, got)) break;
            if (!read) status = 0;  // the connection broke or timed out mid-body
        }
        SDL_free(headers);
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    SDL_free(url);
    return sink_finish(&s_body, status, body, len);
}
#endif

// GS_HTTP_CURL_PROGRAM=1 sends every request through the curl program, for testing that path.
static bool program_only(void) { return SDL_GetHintBoolean("GS_HTTP_CURL_PROGRAM", false); }

int gs_http_fetch(const gs_http_request *r, char **body, size_t *len) {
#ifdef _WIN32
    if (!r->cookies && !program_only()) return fetch_with_winhttp(r, body, len);
#else
    if (!program_only() && load_libcurl()) return fetch_with_libcurl(r, body, len);
#endif
    return fetch_with_curl(r, body, len);
}

int gs_http_stream(const gs_http_request *r, long long from, gs_http_take *take, void *user, SDL_AtomicInt *stop) {
#ifndef _WIN32
    if (!program_only() && load_libcurl()) return stream_with_libcurl(r, from, take, user, stop);
#endif
    return stream_with_curl(r, from, take, user, stop);
}
