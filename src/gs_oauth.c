#include "gs_oauth.h"

#include <SDL3/SDL.h>
#include <string.h>

#include "gs_http.h"
#include "gs_json.h"

size_t gs_oauth_encode(const char *value, char *out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        bool plain = SDL_isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (n + (plain ? 1 : 3) >= size) return 0;
        if (plain) out[n++] = (char)*p;
        else out[n++] = '%', out[n++] = hex[*p >> 4], out[n++] = hex[*p & 15];
    }
    out[n] = 0;
    return n;
}

// A form body built from name/value pairs (a NULL name ends the list; NULL values are skipped).
static bool form(char *out, size_t size, const char *const *pairs) {
    size_t n = 0;
    out[0] = 0;
    for (; pairs[0]; pairs += 2) {
        if (!pairs[1]) continue;
        size_t k = SDL_snprintf(out + n, size - n, "%s%s=", n ? "&" : "", pairs[0]);
        if (n + k >= size) return false;
        n += k;
        size_t v = gs_oauth_encode(pairs[1], out + n, size - n);
        if (!v && pairs[1][0]) return false;
        n += v;
    }
    return true;
}

static gs_oauth_result post(const char *url, const char *body, gs_json **doc, char *message, size_t size) {
    const char *headers[] = { "Content-Type: application/x-www-form-urlencoded", "Accept: application/json", NULL };
    gs_http_request r = { .url = url, .headers = headers, .body = body, .timeout_ms = 15000 };
    char *text = NULL;
    size_t len = 0;
    int status = gs_http_fetch(&r, &text, &len);
    *doc = text ? gs_json_parse(text, len) : NULL;
    SDL_free(text);
    if (!status) return SDL_strlcpy(message, "no answer from the sign-in server", size), GS_OAUTH_NETWORK;
    gs_jv root = gs_json_root(*doc);
    if (status >= 200 && status < 300 && root) return message[0] = 0, GS_OAUTH_OK;
    // RFC 8628 puts the reason in "error"; Twitch puts it in "message".
    const char *error = gs_json_str(gs_json_get(root, "error"), "");
    const char *said = gs_json_str(gs_json_get(root, "message"), gs_json_str(gs_json_get(root, "error_description"), ""));
    const char *why = error[0] ? error : said;
    SDL_snprintf(message, size, "%s", said[0] ? said : why[0] ? why : "the sign-in server refused the request");
    if (SDL_strcasestr(why, "authorization_pending")) return GS_OAUTH_PENDING;
    if (SDL_strcasestr(why, "slow_down")) return GS_OAUTH_SLOW_DOWN;
    if (SDL_strcasestr(why, "access_denied")) return GS_OAUTH_DENIED;
    if (SDL_strcasestr(why, "expired")) return GS_OAUTH_EXPIRED;
    if (SDL_strcasestr(why, "invalid_grant") || SDL_strcasestr(why, "invalid refresh token") || SDL_strcasestr(why, "invalid device code"))
        return GS_OAUTH_INVALID;
    if (status >= 500) return GS_OAUTH_NETWORK;
    return GS_OAUTH_ERROR;
}

gs_oauth_result gs_oauth_start(const gs_oauth_client *c, gs_oauth_device *d, char *message, size_t size) {
    char body[2048];
    const char *pairs[] = { "client_id", c->client_id, c->scope_field ? c->scope_field : "scope", c->scope, "client_secret", c->client_secret, NULL };
    if (!form(body, sizeof body, pairs)) return SDL_strlcpy(message, "the request is too long", size), GS_OAUTH_ERROR;
    gs_json *doc;
    gs_oauth_result r = post(c->device_url, body, &doc, message, size);
    gs_jv root = gs_json_root(doc);
    if (r == GS_OAUTH_OK) {
        SDL_strlcpy(d->device_code, gs_json_str(gs_json_get(root, "device_code"), ""), sizeof d->device_code);
        SDL_strlcpy(d->user_code, gs_json_str(gs_json_get(root, "user_code"), ""), sizeof d->user_code);
        // Google calls the page verification_url; a complete URI carries the code already.
        const char *uri = gs_json_str(gs_json_get(root, "verification_uri_complete"), gs_json_str(gs_json_get(root, "verification_uri"), gs_json_str(gs_json_get(root, "verification_url"), "")));
        SDL_strlcpy(d->verification_uri, uri, sizeof d->verification_uri);
        d->interval = gs_json_num(gs_json_get(root, "interval"), 5);
        d->expires_in = gs_json_num(gs_json_get(root, "expires_in"), 600);
        if (!d->device_code[0] || !d->user_code[0] || !d->verification_uri[0])
            r = GS_OAUTH_ERROR, SDL_strlcpy(message, "the sign-in server's answer has no code", size);
    }
    gs_json_free(doc);
    return r;
}

static gs_oauth_result take_token(gs_oauth_result r, gs_json *doc, gs_oauth_token *t, char *message, size_t size) {
    gs_jv root = gs_json_root(doc);
    if (r == GS_OAUTH_OK) {
        SDL_strlcpy(t->access_token, gs_json_str(gs_json_get(root, "access_token"), ""), sizeof t->access_token);
        SDL_strlcpy(t->refresh_token, gs_json_str(gs_json_get(root, "refresh_token"), ""), sizeof t->refresh_token);
        // Scopes come as a space-separated string (RFC 6749) or, from Twitch, as an array.
        gs_jv scope = gs_json_get(root, "scope");
        t->scope[0] = 0;
        if (gs_json_kind(scope) == GS_JSON_STRING) SDL_strlcpy(t->scope, gs_json_str(scope, ""), sizeof t->scope);
        for (gs_jv s = gs_json_kind(scope) == GS_JSON_ARRAY ? gs_json_first(scope) : NULL; s; s = gs_json_next(s)) {
            if (t->scope[0]) SDL_strlcat(t->scope, " ", sizeof t->scope);
            SDL_strlcat(t->scope, gs_json_str(s, ""), sizeof t->scope);
        }
        t->expires_in = gs_json_num(gs_json_get(root, "expires_in"), 0);
        if (!t->access_token[0]) r = GS_OAUTH_ERROR, SDL_strlcpy(message, "the sign-in server's answer has no token", size);
    }
    gs_json_free(doc);
    return r;
}

gs_oauth_result gs_oauth_poll(const gs_oauth_client *c, gs_oauth_device *d, gs_oauth_token *t, char *message, size_t size) {
    char body[4096];
    const char *pairs[] = { "client_id", c->client_id, "client_secret", c->client_secret, "scope", c->scope, "device_code", d->device_code,
                            "grant_type", "urn:ietf:params:oauth:grant-type:device_code", NULL };
    if (!form(body, sizeof body, pairs)) return SDL_strlcpy(message, "the request is too long", size), GS_OAUTH_ERROR;
    gs_json *doc;
    gs_oauth_result r = post(c->token_url, body, &doc, message, size);
    if (r == GS_OAUTH_SLOW_DOWN) d->interval += 5;  // as RFC 8628 asks
    return take_token(r, doc, t, message, size);
}

gs_oauth_result gs_oauth_refresh(const gs_oauth_client *c, const char *refresh_token, gs_oauth_token *t, char *message, size_t size) {
    char body[8192];
    const char *pairs[] = { "client_id", c->client_id, "client_secret", c->client_secret, "refresh_token", refresh_token, "grant_type", "refresh_token", NULL };
    if (!form(body, sizeof body, pairs)) return SDL_strlcpy(message, "the request is too long", size), GS_OAUTH_ERROR;
    gs_json *doc;
    gs_oauth_result r = post(c->token_url, body, &doc, message, size);
    r = take_token(r, doc, t, message, size);
    if (r == GS_OAUTH_OK && !t->refresh_token[0]) SDL_strlcpy(t->refresh_token, refresh_token, sizeof t->refresh_token);  // (kept when not rotated)
    return r;
}
