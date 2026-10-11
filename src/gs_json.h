// JSON read and written. The reader parses a whole document into a read-only tree in two
// allocations, with strings decoded to UTF-8; gs_jval, further down, is a value that can be built,
// changed, merged and written, for the files an app keeps (settings, history). Lookups on a missing value return NULL and every accessor accepts NULL,
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

// ---- Values that can be changed and written ----
//
// A gs_jval is a JSON value an app owns: built from nothing or from a parsed document, changed,
// and written out as text or to a file. Objects keep their keys in the order they were added.
// Every reading call takes NULL and a value of the wrong kind and answers with the fallback.
typedef struct gs_jval gs_jval;

gs_jval *gs_jval_null(void);
gs_jval *gs_jval_bool(bool b);
gs_jval *gs_jval_num(double n);
gs_jval *gs_jval_str(const char *s);  // NULL gives null
gs_jval *gs_jval_array(void);
gs_jval *gs_jval_object(void);
void gs_jval_free(gs_jval *v);
gs_jval *gs_jval_clone(const gs_jval *v);
gs_jval *gs_jval_from(gs_jv v);                       // a copy of a parsed value
gs_jval *gs_jval_parse(const char *text, size_t len);  // NULL when it is not valid JSON

gs_json_type gs_jval_kind(const gs_jval *v);  // GS_JSON_NULL for NULL
gs_jval *gs_jval_get(const gs_jval *object, const char *key);
gs_jval *gs_jval_path(const gs_jval *v, const char *path);  // keys joined by dots
gs_jval *gs_jval_at(const gs_jval *array, int index);       // an array's element, or an object's value by position
const char *gs_jval_key(const gs_jval *object, int index);  // the key at a position, for going through an object
int gs_jval_count(const gs_jval *v);
const char *gs_jval_string(const gs_jval *v, const char *fallback);
double gs_jval_number(const gs_jval *v, double fallback);
bool gs_jval_boolean(const gs_jval *v, bool fallback);

// Changing. Each takes ownership of `value`.
void gs_jval_set(gs_jval *object, const char *key, gs_jval *value);  // replaces the key's value, or adds the key
void gs_jval_remove(gs_jval *object, const char *key);
void gs_jval_push(gs_jval *array, gs_jval *value);
void gs_jval_set_path(gs_jval *object, const char *path, gs_jval *value);  // at a dotted path, making the objects on the way

// `override` laid over `base`, as saved settings are laid over their defaults: a value of the wrong
// kind for its place gives way to the default, an array replaces an array whole, and keys the
// defaults do not have are kept. Returns a new value.
gs_jval *gs_jval_merge(const gs_jval *base, const gs_jval *override);
bool gs_jval_equal(const gs_jval *a, const gs_jval *b);  // objects whatever their keys' order

// As text: indented by two spaces as JSON.stringify(value, null, 2) writes it when `pretty`, compact
// otherwise. Numbers are written with the fewest digits that read back the same. free() the result.
char *gs_jval_write(const gs_jval *v, bool pretty);
// To and from a file. Saving replaces the file whole (gs_file_save); loading gives NULL for a
// missing file or one that is not JSON.
bool gs_jval_save(const char *path, const gs_jval *v, bool pretty);
gs_jval *gs_jval_load(const char *path);

// Writes a file so that it is never seen half written: the data goes to a temporary file beside it,
// readable by the user alone, which then takes its place. Makes the file's folder if it is missing.
// False with SDL_GetError set when it could not.
bool gs_file_save(const char *path, const void *data, size_t len);
