/*
 * json.h — minimal JSON reader for the test harness.
 *
 * Test-only code: parses a whole file into a malloc'd DOM. The emulation core
 * never uses this. Grammar per https://www.json.org; \uXXXX escapes below
 * U+0080 are decoded, anything above becomes '?' (test names are ASCII).
 */
#ifndef COLOPHON_JSON_H
#define COLOPHON_JSON_H

#include <stddef.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

typedef struct json_value {
  json_type type;
  double number;            /* JSON_NUMBER; JSON_BOOL uses 0/1 */
  char *string;             /* JSON_STRING */
  struct json_value *items; /* JSON_ARRAY and JSON_OBJECT values, items[0..length) */
  char **keys;              /* JSON_OBJECT keys, keys[0..length) */
  size_t length;
} json_value;

/* Returns the parsed root, or NULL with a message in error. Free with json_free. */
json_value *json_parse_file(const char *path, char *error, size_t error_size);
void json_free(json_value *root);

/* Object member lookup; NULL if absent or not an object. */
const json_value *json_get(const json_value *object, const char *key);

#endif
