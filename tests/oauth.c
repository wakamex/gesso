// gs_oauth_encode always; the device flow against Twitch's real endpoints with GS_TEST_NETWORK=1 and
// a public Client ID in GS_TEST_TWITCH_CLIENT_ID.
#include <SDL3/SDL.h>
#include <string.h>

#include "gs_oauth.h"
#include "test.h"

void test_oauth(void) {
    char out[64];
    CHECK(gs_oauth_encode("user:read:follows a/b", out, sizeof out) == 29 && !strcmp(out, "user%3Aread%3Afollows%20a%2Fb"));
    CHECK(gs_oauth_encode("abcdef", out, 4) == 0);

    const char *id = SDL_getenv("GS_TEST_TWITCH_CLIENT_ID");
    if (!SDL_GetHintBoolean("GS_TEST_NETWORK", false) || !id || !id[0]) return;
    gs_oauth_client twitch = { .device_url = "https://id.twitch.tv/oauth2/device", .token_url = "https://id.twitch.tv/oauth2/token",
                               .client_id = id, .scope = "user:read:follows", .scope_field = "scopes" };
    gs_oauth_device d = { 0 };
    char msg[256];
    CHECK(gs_oauth_start(&twitch, &d, msg, sizeof msg) == GS_OAUTH_OK);
    CHECK(d.user_code[0] && strstr(d.verification_uri, "twitch.tv") && d.interval > 0);
    gs_oauth_token t = { 0 };
    gs_oauth_result r = gs_oauth_poll(&twitch, &d, &t, msg, sizeof msg);
    CHECK(r == GS_OAUTH_PENDING);
    if (r != GS_OAUTH_PENDING) fprintf(stderr, "  poll: %d %s\n", (int)r, msg);
    r = gs_oauth_refresh(&twitch, "not-a-refresh-token", &t, msg, sizeof msg);
    CHECK(r == GS_OAUTH_INVALID);
    if (r != GS_OAUTH_INVALID) fprintf(stderr, "  refresh: %d %s\n", (int)r, msg);
}
