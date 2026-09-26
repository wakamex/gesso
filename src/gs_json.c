#include "gs_json.h"

#include <stdlib.h>
#include <string.h>

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
