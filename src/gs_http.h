// HTTPS requests, on the calling thread (so from worker threads). Windows uses WinHTTP, which keeps
// connections open between requests; Linux and macOS use the system's libcurl, loaded at run time,
// with one reused handle per thread for the same reason. Where neither is usable, and on Windows for
// requests with a cookie file or streamed bodies, the curl program does the work (Windows 10 and 11
// ship one in System32); headers and bodies reach it on its standard input, never its command line,
// so tokens stay out of the process list.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *url;
    const char *method;            // NULL for GET, or POST when there is a body
    const char *agent;             // the User-Agent, or NULL for a default
    const char *const *headers;    // more "Name: value" headers, NULL-terminated, or NULL
    const char *body;              // the request body, or NULL
    size_t body_len;               // its length; 0 means strlen(body)
    const char *cookies;           // a Netscape cookie file whose cookies are sent, or NULL
    bool save_cookies;             // and the cookies the server sets saved back to it
    bool follow;                   // follow redirects
    bool compressed;               // accept a compressed response
    const char *to;                // save the response's body to this file
    int timeout_ms;                // the whole request's limit (gs_http_fetch); 0 means 30 s
    // gs_http_fetch calls this as the body arrives, with the bytes received so far and the body's
    // whole length (-1 when the server does not say), and through libcurl also about once a second
    // while nothing arrives; returning false gives the request up, which then returns 0. Or NULL.
    bool (*progress)(void *user, size_t received, long long total);
    void *progress_user;
    // Called with each header of the response, name and value, before its body arrives: for an
    // ETag or Last-Modified to compare before resuming a download, a Content-Type, a rate limit. Of a
    // followed redirect only the final response's headers are given. Or NULL. gs_http_fetch and
    // gs_http_stream both call it.
    void (*header)(void *user, const char *name, const char *value);
    void *header_user;
} gs_http_request;

// The response's HTTP status, or 0 when there was no response (a file: URL read through libcurl gives 200). Its body goes to r->to, or into
// *body (SDL_free it; zero-terminated, *len bytes) when body is not NULL, or is dropped.
int gs_http_fetch(const gs_http_request *r, char **body, size_t *len);

// Streams a GET's body to `take` as it arrives, from byte `from` on (a range request) when from > 0;
// a file: URL read through libcurl gives 200, as for gs_http_fetch.
// `take` returns false to end the transfer, and so does setting *stop, which is checked at least
// every 100 ms while waiting on the network. Returns the HTTP status, or 0 when there was no
// response; the body stops short of the whole when the connection breaks, which only `take` sees.
typedef bool gs_http_take(void *user, const uint8_t *data, size_t len);
int gs_http_stream(const gs_http_request *r, long long from, gs_http_take *take, void *user, SDL_AtomicInt *stop);

// Starts a child process with SDL, one at a time: on Windows a process started while another
// is being set up can inherit its pipes, and then neither sees its output end. On Windows the child
// also runs without a console window (SDL's background flag, so a console program started from a GUI
// app does not open one) and joins a job that ends it when the app does, however the app exits.
SDL_Process *gs_http_spawn(SDL_PropertiesID props);

// A spawned process's exit code, given the `code` SDL_WaitProcess returned for it. On Windows SDL
// reports 0 for a background process, so the real code is read from a handle gs_http_spawn keeps.
int gs_http_exit_code(SDL_Process *proc, int code);
