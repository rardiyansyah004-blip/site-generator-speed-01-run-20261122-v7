/* Minimal JSON: a DOM parser over an arena, and the string escaping the
   answers serializer needs. Written fresh in this session. */
#ifndef JSON_H
#define JSON_H

#include "common.h"

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct JVal JVal;
struct JVal {
  JKind k;
  int boolean;       /* J_BOOL */
  long long num;     /* J_NUM, when integral */
  double dbl;        /* J_NUM, otherwise */
  int num_is_int;    /* J_NUM */
  Str s;             /* J_STR */
  JVal **items;      /* J_ARR */
  size_t count;      /* J_ARR */
  Str *keys;         /* J_OBJ */
  JVal **vals;       /* J_OBJ */
  size_t nfields;    /* J_OBJ */
};

/* Parse a whole JSON document (not NUL-terminated). Returns NULL on error. */
JVal *json_parse(Arena *a, const char *p, size_t n);

/* Look a key up in an object value; NULL when absent or not an object. */
JVal *json_get(JVal *obj, const char *key);

/* Append a JSON-escaped string (with quotes) to out, as JSON.stringify
   does: " \ and control chars escaped, anything else raw UTF-8. */
void json_escape_into(Buf *out, Str s);

#endif
