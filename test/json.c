#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *alloc_or_exit(size_t size) {
  void *memory = malloc(size);
  if (!memory) {
    fprintf(stderr, "json: out of memory\n");
    exit(1);
  }
  return memory;
}

static void *realloc_or_exit(void *memory, size_t size) {
  void *grown = realloc(memory, size);
  if (!grown) {
    fprintf(stderr, "json: out of memory\n");
    exit(1);
  }
  return grown;
}

typedef struct {
  const char *begin, *cursor, *end;
  char *error;
  size_t error_size;
  int ok;
} parser_t;

static void fail(parser_t *parser, const char *message) {
  if (parser->ok) {
    parser->ok = 0;
    if (parser->error && parser->error_size) {
      snprintf(parser->error, parser->error_size, "%s at offset %ld", message,
               (long)(parser->cursor - parser->begin));
    }
  }
}

static void skip_whitespace(parser_t *parser) {
  while (parser->cursor < parser->end) {
    char c = *parser->cursor;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      parser->cursor++;
    } else {
      break;
    }
  }
}

static int match(parser_t *parser, const char *literal) {
  size_t length = strlen(literal);
  if ((size_t)(parser->end - parser->cursor) >= length &&
      memcmp(parser->cursor, literal, length) == 0) {
    parser->cursor += length;
    return 1;
  }
  return 0;
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* Assumes *parser->cursor == '"'. Returns a malloc'd decoded string, or NULL on error. */
static char *parse_string_raw(parser_t *parser) {
  parser->cursor++;
  /* find the closing quote first and size the buffer by the raw span; a
     rest-of-the-file-sized allocation per string dominates the whole run */
  const char *scan = parser->cursor;
  while (scan < parser->end && *scan != '"') {
    if (*scan == '\\' && scan + 1 < parser->end) {
      scan++;
    }
    scan++;
  }
  char *buffer = alloc_or_exit((size_t)(scan - parser->cursor) + 1);
  size_t length = 0;
  /* in bounds: the loop below emits at most one byte per input byte consumed
     (escapes consume two or six per byte emitted), so length never exceeds the
     raw span the buffer was sized by; the analyzer cannot follow the invariant
     across the two loops */
  /* NOLINTBEGIN(clang-analyzer-security.ArrayBound) */
  while (parser->cursor < parser->end) {
    char c = *parser->cursor;
    if (c == '"') {
      parser->cursor++;
      buffer[length] = '\0';
      return buffer;
    }
    if ((unsigned char)c < 0x20) {
      fail(parser, "control character in string");
      break;
    }
    if (c == '\\') {
      parser->cursor++;
      if (parser->cursor >= parser->end) {
        fail(parser, "unterminated escape");
        break;
      }
      char e = *parser->cursor++;
      switch (e) {
        case '"':
          buffer[length++] = '"';
          break;
        case '\\':
          buffer[length++] = '\\';
          break;
        case '/':
          buffer[length++] = '/';
          break;
        case 'b':
          buffer[length++] = '\b';
          break;
        case 'f':
          buffer[length++] = '\f';
          break;
        case 'n':
          buffer[length++] = '\n';
          break;
        case 'r':
          buffer[length++] = '\r';
          break;
        case 't':
          buffer[length++] = '\t';
          break;
        case 'u': {
          int code = 0;
          for (int k = 0; k < 4; k++) {
            int d = parser->cursor < parser->end ? hex_digit(*parser->cursor) : -1;
            if (d < 0) {
              fail(parser, "bad \\u escape");
              free(buffer);
              return NULL;
            }
            code = code * 16 + d;
            parser->cursor++;
          }
          buffer[length++] = (char)(code < 0x80 ? code : '?');
          break;
        }
        default:
          fail(parser, "bad escape");
          free(buffer);
          return NULL;
      }
    } else {
      buffer[length++] = c;
      parser->cursor++;
    }
  }
  /* NOLINTEND(clang-analyzer-security.ArrayBound) */
  if (parser->ok) {
    fail(parser, "unterminated string");
  }
  free(buffer);
  return NULL;
}

static int parse_value(parser_t *parser, json_value *out);

static int parse_array(parser_t *parser, json_value *out) {
  parser->cursor++; /* '[' */
  out->type = JSON_ARRAY;
  skip_whitespace(parser);
  if (parser->cursor < parser->end && *parser->cursor == ']') {
    parser->cursor++;
    return 1;
  }
  size_t capacity = 0;
  for (;;) {
    if (out->length == capacity) {
      capacity = capacity ? capacity * 2 : 8;
      out->items = realloc_or_exit(out->items, capacity * sizeof *out->items);
    }
    if (!parse_value(parser, &out->items[out->length])) {
      return 0;
    }
    out->length++;
    skip_whitespace(parser);
    if (parser->cursor >= parser->end) {
      fail(parser, "unterminated array");
      return 0;
    }
    if (*parser->cursor == ',') {
      parser->cursor++;
    } else if (*parser->cursor == ']') {
      parser->cursor++;
      return 1;
    } else {
      fail(parser, "expected ',' or ']'");
      return 0;
    }
  }
}

static int parse_object(parser_t *parser, json_value *out) {
  parser->cursor++; /* '{' */
  out->type = JSON_OBJECT;
  skip_whitespace(parser);
  if (parser->cursor < parser->end && *parser->cursor == '}') {
    parser->cursor++;
    return 1;
  }
  size_t capacity = 0;
  for (;;) {
    skip_whitespace(parser);
    if (parser->cursor >= parser->end || *parser->cursor != '"') {
      fail(parser, "expected object key");
      return 0;
    }
    char *key = parse_string_raw(parser);
    if (!key) {
      return 0;
    }
    skip_whitespace(parser);
    if (parser->cursor >= parser->end || *parser->cursor != ':') {
      free(key);
      fail(parser, "expected ':'");
      return 0;
    }
    parser->cursor++;
    if (out->length == capacity) {
      capacity = capacity ? capacity * 2 : 8;
      out->items = realloc_or_exit(out->items, capacity * sizeof *out->items);
      out->keys = (char **)realloc_or_exit((void *)out->keys, capacity * sizeof *out->keys);
    }
    out->keys[out->length] = key;
    if (!parse_value(parser, &out->items[out->length])) {
      /* the key is owned by out once stored; count it so free works */
      out->length++;
      return 0;
    }
    out->length++;
    skip_whitespace(parser);
    if (parser->cursor >= parser->end) {
      fail(parser, "unterminated object");
      return 0;
    }
    if (*parser->cursor == ',') {
      parser->cursor++;
    } else if (*parser->cursor == '}') {
      parser->cursor++;
      return 1;
    } else {
      fail(parser, "expected ',' or '}'");
      return 0;
    }
  }
}

static int parse_value(parser_t *parser, json_value *out) {
  skip_whitespace(parser);
  memset(out, 0, sizeof *out);
  if (parser->cursor >= parser->end) {
    fail(parser, "unexpected end of input");
    return 0;
  }
  char c = *parser->cursor;
  if (c == 'n') {
    if (!match(parser, "null")) {
      fail(parser, "bad literal");
      return 0;
    }
    out->type = JSON_NULL;
    return 1;
  }
  if (c == 't' || c == 'f') {
    if (match(parser, "true")) {
      out->type = JSON_BOOL;
      out->number = 1;
      return 1;
    }
    if (match(parser, "false")) {
      out->type = JSON_BOOL;
      out->number = 0;
      return 1;
    }
    fail(parser, "bad literal");
    return 0;
  }
  if (c == '"') {
    out->type = JSON_STRING;
    out->string = parse_string_raw(parser);
    return out->string != NULL;
  }
  if (c == '[') {
    return parse_array(parser, out);
  }
  if (c == '{') {
    return parse_object(parser, out);
  }
  /* the input buffer is NUL-terminated by json_parse_file, so strtod cannot overrun */
  char *number_end;
  double d = strtod(parser->cursor, &number_end);
  if (number_end == parser->cursor) {
    fail(parser, "unexpected character");
    return 0;
  }
  out->type = JSON_NUMBER;
  out->number = d;
  parser->cursor = number_end;
  return 1;
}

static void free_contents(json_value *value) {
  switch (value->type) {
    case JSON_STRING:
      free(value->string);
      break;
    case JSON_ARRAY:
      for (size_t k = 0; k < value->length; k++) {
        free_contents(&value->items[k]);
      }
      free(value->items);
      break;
    case JSON_OBJECT:
      for (size_t k = 0; k < value->length; k++) {
        free(value->keys[k]);
        free_contents(&value->items[k]);
      }
      free((void *)value->keys);
      free(value->items);
      break;
    default:
      break;
  }
}

void json_free(json_value *root) {
  if (root) {
    free_contents(root);
    free(root);
  }
}

const json_value *json_get(const json_value *object, const char *key) {
  if (!object || object->type != JSON_OBJECT) {
    return NULL;
  }
  for (size_t k = 0; k < object->length; k++) {
    if (strcmp(object->keys[k], key) == 0) {
      return &object->items[k];
    }
  }
  return NULL;
}

json_value *json_parse_file(const char *path, char *error, size_t error_size) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    if (error && error_size) {
      snprintf(error, error_size, "cannot open %s", path);
    }
    return NULL;
  }
  long size = -1;
  if (fseek(file, 0, SEEK_END) == 0) {
    size = ftell(file);
  }
  /* the largest real test file is ~2 MB; anything huge is a wrong path, not data */
  if (size < 0 || size > 256L * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    if (error && error_size) {
      snprintf(error, error_size, "cannot size %s", path);
    }
    return NULL;
  }
  char *buffer = alloc_or_exit((size_t)size + 1);
  size_t bytes_read = fread(buffer, 1, (size_t)size, file);
  fclose(file);
  if (bytes_read != (size_t)size) {
    free(buffer);
    if (error && error_size) {
      snprintf(error, error_size, "cannot read %s", path);
    }
    return NULL;
  }
  /* in bounds: buffer holds size+1 bytes; the analyzer distrusts any ftell-derived
     index regardless of guards */
  buffer[size] = '\0'; /* NOLINT(clang-analyzer-security.ArrayBound) */
  parser_t parser = {buffer, buffer, buffer + bytes_read, error, error_size, 1};
  json_value *root = alloc_or_exit(sizeof *root);
  int ok = parse_value(&parser, root);
  if (ok) {
    skip_whitespace(&parser);
    if (parser.cursor != parser.end) {
      fail(&parser, "trailing data");
      ok = 0;
    }
  }
  free(buffer);
  if (!ok) {
    json_free(root);
    return NULL;
  }
  return root;
}
