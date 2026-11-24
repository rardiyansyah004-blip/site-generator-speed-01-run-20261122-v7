/* Shared low-level pieces for the v6 site generator: arena, growable
   buffer, string views, UTF-8 helpers, the slug rule, HTML escaping.
   Written fresh in this session for site-generator-speed-01-run-20261122. */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- a byte slice (not NUL-terminated) ---- */
typedef struct {
  const char *p;
  size_t n;
} Str;

static inline Str str_of(const char *p, size_t n) {
  Str s;
  s.p = p;
  s.n = n;
  return s;
}
static inline int str_eq(Str a, Str b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}
/* code-unit order (JS < on strings): for the ASCII sort keys this build
   ever compares, byte order and UTF-16 code-unit order agree. */
static inline int str_cmp(Str a, Str b) {
  size_t n = a.n < b.n ? a.n : b.n;
  int c = n == 0 ? 0 : memcmp(a.p, b.p, n);
  if (c != 0) return c;
  return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

/* ---- bump arena: one big block, mark/reset for per-case scratch ---- */
typedef struct {
  char *base;
  char *ptr;
  char *end;
} Arena;

static inline void arena_init(Arena *a, size_t bytes) {
  a->base = (char *)malloc(bytes);
  if (!a->base) _exit(70);
  a->ptr = a->base;
  a->end = a->base + bytes;
}
static inline void *arena_alloc(Arena *a, size_t n) {
  /* 8-byte alignment */
  n = (n + 7) & ~(size_t)7;
  if (a->ptr + n > a->end) _exit(71);
  void *p = a->ptr;
  a->ptr += n;
  return p;
}
static inline char *arena_dup(Arena *a, const char *p, size_t n) {
  char *d = (char *)arena_alloc(a, n + 1);
  memcpy(d, p, n);
  d[n] = 0;
  return d;
}
static inline size_t arena_mark(Arena *a) { return (size_t)(a->ptr - a->base); }
static inline void arena_reset(Arena *a, size_t mark) { a->ptr = a->base + mark; }

/* ---- growable byte buffer ---- */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} Buf;

static inline void buf_init(Buf *b) {
  /* Lazy: no allocation until the first write. (The old eager 64 KiB
     malloc per buffer cost real time: render_value built one per
     interpolation.) */
  b->cap = 0;
  b->len = 0;
  b->buf = NULL;
}
static inline void buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra > b->cap) {
    size_t nc = b->cap == 0 ? 256 : b->cap;
    while (nc < b->len + extra) nc *= 2;
    b->buf = (char *)realloc(b->buf, nc);
    if (!b->buf) _exit(70);
    b->cap = nc;
  }
}
static inline void buf_addn(Buf *b, const void *p, size_t n) {
  buf_reserve(b, n);
  memcpy(b->buf + b->len, p, n);
  b->len += n;
}
static inline void buf_adds(Buf *b, Str s) { buf_addn(b, s.p, s.n); }
static inline void buf_addc(Buf *b, char c) {
  if (b->len == b->cap) buf_reserve(b, 1);
  b->buf[b->len++] = c;
}
static inline void buf_addz(Buf *b, const char *z) { buf_addn(b, z, strlen(z)); }
static inline void buf_clear(Buf *b) { b->len = 0; }

/* ---- UTF-8 ---- */
/* Decode one code point at s[0..n); returns its length in bytes (1-4),
   or 1 for a stray byte (treated as U+FFFD by callers that care). */
static inline int utf8_decode(const char *s, size_t n, uint32_t *cp) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  if (c >= 0xc2 && c < 0xe0 && n >= 2 && ((unsigned char)s[1] & 0xc0) == 0x80) {
    *cp = ((c & 0x1f) << 6) | ((unsigned char)s[1] & 0x3f);
    return 2;
  }
  if (c >= 0xe0 && c < 0xf0 && n >= 3 && ((unsigned char)s[1] & 0xc0) == 0x80 &&
      ((unsigned char)s[2] & 0xc0) == 0x80) {
    *cp = ((c & 0x0f) << 12) | (((unsigned char)s[1] & 0x3f) << 6) |
          ((unsigned char)s[2] & 0x3f);
    return 3;
  }
  if (c >= 0xf0 && c < 0xf5 && n >= 4 && ((unsigned char)s[1] & 0xc0) == 0x80 &&
      ((unsigned char)s[2] & 0xc0) == 0x80 && ((unsigned char)s[3] & 0xc0) == 0x80) {
    *cp = ((c & 0x07) << 18) | (((unsigned char)s[1] & 0x3f) << 12) |
          (((unsigned char)s[2] & 0x3f) << 6) | ((unsigned char)s[3] & 0x3f);
    return 4;
  }
  *cp = 0xfffd;
  return 1;
}
/* Encode cp as UTF-8 into out (4 bytes max); returns the byte count. */
static inline int utf8_encode(uint32_t cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

/* JS whitespace (White_Space + LineTerminator) as code points. */
static inline int js_is_space(uint32_t cp) {
  if (cp == 0x09 || cp == 0x0a || cp == 0x0b || cp == 0x0c || cp == 0x0d ||
      cp == 0x20 || cp == 0x00a0 || cp == 0x1680 || cp == 0x2028 ||
      cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000 ||
      cp == 0xfeff)
    return 1;
  return cp >= 0x2000 && cp <= 0x200a;
}

/* ---- the slug rule (lib/text.js): NFKD, drop marks, a-z0-9 runs, 64 ---- */
int is_mark(uint32_t cp); /* unicode.h ranges */
/* Append slugify(text) to out. */
void slugify_into(Buf *out, Str text);
/* The slug of text as a fresh arena string. */
Str slugify_str(Arena *a, Str text);

/* HTML escaping (& < > " ') appended to out. */
void escape_html_into(Buf *out, Str s);
void escape_html_into_cstr(Buf *out, const char *p, size_t n);

/* The body's words (runs of non-JS-whitespace), as lib/index.js wordsOf. */
long words_of(Str text);

/* "2026-11-09" -> "9 November 2026" (from the string alone). */
void long_date_into(Buf *out, Str date);

#endif
