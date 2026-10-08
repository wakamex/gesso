#include <string.h>

#include "gs_hls.h"
#include "test.h"

static const char master[] =
    "#EXTM3U\n"
    "#EXT-X-SESSION-DATA:DATA-ID=\"NODE\",VALUE=\"x\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=7982196,RESOLUTION=1920x1080,CODECS=\"avc1.64002A,mp4a.40.2\",FRAME-RATE=60.000,STABLE-VARIANT-ID=\"1080p60\",IVS-VARIANT-SOURCE=\"source\"\n"
    "https://cdn.example/v1/playlist/a.m3u8\n"
    "#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID=\"720p30\",NAME=\"720p\",AUTOSELECT=YES\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=3000000,AVERAGE-BANDWIDTH=2500000,RESOLUTION=1280x720,CODECS=\"avc1.4D401F,mp4a.40.2\",VIDEO=\"720p30\",FRAME-RATE=30.000\n"
    "b/720.m3u8\r\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=160000,CODECS=\"mp4a.40.2\",STABLE-VARIANT-ID=\"audio_only\"\n"
    "/audio.m3u8?x=1\n";

static const char media[] =
    "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:2407\n"
    "#EXT-X-MAP:URI=\"init-0.mp4\"\n"
    "#EXT-X-PROGRAM-DATE-TIME:2026-10-08T17:26:03.620Z\n#EXTINF:2.000,live\nseg-1.mp4\n"
    "#EXT-X-DISCONTINUITY\n#EXT-X-DATERANGE:ID=\"ad\",CLASS=\"twitch-stitched-ad\"\n#EXT-X-MAP:URI=\"https://ads.example/init.mp4\"\n"
    "#EXTINF:2.002,Amazon\nhttps://ads.example/ad-1.mp4\n"
    "#EXT-X-TWITCH-PREFETCH:https://cdn.example/next.mp4\n";

void test_hls(void) {
    gs_hls_variant v[4];
    int n = gs_hls_master(master, sizeof master - 1, "https://usher.example/api/channel/hls/x.m3u8?sig=1", v, 4);
    CHECK(n == 3);
    CHECK(v[0].height == 1080 && v[0].width == 1920 && v[0].fps == 60 && v[0].bandwidth == 7982196 && !v[0].audio_only);
    char value[32];
    CHECK(gs_hls_attr(v[0].attrs, "STABLE-VARIANT-ID", value, sizeof value) && !strcmp(value, "1080p60"));
    CHECK(gs_hls_attr(v[0].attrs, "ivs-variant-source", value, sizeof value) && !strcmp(value, "source"));
    CHECK(!gs_hls_attr(v[0].attrs, "VIDEO", value, sizeof value));
    CHECK(!strcmp(v[1].name, "720p") && v[1].bandwidth == 2500000 && !strcmp(v[1].codecs, "avc1.4D401F,mp4a.40.2"));
    CHECK(!strcmp(v[1].url, "https://usher.example/api/channel/hls/b/720.m3u8"));
    CHECK(v[2].audio_only && !strcmp(v[2].url, "https://usher.example/audio.m3u8?x=1"));
    CHECK(gs_hls_master(media, sizeof media - 1, NULL, v, 4) == -1);

    gs_hls_media *m = gs_hls_media_parse(media, sizeof media - 1, "https://cdn.example/v1/playlist/a.m3u8");
    CHECK(m && m->count == 2 && m->sequence == 2407 && m->target_duration == 6 && !m->ended);
    if (m && m->count == 2) {
        gs_hls_segment *s = m->segments;
        CHECK(s[0].sequence == 2407 && s[0].duration == 2 && !strcmp(s[0].title, "live") && !s[0].discontinuity);
        CHECK(!strcmp(s[0].url, "https://cdn.example/v1/playlist/seg-1.mp4"));
        CHECK(!strcmp(s[0].map_url, "https://cdn.example/v1/playlist/init-0.mp4"));
        CHECK(!strcmp(s[0].tags, "#EXT-X-PROGRAM-DATE-TIME:2026-10-08T17:26:03.620Z"));
        CHECK(s[1].sequence == 2408 && s[1].discontinuity && !strcmp(s[1].title, "Amazon"));
        CHECK(!strcmp(s[1].map_url, "https://ads.example/init.mp4"));
        CHECK(strstr(s[1].tags, "twitch-stitched-ad") != NULL);
        CHECK(!strcmp(m->tail, "#EXT-X-TWITCH-PREFETCH:https://cdn.example/next.mp4"));
    }
    gs_hls_media_free(m);
    CHECK(gs_hls_media_parse(master, sizeof master - 1, NULL) == NULL);

    char out[256];
    CHECK(gs_hls_resolve("https://a.example/x/y.m3u8", "//b.example/z", out, sizeof out) && !strcmp(out, "https://b.example/z"));
    CHECK(gs_hls_resolve("https://a.example", "z.ts", out, sizeof out) && !strcmp(out, "https://a.example/z.ts"));
}
