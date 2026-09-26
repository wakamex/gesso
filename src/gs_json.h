// A small JSON reader: parses a whole document into a read-only tree in two allocations, with
// strings decoded to UTF-8. Lookups on a missing value return NULL and every accessor accepts NULL,
// so deep paths need no checks at each step:
//     gs_json_str(gs_json_path(root, "contents.0.title.runs.0.text"), "")
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { GS_JSON_NULL, GS_JSON_BOOL, GS_JSON_NUMBER, GS_JSON_STRING, GS_JSON_ARRAY, GS_JSON_OBJECT } gs_json_type;

typedef struct gs_json gs_json;          // a parsed document
typedef const struct gs_json_node *gs_jv;  // a value inside it, or NULL

gs_json *gs_json_parse(const char *text, size_t len);  // NULL if it is not valid JSON
void gs_json_free(gs_json *doc);
gs_jv gs_json_root(const gs_json *doc);

gs_json_type gs_json_kind(gs_jv v);  // GS_JSON_NULL for NULL
gs_jv gs_json_get(gs_jv object, const char *key);
gs_jv gs_json_at(gs_jv array, int index);
gs_jv gs_json_path(gs_jv v, const char *path);  // keys and array indexes joined by dots
int gs_json_count(gs_jv v);                     // members or elements

// Iterating an object's members or an array's elements: for (gs_jv c = gs_json_first(v); c; c = gs_json_next(c))
gs_jv gs_json_first(gs_jv v);
gs_jv gs_json_next(gs_jv v);
const char *gs_json_key(gs_jv member);  // the member's key, or NULL for array elements

const char *gs_json_str(gs_jv v, const char *fallback);  // fallback unless v is a string
double gs_json_num(gs_jv v, double fallback);
bool gs_json_bool(gs_jv v, bool fallback);
