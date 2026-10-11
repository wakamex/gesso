#include "gs_json.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

// Nodes live in one array, linked by offsets relative to themselves, so a node pointer is enough to
// walk the tree. Strings are decoded in place in a copy of the text (decoding never lengthens one).
struct gs_json_node {
    unsigned char kind;
    int first, next;  // offsets to the first child and the next sibling; 0 for none
    int count;
    const char *key, *str;
    double num;
};

struct gs_json {
    struct gs_json_node *nodes;
    char *text;
};

typedef struct {
    char *p, *end;
    struct gs_json_node *nodes;
    int n, cap, depth;
} parser;

enum { MAX_DEPTH = 512 };

static void skip_space(parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static int new_node(parser *ps, int kind) {
    if (ps->n == ps->cap) {
        int cap = ps->cap ? ps->cap * 2 : 256;
        struct gs_json_node *nodes = realloc(ps->nodes, sizeof *nodes * (size_t)cap);
        if (!nodes) return -1;
        ps->nodes = nodes, ps->cap = cap;
    }
    memset(&ps->nodes[ps->n], 0, sizeof *ps->nodes);
    ps->nodes[ps->n].kind = (unsigned char)kind;
    return ps->n++;
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1000000);
    }
    return v;
}

static char *put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) *o++ = (char)(0xC0 | cp >> 6), *o++ = (char)(0x80 | (cp & 0x3F));
    else if (cp < 0x10000) *o++ = (char)(0xE0 | cp >> 12), *o++ = (char)(0x80 | (cp >> 6 & 0x3F)), *o++ = (char)(0x80 | (cp & 0x3F));
    else *o++ = (char)(0xF0 | cp >> 18), *o++ = (char)(0x80 | (cp >> 12 & 0x3F)), *o++ = (char)(0x80 | (cp >> 6 & 0x3F)), *o++ = (char)(0x80 | (cp & 0x3F));
    return o;
}

// Decodes the string at p (after its opening quote) in place; returns it, or NULL if malformed.
static const char *string(parser *ps) {
    char *start = ++ps->p, *o = start;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if ((unsigned char)c < 0x20) return NULL;
        if (c != '\\') { *o++ = c; continue; }
        if (ps->p >= ps->end) return NULL;
        switch (c = *ps->p++) {
        case '"': case '\\': case '/': *o++ = c; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            if (ps->end - ps->p < 4) return NULL;
            int cp = hex4(ps->p);
            ps->p += 4;
            if (cp < 0) return NULL;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                int lo = hex4(ps->p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00), ps->p += 6;
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;  // a lone surrogate
            o = put_utf8(o, (unsigned)cp);
            break;
        }
        default: return NULL;
        }
    }
    if (ps->p >= ps->end) return NULL;
    *o = 0;  // at or before the closing quote
    ps->p++;
    return start;
}

static int value(parser *ps);

// Parses the members or elements of the container at index `self`, linking them as children.
static bool children(parser *ps, int self, bool object) {
    char close = object ? '}' : ']';
    ps->p++;
    skip_space(ps);
    if (ps->p < ps->end && *ps->p == close) { ps->p++; return true; }
    int prev = -1;
    for (;;) {
        const char *key = NULL;
        skip_space(ps);
        if (object) {
            if (ps->p >= ps->end || *ps->p != '"' || !(key = string(ps))) return false;
            skip_space(ps);
            if (ps->p >= ps->end || *ps->p++ != ':') return false;
        }
        int child = value(ps);
        if (child < 0) return false;
        ps->nodes[child].key = key;
        if (prev < 0) ps->nodes[self].first = child - self;
        else ps->nodes[prev].next = child - prev;
        prev = child;
        ps->nodes[self].count++;
        skip_space(ps);
        if (ps->p >= ps->end) return false;
        char c = *ps->p++;
        if (c == close) return true;
        if (c != ',') return false;
    }
}

static int value(parser *ps) {
    skip_space(ps);
    if (ps->p >= ps->end) return -1;
    char c = *ps->p;
    int self;
    if (c == '{' || c == '[') {
        if (++ps->depth > MAX_DEPTH || (self = new_node(ps, c == '{' ? GS_JSON_OBJECT : GS_JSON_ARRAY)) < 0) return -1;
        if (!children(ps, self, c == '{')) return -1;
        ps->depth--;
    } else if (c == '"') {
        if ((self = new_node(ps, GS_JSON_STRING)) < 0) return -1;
        const char *s = string(ps);
        if (!s) return -1;
        ps->nodes[self].str = s;
    } else if (c == 't' && ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) {
        if ((self = new_node(ps, GS_JSON_BOOL)) < 0) return -1;
        ps->nodes[self].num = 1, ps->p += 4;
    } else if (c == 'f' && ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) {
        if ((self = new_node(ps, GS_JSON_BOOL)) < 0) return -1;
        ps->p += 5;
    } else if (c == 'n' && ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) {
        if ((self = new_node(ps, GS_JSON_NULL)) < 0) return -1;
        ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *num_end;
        double d = strtod(ps->p, &num_end);  // the copy is zero-terminated, so strtod stops in it
        if (num_end == ps->p || num_end > ps->end || (self = new_node(ps, GS_JSON_NUMBER)) < 0) return -1;
        ps->nodes[self].num = d, ps->p = num_end;
    } else {
        return -1;
    }
    return self;
}

gs_json *gs_json_parse(const char *text, size_t len) {
    parser ps = { 0 };
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len);
    copy[len] = 0;
    ps.p = copy, ps.end = copy + len;
    bool ok = value(&ps) == 0;
    skip_space(&ps);
    if (!ok || ps.p != ps.end) {
        free(ps.nodes), free(copy);
        return NULL;
    }
    gs_json *doc = malloc(sizeof *doc);
    if (!doc) {
        free(ps.nodes), free(copy);
        return NULL;
    }
    doc->nodes = ps.nodes, doc->text = copy;
    return doc;
}

void gs_json_free(gs_json *doc) {
    if (!doc) return;
    free(doc->nodes), free(doc->text), free(doc);
}

gs_jv gs_json_root(const gs_json *doc) { return doc ? doc->nodes : NULL; }
gs_json_type gs_json_kind(gs_jv v) { return v ? (gs_json_type)v->kind : GS_JSON_NULL; }
int gs_json_count(gs_jv v) { return v ? v->count : 0; }
gs_jv gs_json_first(gs_jv v) { return v && v->first ? v + v->first : NULL; }
gs_jv gs_json_next(gs_jv v) { return v && v->next ? v + v->next : NULL; }
const char *gs_json_key(gs_jv v) { return v ? v->key : NULL; }

gs_jv gs_json_get(gs_jv object, const char *key) {
    if (gs_json_kind(object) != GS_JSON_OBJECT) return NULL;
    for (gs_jv c = gs_json_first(object); c; c = gs_json_next(c))
        if (!strcmp(c->key, key)) return c;
    return NULL;
}

gs_jv gs_json_at(gs_jv array, int index) {
    if (gs_json_kind(array) != GS_JSON_ARRAY || index < 0) return NULL;
    gs_jv c = gs_json_first(array);
    while (c && index--) c = gs_json_next(c);
    return c;
}

gs_jv gs_json_path(gs_jv v, const char *path) {
    char part[128];
    while (v && *path) {
        size_t n = strcspn(path, ".");
        if (n >= sizeof part) return NULL;
        memcpy(part, path, n);
        part[n] = 0;
        path += n + (path[n] == '.');
        if (gs_json_kind(v) == GS_JSON_ARRAY) {
            char *end;
            long i = strtol(part, &end, 10);
            v = *end || end == part ? NULL : gs_json_at(v, (int)i);
        } else {
            v = gs_json_get(v, part);
        }
    }
    return v;
}

const char *gs_json_str(gs_jv v, const char *fallback) { return gs_json_kind(v) == GS_JSON_STRING ? v->str : fallback; }
double gs_json_num(gs_jv v, double fallback) { return gs_json_kind(v) == GS_JSON_NUMBER ? v->num : fallback; }
bool gs_json_bool(gs_jv v, bool fallback) { return gs_json_kind(v) == GS_JSON_BOOL ? v->num != 0 : fallback; }

// ---- Values that can be changed and written ----

struct gs_jval {
    gs_json_type kind;
    bool boolean;
    double number;
    char *string;
    char **keys;      // objects: one per item
    gs_jval **items;  // arrays and objects
    int count, cap;
};

static char *dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, s, n);
    return copy;
}

static gs_jval *make(gs_json_type kind) {
    gs_jval *v = calloc(1, sizeof *v);
    if (v) v->kind = kind;
    return v;
}

gs_jval *gs_jval_null(void) { return make(GS_JSON_NULL); }
gs_jval *gs_jval_array(void) { return make(GS_JSON_ARRAY); }
gs_jval *gs_jval_object(void) { return make(GS_JSON_OBJECT); }

gs_jval *gs_jval_bool(bool b) {
    gs_jval *v = make(GS_JSON_BOOL);
    v->boolean = b;
    return v;
}

gs_jval *gs_jval_num(double n) {
    gs_jval *v = make(GS_JSON_NUMBER);
    v->number = n;
    return v;
}

gs_jval *gs_jval_str(const char *s) {
    if (!s) return gs_jval_null();
    gs_jval *v = make(GS_JSON_STRING);
    v->string = dup(s);
    return v;
}

void gs_jval_free(gs_jval *v) {
    if (!v) return;
    for (int i = 0; i < v->count; i++) {
        gs_jval_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->string);
    free(v);
}

static void grow(gs_jval *v) {
    if (v->count < v->cap) return;
    v->cap = v->cap * 2 + 4;
    v->items = realloc(v->items, sizeof(gs_jval *) * (size_t)v->cap);
    if (v->kind == GS_JSON_OBJECT) v->keys = realloc(v->keys, sizeof(char *) * (size_t)v->cap);
}

void gs_jval_push(gs_jval *array, gs_jval *value) {
    grow(array);
    array->items[array->count++] = value;
}

static int find(const gs_jval *object, const char *key) {
    if (!object || object->kind != GS_JSON_OBJECT) return -1;
    for (int i = 0; i < object->count; i++)
        if (!strcmp(object->keys[i], key)) return i;
    return -1;
}

void gs_jval_set(gs_jval *object, const char *key, gs_jval *value) {
    int i = find(object, key);
    if (i >= 0) {
        gs_jval_free(object->items[i]);
        object->items[i] = value;
        return;
    }
    grow(object);
    object->keys[object->count] = dup(key);
    object->items[object->count++] = value;
}

void gs_jval_remove(gs_jval *object, const char *key) {
    int i = find(object, key);
    if (i < 0) return;
    gs_jval_free(object->items[i]);
    free(object->keys[i]);
    for (int k = i; k < object->count - 1; k++) {
        object->items[k] = object->items[k + 1];
        object->keys[k] = object->keys[k + 1];
    }
    object->count--;
}

gs_jval *gs_jval_get(const gs_jval *object, const char *key) {
    int i = find(object, key);
    return i < 0 ? NULL : object->items[i];
}

gs_jval *gs_jval_at(const gs_jval *array, int index) {
    return array && (array->kind == GS_JSON_ARRAY || array->kind == GS_JSON_OBJECT) && index >= 0 && index < array->count ? array->items[index] : NULL;
}

int gs_jval_count(const gs_jval *v) { return v && (v->kind == GS_JSON_ARRAY || v->kind == GS_JSON_OBJECT) ? v->count : 0; }
const char *gs_jval_string(const gs_jval *v, const char *fallback) { return v && v->kind == GS_JSON_STRING ? v->string : fallback; }
double gs_jval_number(const gs_jval *v, double fallback) { return v && v->kind == GS_JSON_NUMBER ? v->number : fallback; }
bool gs_jval_boolean(const gs_jval *v, bool fallback) { return v && v->kind == GS_JSON_BOOL ? v->boolean : fallback; }

gs_jval *gs_jval_path(const gs_jval *v, const char *path) {
    char key[128];
    while (v && *path) {
        const char *dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        if (n >= sizeof key) return NULL;
        memcpy(key, path, n);
        key[n] = 0;
        v = gs_jval_get(v, key);
        path += n + (dot ? 1 : 0);
    }
    return (gs_jval *)v;
}

void gs_jval_set_path(gs_jval *object, const char *path, gs_jval *value) {
    char key[128];
    for (;;) {
        const char *dot = strchr(path, '.');
        size_t n = dot ? (size_t)(dot - path) : strlen(path);
        if (n >= sizeof key) { gs_jval_free(value); return; }
        memcpy(key, path, n);
        key[n] = 0;
        if (!dot) { gs_jval_set(object, key, value); return; }
        gs_jval *next = gs_jval_get(object, key);
        if (!next || next->kind != GS_JSON_OBJECT) {
            next = gs_jval_object();
            gs_jval_set(object, key, next);
        }
        object = next;
        path = dot + 1;
    }
}

gs_jval *gs_jval_clone(const gs_jval *v) {
    if (!v) return gs_jval_null();
    gs_jval *out = make(v->kind);
    out->boolean = v->boolean;
    out->number = v->number;
    if (v->string) out->string = dup(v->string);
    for (int i = 0; i < v->count; i++) {
        if (v->kind == GS_JSON_OBJECT) gs_jval_set(out, v->keys[i], gs_jval_clone(v->items[i]));
        else gs_jval_push(out, gs_jval_clone(v->items[i]));
    }
    return out;
}

bool gs_jval_equal(const gs_jval *a, const gs_jval *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind || a->count != b->count) return false;
    switch (a->kind) {
    case GS_JSON_BOOL: return a->boolean == b->boolean;
    case GS_JSON_NUMBER: return a->number == b->number;
    case GS_JSON_STRING: return !strcmp(a->string, b->string);
    case GS_JSON_ARRAY:
        for (int i = 0; i < a->count; i++)
            if (!gs_jval_equal(a->items[i], b->items[i])) return false;
        return true;
    case GS_JSON_OBJECT:
        for (int i = 0; i < a->count; i++)
            if (!gs_jval_equal(a->items[i], gs_jval_get(b, a->keys[i]))) return false;
        return true;
    default: return true;
    }
}

gs_jval *gs_jval_merge(const gs_jval *base, const gs_jval *override) {
    if (!override || override->kind == GS_JSON_NULL) return gs_jval_clone(base);
    if (!base) return gs_jval_clone(override);
    if (base->kind == GS_JSON_ARRAY) return gs_jval_clone(override->kind == GS_JSON_ARRAY ? override : base);
    if (base->kind != GS_JSON_OBJECT) return gs_jval_clone(override->kind == base->kind ? override : base);
    if (override->kind != GS_JSON_OBJECT) return gs_jval_clone(base);
    gs_jval *out = gs_jval_clone(base);
    for (int i = 0; i < override->count; i++) {
        const gs_jval *mine = gs_jval_get(base, override->keys[i]);
        gs_jval_set(out, override->keys[i], mine ? gs_jval_merge(mine, override->items[i]) : gs_jval_clone(override->items[i]));
    }
    return out;
}

gs_jval *gs_jval_from(gs_jv v) {
    switch (gs_json_kind(v)) {
    case GS_JSON_BOOL: return gs_jval_bool(gs_json_bool(v, false));
    case GS_JSON_NUMBER: return gs_jval_num(gs_json_num(v, 0));
    case GS_JSON_STRING: return gs_jval_str(gs_json_str(v, ""));
    case GS_JSON_ARRAY: {
        gs_jval *a = gs_jval_array();
        for (gs_jv c = gs_json_first(v); c; c = gs_json_next(c)) gs_jval_push(a, gs_jval_from(c));
        return a;
    }
    case GS_JSON_OBJECT: {
        gs_jval *o = gs_jval_object();
        for (gs_jv c = gs_json_first(v); c; c = gs_json_next(c)) gs_jval_set(o, gs_json_key(c), gs_jval_from(c));
        return o;
    }
    default: return gs_jval_null();
    }
}

gs_jval *gs_jval_parse(const char *text, size_t len) {
    gs_json *doc = gs_json_parse(text, len);
    if (!doc) return NULL;
    gs_jval *v = gs_jval_from(gs_json_root(doc));
    gs_json_free(doc);
    return v;
}

typedef struct { char *s; size_t n, cap; } buf;

static void put(buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) b->s = realloc(b->s, b->cap = (b->cap + n) * 2 + 256);
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void put_string(buf *b, const char *s) {
    put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        const char *named = *p == '"' ? "\\\"" : *p == '\\' ? "\\\\" : *p == '\n' ? "\\n" : *p == '\r' ? "\\r" : *p == '\t' ? "\\t" : *p == '\b' ? "\\b" : *p == '\f' ? "\\f" : NULL;
        if (named) put(b, named, 2);
        else if (*p < 0x20) put(b, esc, (size_t)snprintf(esc, sizeof esc, "\\u%04x", *p));
        else put(b, (const char *)p, 1);
    }
    put(b, "\"", 1);
}

// The shortest decimal that reads back as the same number, as JavaScript prints it.
static void put_number(buf *b, double n) {
    char text[40];
    if (!isfinite(n)) { put(b, "null", 4); return; }
    if (n == floor(n) && fabs(n) < 1e15) { put(b, text, (size_t)snprintf(text, sizeof text, "%.0f", n)); return; }
    for (int digits = 1; digits <= 17; digits++) {
        snprintf(text, sizeof text, "%.*g", digits, n);
        if (strtod(text, NULL) == n) break;
    }
    put(b, text, strlen(text));
}

static void newline(buf *b, bool pretty, int depth) {
    if (!pretty) return;
    put(b, "\n", 1);
    for (int i = 0; i < depth; i++) put(b, "  ", 2);
}

static void write_value(buf *b, const gs_jval *v, bool pretty, int depth) {
    switch (v ? v->kind : GS_JSON_NULL) {
    case GS_JSON_NULL: put(b, "null", 4); break;
    case GS_JSON_BOOL: put(b, v->boolean ? "true" : "false", v->boolean ? 4 : 5); break;
    case GS_JSON_NUMBER: put_number(b, v->number); break;
    case GS_JSON_STRING: put_string(b, v->string); break;
    default: {
        bool object = v->kind == GS_JSON_OBJECT;
        put(b, object ? "{" : "[", 1);
        for (int i = 0; i < v->count; i++) {
            if (i) put(b, ",", 1);
            newline(b, pretty, depth + 1);
            if (object) {
                put_string(b, v->keys[i]);
                put(b, pretty ? ": " : ":", pretty ? 2 : 1);
            }
            write_value(b, v->items[i], pretty, depth + 1);
        }
        if (v->count) newline(b, pretty, depth);
        put(b, object ? "}" : "]", 1);
    }
    }
}

char *gs_jval_write(const gs_jval *v, bool pretty) {
    buf b = { 0 };
    put(&b, "", 0);
    write_value(&b, v, pretty, 0);
    return b.s;
}

gs_json_type gs_jval_kind(const gs_jval *v) { return v ? v->kind : GS_JSON_NULL; }
const char *gs_jval_key(const gs_jval *object, int index) { return object && object->kind == GS_JSON_OBJECT && index >= 0 && index < object->count ? object->keys[index] : NULL; }

gs_jval *gs_jval_load(const char *path) {
    size_t len;
    char *text = SDL_LoadFile(path, &len);
    gs_jval *v = text ? gs_jval_parse(text, len) : NULL;
    SDL_free(text);
    return v;
}

bool gs_jval_save(const char *path, const gs_jval *v, bool pretty) {
    char *text = gs_jval_write(v, pretty);
    bool ok = text && gs_file_save(path, text, strlen(text));
    free(text);
    return ok;
}

bool gs_file_save(const char *path, const void *data, size_t len) {
    size_t n = strlen(path);
    char *folder = dup(path), *tmp = malloc(n + 40);
    if (!folder || !tmp) return free(folder), free(tmp), false;
    char *slash = strrchr(folder, '/');
#ifdef _WIN32
    char *back = strrchr(folder, '\\');
    if (back > slash) slash = back;
#endif
    if (slash && slash > folder) *slash = 0, SDL_CreateDirectory(folder);
    // Written beside it under a name of this thread's, then moved over it: a reader sees the old file
    // or the new one whole, and a crash part way leaves the old one.
    snprintf(tmp, n + 40, "%s.%llu.tmp", path, (unsigned long long)SDL_GetCurrentThreadID());
    SDL_RemovePath(tmp);
    SDL_IOStream *io = SDL_IOFromFile(tmp, "wb");
    bool ok = io != NULL;
#ifndef _WIN32
    if (ok) chmod(tmp, 0600);  // the user's own, before any content is in it
#endif
    ok = ok && SDL_WriteIO(io, data, len) == len && SDL_FlushIO(io);
    if (io) ok = SDL_CloseIO(io) && ok;
    ok = ok && SDL_RenamePath(tmp, path);
    if (!ok) SDL_RemovePath(tmp);
    free(folder), free(tmp);
    return ok;
}
