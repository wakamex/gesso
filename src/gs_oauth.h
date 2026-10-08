// Signing in with the OAuth 2.0 device authorization grant (RFC 8628): the app shows a short code,
// the user enters it on the provider's page in their own browser, and the app polls until the
// provider hands over tokens. No embedded browser and no client secret for public clients. The
// provider's endpoints and quirks are parameters, so the same code signs in to Twitch, Google,
// GitHub or Microsoft. Requests go through gs_http, so call these from a worker thread.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *device_url;      // where a code is requested
    const char *token_url;       // where codes and refresh tokens are exchanged for tokens
    const char *client_id;
    const char *client_secret;   // NULL for a public client (Google's device flow needs one)
    const char *scope;           // space-separated
    const char *scope_field;     // the device request's scope field: NULL for RFC 8628's "scope" (Twitch takes "scopes")
} gs_oauth_client;

typedef struct {
    char device_code[1024], user_code[64], verification_uri[512];
    double interval;   // seconds to wait between polls; raised when the provider asks to slow down
    double expires_in; // seconds the code lasts from when it was issued
} gs_oauth_device;

typedef struct {
    char access_token[4096], refresh_token[4096], scope[1024];
    double expires_in;  // seconds; 0 when the provider did not say
} gs_oauth_token;

typedef enum {
    GS_OAUTH_OK,
    GS_OAUTH_PENDING,    // the user has not finished on the provider's page yet: poll again
    GS_OAUTH_SLOW_DOWN,  // the same, polling too often: the interval has been raised
    GS_OAUTH_DENIED,     // the user declined
    GS_OAUTH_EXPIRED,    // the code ran out: start again
    GS_OAUTH_INVALID,    // the code or refresh token is not (or no longer) valid: sign in again
    GS_OAUTH_NETWORK,    // no answer from the provider: try again later
    GS_OAUTH_ERROR,      // anything else; the message says what
} gs_oauth_result;

// Each writes the provider's own explanation, or a description of the failure, to `message`.
gs_oauth_result gs_oauth_start(const gs_oauth_client *c, gs_oauth_device *d, char *message, size_t size);
gs_oauth_result gs_oauth_poll(const gs_oauth_client *c, gs_oauth_device *d, gs_oauth_token *t, char *message, size_t size);
gs_oauth_result gs_oauth_refresh(const gs_oauth_client *c, const char *refresh_token, gs_oauth_token *t, char *message, size_t size);

// Percent-encodes `value` for a URL query or a form body ("a b" becomes "a%20b"). Returns the
// length written, or 0 when it does not fit.
size_t gs_oauth_encode(const char *value, char *out, size_t size);
