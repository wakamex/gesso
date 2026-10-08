#include "gs_hls.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

// The next line of text (without its line ending), or false at the end.
static bool next_line(const char **p, const char *end, const char **line, size_t *n) {
    while (*p < end && (**p == '\n' || **p == '\r')) (*p)++;
    if (*p >= end) return false;
    *line = *p;
    while (*p < end && **p != '\n' && **p != '\r') (*p)++;
    *n = (size_t)(*p - *line);
    while (*n && ((*line)[*n - 1] == ' ' || (*line)[*n - 1] == '\t')) (*n)--;
    return true;
}

static bool starts(const char *line, size_t n, const char *prefix) {
    size_t k = strlen(prefix);
    return n >= k && !memcmp(line, prefix, k);
}

bool gs_hls_attr(const char *attrs, const char *name, char *out, size_t size) {
    size_t k = strlen(name);
    for (const char *p = attrs; *p;) {
        while (*p == ',' || *p == ' ') p++;
        const char *key = p;
        while (*p && *p != '=' && *p != ',') p++;
        bool match = (size_t)(p - key) == k && !SDL_strncasecmp(key, name, k);
        if (*p != '=') continue;
        p++;
        const char *v = p, *v_end;
        if (*p == '"') {
            v = ++p;
            while (*p && *p != '"') p++;
            v_end = p;
            if (*p) p++;
        } else {
            while (*p && *p != ',') p++;
            v_end = p;
        }
        if (match) {
            size_t n = (size_t)(v_end - v);
            if (size) SDL_strlcpy(out, v, n + 1 < size ? n + 1 : size);
            return true;
        }
    }
    return false;
}

bool gs_hls_resolve(const char *base, const char *url, char *out, size_t size) {
    if (SDL_strstr(url, "://") || !base || !base[0]) return (size_t)SDL_snprintf(out, size, "%s", url) < size;
    const char *scheme = SDL_strstr(base, "://");
    if (!scheme) return (size_t)SDL_snprintf(out, size, "%s", url) < size;
    if (url[0] == '/' && url[1] == '/')  // scheme-relative
        return (size_t)SDL_snprintf(out, size, "%.*s:%s", (int)(scheme - base), base, url) < size;
    const char *host = scheme + 3, *path = strchr(host, '/');
    if (url[0] == '/')  // host-relative
        return (size_t)SDL_snprintf(out, size, "%.*s%s", (int)((path ? path : host + strlen(host)) - base), base, url) < size;
    const char *query = strchr(host, '?');
    const char *last = path ? path : NULL;
    for (const char *s = path; s && *s && (!query || s < query); s++)
        if (*s == '/') last = s;
    if (!last) return (size_t)SDL_snprintf(out, size, "%s/%s", base, url) < size;
    return (size_t)SDL_snprintf(out, size, "%.*s%s", (int)(last + 1 - base), base, url) < size;
}

// ---- Master playlists ----

int gs_hls_master(const char *text, size_t len, const char *base_url, gs_hls_variant *out, int max) {
    const char *p = text, *end = text + len, *line;
    size_t n;
    if (!next_line(&p, end, &line, &n) || !starts(line, n, "#EXTM3U")) return -1;
    // Video groups' names, for variants that name a group.
    struct { char group[64], name[64]; } groups[32];
    int ngroups = 0, count = 0;
    bool pending = false, master = false;
    gs_hls_variant v;
    char attrs[1024];
    while (next_line(&p, end, &line, &n)) {
        if (starts(line, n, "#EXTINF")) return -1;  // a media playlist
        if (starts(line, n, "#EXT-X-MEDIA:") && ngroups < 32) {
            SDL_strlcpy(attrs, line + 13, n - 13 + 1 < sizeof attrs ? n - 13 + 1 : sizeof attrs);
            char type[16];
            if (gs_hls_attr(attrs, "TYPE", type, sizeof type) && !SDL_strcasecmp(type, "VIDEO") &&
                gs_hls_attr(attrs, "GROUP-ID", groups[ngroups].group, sizeof groups[ngroups].group)) {
                if (!gs_hls_attr(attrs, "NAME", groups[ngroups].name, sizeof groups[ngroups].name)) groups[ngroups].name[0] = 0;
                ngroups++;
            }
        } else if (starts(line, n, "#EXT-X-STREAM-INF:")) {
            master = pending = true;
            memset(&v, 0, sizeof v);
            SDL_strlcpy(v.attrs, line + 18, n - 18 + 1 < sizeof v.attrs ? n - 18 + 1 : sizeof v.attrs);
            char value[64];
            if (gs_hls_attr(v.attrs, "AVERAGE-BANDWIDTH", value, sizeof value) || gs_hls_attr(v.attrs, "BANDWIDTH", value, sizeof value))
                v.bandwidth = SDL_strtoll(value, NULL, 10);
            if (gs_hls_attr(v.attrs, "RESOLUTION", value, sizeof value)) {
                v.width = SDL_atoi(value);
                const char *x = SDL_strchr(value, 'x');
                v.height = x ? SDL_atoi(x + 1) : 0;
            }
            if (gs_hls_attr(v.attrs, "FRAME-RATE", value, sizeof value)) v.fps = SDL_atof(value);
            gs_hls_attr(v.attrs, "CODECS", v.codecs, sizeof v.codecs);
            char group[64];
            if (gs_hls_attr(v.attrs, "VIDEO", group, sizeof group))
                for (int i = 0; i < ngroups; i++)
                    if (!strcmp(groups[i].group, group)) SDL_strlcpy(v.name, groups[i].name, sizeof v.name);
            v.audio_only = !v.height && !SDL_strstr(v.codecs, "avc1") && !SDL_strstr(v.codecs, "hvc1") && !SDL_strstr(v.codecs, "hev1") && !SDL_strstr(v.codecs, "av01");
        } else if (line[0] != '#' && pending) {
            char uri[2048];
            SDL_strlcpy(uri, line, n + 1 < sizeof uri ? n + 1 : sizeof uri);
            if (count < max && gs_hls_resolve(base_url, uri, v.url, sizeof v.url)) out[count] = v;
            count++;
            pending = false;
        }
    }
    return master ? count : -1;
}

// ---- Media playlists ----

// Text is gathered in one growing buffer and addressed by offset until parsing ends, since the buffer moves.
typedef struct { char *data; size_t len, cap; } arena;

static size_t put(arena *a, const char *s, size_t n) {
    if (a->len + n + 1 > a->cap) {
        size_t cap = (a->len + n + 1) * 2;
        char *grown = SDL_realloc(a->data, cap);
        if (!grown) return (size_t)-1;
        a->data = grown, a->cap = cap;
    }
    size_t at = a->len;
    memcpy(a->data + at, s, n);
    a->data[at + n] = 0;
    a->len += n + 1;
    return at;
}

// Appends a line to a newline-separated list.
static void add_line(arena *a, const char *line, size_t n) {
    if (a->len + n + 2 > a->cap) {
        size_t cap = (a->len + n + 2) * 2;
        char *grown = SDL_realloc(a->data, cap);
        if (!grown) return;
        a->data = grown, a->cap = cap;
    }
    if (a->len) a->data[a->len++] = '\n';
    memcpy(a->data + a->len, line, n);
    a->len += n;
}

typedef struct { long long sequence; double duration; size_t title, url, map, tags; bool discontinuity; } raw_segment;

gs_hls_media *gs_hls_media_parse(const char *text, size_t len, const char *base_url) {
    const char *p = text, *end = text + len, *line;
    size_t n;
    if (!next_line(&p, end, &line, &n) || !starts(line, n, "#EXTM3U")) return NULL;
    arena a = { 0 }, tags = { 0 };
    raw_segment *segs = NULL;
    int count = 0, cap = 0;
    double target = 0, duration = 0;
    long long sequence = 0;
    bool ended = false, discontinuity = false, media = false, extinf = false;
    size_t empty = put(&a, "", 0), title = empty, map = (size_t)-1;
    char buf[4096];
    while (next_line(&p, end, &line, &n)) {
        if (starts(line, n, "#EXT-X-STREAM-INF")) { media = false; break; }  // a master playlist
        if (starts(line, n, "#EXT-X-TARGETDURATION:")) {
            target = SDL_atof(line + 22), media = true;
        } else if (starts(line, n, "#EXT-X-MEDIA-SEQUENCE:")) {
            sequence = SDL_strtoll(line + 22, NULL, 10);
        } else if (starts(line, n, "#EXT-X-VERSION:")) {
            // (the playlist's format, not a segment's tag)
        } else if (starts(line, n, "#EXT-X-ENDLIST")) {
            ended = true;
        } else if (starts(line, n, "#EXT-X-DISCONTINUITY") && n == 20) {
            discontinuity = true;
        } else if (starts(line, n, "#EXT-X-MAP:")) {
            SDL_strlcpy(buf, line + 11, n - 11 + 1 < sizeof buf ? n - 11 + 1 : sizeof buf);
            char uri[2048], resolved[2048];
            if (gs_hls_attr(buf, "URI", uri, sizeof uri) && gs_hls_resolve(base_url, uri, resolved, sizeof resolved))
                map = put(&a, resolved, strlen(resolved));
        } else if (starts(line, n, "#EXTINF:")) {
            media = extinf = true;
            duration = SDL_atof(line + 8);
            const char *comma = memchr(line, ',', n);
            title = comma ? put(&a, comma + 1, (size_t)(line + n - comma - 1)) : empty;
        } else if (line[0] == '#') {
            add_line(&tags, line, n);
        } else if (extinf) {
            if (count == cap) {
                cap = cap ? cap * 2 : 16;
                raw_segment *grown = SDL_realloc(segs, cap * sizeof *segs);
                if (!grown) break;
                segs = grown;
            }
            SDL_strlcpy(buf, line, n + 1 < sizeof buf ? n + 1 : sizeof buf);
            char resolved[4096];
            if (!gs_hls_resolve(base_url, buf, resolved, sizeof resolved)) SDL_strlcpy(resolved, buf, sizeof resolved);
            segs[count] = (raw_segment){ sequence + count, duration, title, put(&a, resolved, strlen(resolved)), map,
                                         put(&a, tags.data ? tags.data : "", tags.len), discontinuity };
            count++;
            tags.len = 0, discontinuity = extinf = false, title = empty;
        }
    }
    gs_hls_media *m = media ? SDL_calloc(1, sizeof *m) : NULL;
    if (m) {
        m->target_duration = target, m->sequence = sequence, m->ended = ended, m->count = count;
        size_t tail = put(&a, tags.data ? tags.data : "", tags.len);
        // One block: the struct, the segments, then the text they point into.
        gs_hls_media *block = SDL_malloc(sizeof *m + count * sizeof *m->segments + a.len);
        if (block) {
            *block = *m;
            block->segments = (gs_hls_segment *)(block + 1);
            char *t = (char *)(block->segments + count);
            memcpy(t, a.data, a.len);
            block->tail = t + tail;
            for (int i = 0; i < count; i++) {
                raw_segment *r = &segs[i];
                block->segments[i] = (gs_hls_segment){ r->sequence, r->duration, t + r->title, t + r->url,
                                                       r->map == (size_t)-1 ? NULL : t + r->map, t + r->tags, r->discontinuity };
            }
        }
        SDL_free(m);
        m = block;
    }
    SDL_free(a.data), SDL_free(tags.data), SDL_free(segs);
    return m;
}

void gs_hls_media_free(gs_hls_media *m) { SDL_free(m); }
