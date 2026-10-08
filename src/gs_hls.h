// HTTP Live Streaming (RFC 8216) playlists: a master playlist's renditions, and a media playlist's
// segments with the tags before each. Tags gesso does not interpret reach the app as text, so a
// service's own tags (Twitch's ad markers and prefetch segments, say) stay the app's business.
// URLs in playlists are resolved against the playlist's own URL.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// ---- Master playlists ----

typedef struct {
    char url[2048];
    char name[64];        // the NAME of its EXT-X-MEDIA video group, when it has one
    char codecs[128];
    char attrs[1024];     // its EXT-X-STREAM-INF attribute list, for gs_hls_attr
    long long bandwidth;  // AVERAGE-BANDWIDTH when given, else BANDWIDTH
    int width, height;    // 0 when not given
    double fps;           // 0 when not given
    bool audio_only;      // no RESOLUTION and no video codec
} gs_hls_variant;

// Fills up to `max` variants and returns how many the playlist has (more than max when it was cut
// short), or -1 when the text is not a master playlist.
int gs_hls_master(const char *text, size_t len, const char *base_url, gs_hls_variant *out, int max);

// Copies the value of attribute `name` from an attribute list (quotes removed). False when absent.
bool gs_hls_attr(const char *attrs, const char *name, char *out, size_t size);

// ---- Media playlists ----

typedef struct {
    long long sequence;   // its media sequence number
    double duration;      // from EXTINF
    const char *title;    // EXTINF's title, such as Twitch's "live" or "Amazon"; "" when none
    const char *url;
    const char *map_url;  // the EXT-X-MAP initialization segment in effect (fragmented MP4), or NULL
    const char *tags;     // the tag lines before it other than EXTINF, newline-separated; "" when none
    bool discontinuity;   // EXT-X-DISCONTINUITY before it
} gs_hls_segment;

typedef struct {
    double target_duration;
    long long sequence;   // EXT-X-MEDIA-SEQUENCE: the first segment's number
    bool ended;           // EXT-X-ENDLIST: no more segments will be added
    const char *tail;     // tag lines after the last segment (where Twitch lists prefetch segments)
    int count;
    gs_hls_segment *segments;
} gs_hls_media;

// NULL when the text is not a media playlist. Strings point into the result; free it whole.
gs_hls_media *gs_hls_media_parse(const char *text, size_t len, const char *base_url);
void gs_hls_media_free(gs_hls_media *m);

// Resolves a possibly relative URL against the playlist's URL into out. False when it does not fit.
bool gs_hls_resolve(const char *base_url, const char *url, char *out, size_t size);
