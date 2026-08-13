#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Out-of-memory in the test harness is fatal by design. */
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

static void fail(parser_t *p, const char *message) {
  if (p->ok) {
    p->ok = 0;
    if (p->error && p->error_size) {
      snprintf(p->error, p->error_size, "%s at offset %ld", message, (long)(p->cursor - p->begin));
    }
  }
}

static void skip_whitespace(parser_t *p) {
  while (p->cursor < p->end) {
    char c = *p->cursor;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      p->cursor++;
    } else {
      break;
    }
  }
}

static int match(parser_t *p, const char *literal) {
  size_t n = strlen(literal);
  if ((size_t)(p->end - p->cursor) >= n && memcmp(p->cursor, literal, n) == 0) {
    p->cursor += n;
    return 1;
  }
  return 0;
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* Assumes *p->cursor == '"'. Returns a malloc'd decoded string, or NULL on error. */
static char *parse_string_raw(parser_t *p) {
  p->cursor++;
  char *buffer = alloc_or_exit((size_t)(p->end - p->cursor) + 1);
  size_t length = 0;
  while (p->cursor < p->end) {
    char c = *p->cursor;
    if (c == '"') {
      p->cursor++;
      buffer[length] = '\0';
      return buffer;
    }
    if ((unsigned char)c < 0x20) {
      fail(p, "control character in string");
      break;
    }
    if (c == '\\') {
      p->cursor++;
      if (p->cursor >= p->end) {
        fail(p, "unterminated escape");
        break;
      }
      char e = *p->cursor++;
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
            int d = p->cursor < p->end ? hex_digit(*p->cursor) : -1;
            if (d < 0) {
              fail(p, "bad \\u escape");
              free(buffer);
              return NULL;
            }
            code = code * 16 + d;
            p->cursor++;
          }
          buffer[length++] = (char)(code < 0x80 ? code : '?');
          break;
        }
        default:
          fail(p, "bad escape");
          free(buffer);
          return NULL;
      }
    } else {
      buffer[length++] = c;
      p->cursor++;
    }
  }
  if (p->ok) {
    fail(p, "unterminated string");
  }
  free(buffer);
  return NULL;
}

static int parse_value(parser_t *p, json_value *out);

static int parse_array(parser_t *p, json_value *out) {
  p->cursor++; /* '[' */
  out->type = JSON_ARRAY;
  skip_whitespace(p);
  if (p->cursor < p->end && *p->cursor == ']') {
    p->cursor++;
    return 1;
  }
  size_t cap = 0;
  for (;;) {
    if (out->length == cap) {
      cap = cap ? cap * 2 : 8;
      out->items = realloc_or_exit(out->items, cap * sizeof *out->items);
    }
    if (!parse_value(p, &out->items[out->length])) {
      return 0;
    }
    out->length++;
    skip_whitespace(p);
    if (p->cursor >= p->end) {
      fail(p, "unterminated array");
      return 0;
    }
    if (*p->cursor == ',') {
      p->cursor++;
    } else if (*p->cursor == ']') {
      p->cursor++;
      return 1;
    } else {
      fail(p, "expected ',' or ']'");
      return 0;
    }
  }
}

static int parse_object(parser_t *p, json_value *out) {
  p->cursor++; /* '{' */
  out->type = JSON_OBJECT;
  skip_whitespace(p);
  if (p->cursor < p->end && *p->cursor == '}') {
    p->cursor++;
    return 1;
  }
  size_t cap = 0;
  for (;;) {
    skip_whitespace(p);
    if (p->cursor >= p->end || *p->cursor != '"') {
      fail(p, "expected object key");
      return 0;
    }
    char *key = parse_string_raw(p);
    if (!key) {
      return 0;
    }
    skip_whitespace(p);
    if (p->cursor >= p->end || *p->cursor != ':') {
      free(key);
      fail(p, "expected ':'");
      return 0;
    }
    p->cursor++;
    if (out->length == cap) {
      cap = cap ? cap * 2 : 8;
      out->items = realloc_or_exit(out->items, cap * sizeof *out->items);
      out->keys = (char **)realloc_or_exit((void *)out->keys, cap * sizeof *out->keys);
    }
    out->keys[out->length] = key;
    if (!parse_value(p, &out->items[out->length])) {
      /* the key is owned by out once stored; count it so free works */
      out->length++;
      return 0;
    }
    out->length++;
    skip_whitespace(p);
    if (p->cursor >= p->end) {
      fail(p, "unterminated object");
      return 0;
    }
    if (*p->cursor == ',') {
      p->cursor++;
    } else if (*p->cursor == '}') {
      p->cursor++;
      return 1;
    } else {
      fail(p, "expected ',' or '}'");
      return 0;
    }
  }
}

static int parse_value(parser_t *p, json_value *out) {
  skip_whitespace(p);
  memset(out, 0, sizeof *out);
  if (p->cursor >= p->end) {
    fail(p, "unexpected end of input");
    return 0;
  }
  char c = *p->cursor;
  if (c == 'n') {
    if (!match(p, "null")) {
      fail(p, "bad literal");
      return 0;
    }
    out->type = JSON_NULL;
    return 1;
  }
  if (c == 't' || c == 'f') {
    if (match(p, "true")) {
      out->type = JSON_BOOL;
      out->number = 1;
      return 1;
    }
    if (match(p, "false")) {
      out->type = JSON_BOOL;
      out->number = 0;
      return 1;
    }
    fail(p, "bad literal");
    return 0;
  }
  if (c == '"') {
    out->type = JSON_STRING;
    out->string = parse_string_raw(p);
    return out->string != NULL;
  }
  if (c == '[') {
    return parse_array(p, out);
  }
  if (c == '{') {
    return parse_object(p, out);
  }
  /* number; the input buffer is NUL-terminated so strtod cannot overrun */
  char *number_end;
  double d = strtod(p->cursor, &number_end);
  if (number_end == p->cursor) {
    fail(p, "unexpected character");
    return 0;
  }
  out->type = JSON_NUMBER;
  out->number = d;
  p->cursor = number_end;
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
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (error && error_size) {
      snprintf(error, error_size, "cannot open %s", path);
    }
    return NULL;
  }
  long size = -1;
  if (fseek(f, 0, SEEK_END) == 0) {
    size = ftell(f);
  }
  /* the largest real test file is ~2 MB; anything huge is a wrong path, not data */
  if (size < 0 || size > 256L * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    if (error && error_size) {
      snprintf(error, error_size, "cannot size %s", path);
    }
    return NULL;
  }
  char *buffer = alloc_or_exit((size_t)size + 1);
  size_t got = fread(buffer, 1, (size_t)size, f);
  fclose(f);
  if (got != (size_t)size) {
    free(buffer);
    if (error && error_size) {
      snprintf(error, error_size, "cannot read %s", path);
    }
    return NULL;
  }
  /* in bounds: buffer holds size+1 bytes; the analyzer distrusts any ftell-derived
     index regardless of guards */
  buffer[size] = '\0'; /* NOLINT(clang-analyzer-security.ArrayBound) */
  parser_t p = {buffer, buffer, buffer + got, error, error_size, 1};
  json_value *root = alloc_or_exit(sizeof *root);
  int ok = parse_value(&p, root);
  if (ok) {
    skip_whitespace(&p);
    if (p.cursor != p.end) {
      fail(&p, "trailing data");
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
