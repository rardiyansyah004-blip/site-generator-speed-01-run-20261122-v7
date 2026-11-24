/* makefun-site 1.0.0 in C. Semantics: the kit's vendor README.md + the
   reference implementation (the oracle). This is a fresh implementation
   written in this session; no earlier entry's code was read or copied. */
#include <stdio.h>

#include "site.h"

/* ------------------------------------------------------------------ */
/* Template values                                                    */
/* ------------------------------------------------------------------ */

typedef enum { V_UNDEF, V_STR, V_INT, V_BOOL, V_ARR, V_OBJ } VKind;

/* Object shapes, for the compiled template renderer's verified path
   cache: every object of one shape holds the same fields in the same
   order (each builder appends its fields once, in a fixed order), so a
   cached field index stays valid while the shape matches. Vars objects
   need no shape: the first path segment is resolved by a scope scan. */
enum {
  SH_NONE = 0,
  SH_CARD,    /* card_of: title url date date_long summary excerpt reading_minutes tags */
  SH_PAGE,    /* page: card fields + slug content toc newer older is_post */
  SH_TOC,     /* toc entry: level id html */
  SH_LINK,    /* newer/older: title url date date_long */
  SH_SITE,    /* site: title base_url */
  SH_TAGCARD, /* tag list entry: name slug url count */
  SH_TAGVIEW, /* tag page: name slug url count posts */
  SH_CARDTAG, /* a card's tag: name slug url */
  SH_LOOP     /* loop: index first last length */
};

typedef struct VVal VVal;
struct VVal {
  VKind k;
  Arena *arena;    /* arena that owns this value (for v_set growth) */
  int shape;       /* SH_* above, 0 when the field order is not fixed */
  Str s;           /* V_STR */
  long long n;     /* V_INT */
  int b;           /* V_BOOL */
  VVal **items;    /* V_ARR */
  size_t count;    /* V_ARR */
  Str *names;      /* V_OBJ */
  VVal **vals;     /* V_OBJ */
  size_t nfields;  /* V_OBJ */
  size_t cap;      /* V_OBJ: allocated fields */
};

static VVal *v_new(Arena *a, VKind k) {
  VVal *v = (VVal *)arena_alloc(a, sizeof(VVal));
  memset(v, 0, sizeof *v);
  v->k = k;
  v->arena = a;
  return v;
}
static VVal *v_str(Arena *a, Str s) {
  VVal *v = v_new(a, V_STR);
  v->s = s;
  return v;
}
static VVal *v_int(Arena *a, long long n) {
  VVal *v = v_new(a, V_INT);
  v->n = n;
  return v;
}
static VVal *v_bool(Arena *a, int b) {
  VVal *v = v_new(a, V_BOOL);
  v->b = b;
  return v;
}
static VVal *v_arr(Arena *a, VVal **items, size_t count) {
  VVal *v = v_new(a, V_ARR);
  v->items = items;
  v->count = count;
  return v;
}
static VVal *v_obj(Arena *a, size_t cap) {
  VVal *v = v_new(a, V_OBJ);
  v->cap = cap;
  if (cap > 0) {
    v->names = (Str *)arena_alloc(a, cap * sizeof(Str));
    v->vals = (VVal **)arena_alloc(a, cap * sizeof(VVal *));
  }
  return v;
}
/* Set (or replace, last wins) a field of an object value; grows the field
   arrays when the object was sized too small. */
static void v_set(VVal *obj, Str name, VVal *val) {
  for (size_t i = 0; i < obj->nfields; i++) {
    if (str_eq(obj->names[i], name)) {
      obj->vals[i] = val;
      return;
    }
  }
  if (obj->nfields == obj->cap) {
    size_t nc = obj->cap == 0 ? 4 : obj->cap * 2;
    Str *nn = (Str *)arena_alloc(obj->arena, nc * sizeof(Str));
    VVal **nv = (VVal **)arena_alloc(obj->arena, nc * sizeof(VVal *));
    if (obj->nfields > 0) {
      memcpy(nn, obj->names, obj->nfields * sizeof(Str));
      memcpy(nv, obj->vals, obj->nfields * sizeof(VVal *));
    }
    obj->names = nn;
    obj->vals = nv;
    obj->cap = nc;
  }
  obj->names[obj->nfields] = name;
  obj->vals[obj->nfields] = val;
  obj->nfields++;
}
static VVal *v_get(VVal *obj, Str name) {
  if (!obj || obj->k != V_OBJ) return NULL;
  for (size_t i = 0; i < obj->nfields; i++) {
    if (str_eq(obj->names[i], name)) return obj->vals[i];
  }
  return NULL;
}

typedef struct Scope Scope;
struct Scope {
  VVal *vars; /* V_OBJ */
  Scope *parent;
};

/* The value at path in the scope chain (template.js lookup). */
/* (Replaced by the compiled renderer's op_lookup / op_lookup_base.) */

/* textOf: a value as text (template.js). */
static void text_of(Buf *out, VVal *v, Arena *a);
static void text_of_len(Buf *out, long long n) {
  char tmp[32];
  int w = snprintf(tmp, sizeof tmp, "%lld", n);
  buf_addn(out, tmp, (size_t)w);
}
/* lookup that also resolves a trailing ".length" on an array */
/* (Replaced by the compiled renderer's op_lookup.) */

static void text_of(Buf *out, VVal *v, Arena *a) {
  if (!v) return;
  switch (v->k) {
    case V_STR:
      buf_adds(out, v->s);
      break;
    case V_INT:
      text_of_len(out, v->n);
      break;
    case V_BOOL:
      buf_addz(out, v->b ? "true" : "false");
      break;
    case V_ARR:
      for (size_t i = 0; i < v->count; i++) {
        if (i > 0) buf_addz(out, ", ");
        text_of(out, v->items[i], a);
      }
      break;
    default:
      break;
  }
}

/* textOf, HTML-escaped as it is written: no intermediate buffer, no
   second copy. (Escaping the joined text would give the same bytes --
   ", " holds no escapable char -- but per-item escaping keeps the
   invariant local.) */
static void text_of_escaped(Buf *out, VVal *v) {
  if (!v) return;
  switch (v->k) {
    case V_STR:
      escape_html_into(out, v->s);
      break;
    case V_INT:
      text_of_len(out, v->n);
      break;
    case V_BOOL:
      buf_addz(out, v->b ? "true" : "false");
      break;
    case V_ARR:
      for (size_t i = 0; i < v->count; i++) {
        if (i > 0) buf_addz(out, ", ");
        text_of_escaped(out, v->items[i]);
      }
      break;
    default:
      break;
  }
}

static int truthy(VVal *v) {
  if (!v) return 0;
  if (v->k == V_ARR) return v->count > 0;
  if (v->k == V_STR) return v->s.n > 0;
  if (v->k == V_INT) return v->n != 0;
  if (v->k == V_BOOL) return v->b;
  if (v->k == V_OBJ) return 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Front matter (lib/frontmatter.js)                                  */
/* ------------------------------------------------------------------ */

typedef struct {
  int has_title, has_summary, has_date, has_tags, has_draft, has_layout, has_slug;
  Str title, summary, date, layout, slug;
  Str *tags;
  size_t ntags;
  int draft;
} FmData;

/* Error strings, exactly as the reference reports them. */
static const char *FM_NO_FM = "no front matter";
static const char *FM_NOT_CLOSED = "the front matter is not closed";
static const char *FM_NO_TITLE = "no title";

static int is_key_char(char c) { return (c >= 'a' && c <= 'z') || c == '_'; }

/* A string value: quoted ("..." with \" and \\ only) or bare. */
static int fm_string(Arena *a, Str raw, Str *out, Buf *err) {
  (void)a;
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      buf_addz(err, "a quoted string is not closed");
      return 0;
    }
    buf_clear(err);
    Buf tmp;
    buf_init(&tmp);
    for (size_t i = 1; i + 1 < raw.n; i++) {
      char c = raw.p[i];
      if (c == '\\') {
        char n = raw.p[i + 1];
        if ((n != '"' && n != '\\') || i + 1 == raw.n - 1) {
          buf_addz(err, "a quoted string holds an escape other than \\\" or \\\\");
          free(tmp.buf);
          return 0;
        }
        buf_addc(&tmp, n);
        i++;
      } else if (c == '"') {
        buf_addz(err, "a quoted string holds a stray quote");
        free(tmp.buf);
        return 0;
      } else {
        buf_addc(&tmp, c);
      }
    }
    char *d = arena_dup(a, tmp.buf, tmp.len);
    *out = str_of(d, tmp.len);
    free(tmp.buf);
    return 1;
  }
  if (raw.n > 0 && raw.p[0] == '[') {
    buf_addz(err, "a bare string opens on [");
    return 0;
  }
  *out = raw;
  return 1;
}

static int days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

/* Parse one key's value. err holds the reason on failure. */
static int fm_value(Arena *a, Str key, Str raw, FmData *d, Buf *err) {
  buf_clear(err);
  if (str_eq(key, str_of("title", 5)) || str_eq(key, str_of("summary", 7))) {
    Str v;
    if (!fm_string(a, raw, &v, err)) {
      /* the reference prefixes a value error with its key */
      Buf tmp;
      buf_init(&tmp);
      buf_addz(&tmp, "key \"");
      buf_adds(&tmp, key);
      buf_addz(&tmp, "\": ");
      buf_adds(&tmp, str_of(err->buf, err->len));
      buf_clear(err);
      buf_adds(err, str_of(tmp.buf, tmp.len));
      free(tmp.buf);
      return 0;
    }
    if (str_eq(key, str_of("title", 5))) {
      d->title = v;
      d->has_title = 1;
    } else {
      d->summary = v;
      d->has_summary = 1;
    }
    return 1;
  }
  if (str_eq(key, str_of("date", 4))) {
    int ok = 0;
    if (raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-') {
      int dig = 1;
      for (size_t i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (raw.p[i] < '0' || raw.p[i] > '9') dig = 0;
      }
      if (dig) {
        int y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 +
                (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
        int mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
        int dd = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
        if (y >= 1970 && y <= 2099 && mo >= 1 && mo <= 12 && dd >= 1 &&
            dd <= days_in(y, mo))
          ok = 1;
      }
    }
    if (!ok) {
      buf_addz(err, "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
      return 0;
    }
    d->date = raw;
    d->has_date = 1;
    return 1;
  }
  if (str_eq(key, str_of("tags", 4))) {
    if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
      buf_addz(err, "key \"tags\": not a list [a, b]");
      return 0;
    }
    Str inner = str_of(raw.p + 1, raw.n - 2);
    /* inner.trim() === '' */
    size_t s0 = 0, s1 = inner.n;
    while (s0 < s1 && (inner.p[s0] == ' ' || inner.p[s0] == '\t')) s0++;
    while (s1 > s0 && (inner.p[s1 - 1] == ' ' || inner.p[s1 - 1] == '\t')) s1--;
    if (s0 == s1) {
      d->ntags = 0;
      d->has_tags = 1;
      return 1;
    }
    /* split on ',' */
    size_t cap = 4, cnt = 0;
    Str *tags = (Str *)arena_alloc(a, cap * sizeof(Str));
    size_t pos = 0;
    while (pos <= inner.n) {
      size_t comma = pos;
      while (comma < inner.n && inner.p[comma] != ',') comma++;
      Str t = str_of(inner.p + pos, comma - pos);
      /* trim */
      while (t.n > 0 && (t.p[0] == ' ' || t.p[0] == '\t')) {
        t.p++;
        t.n--;
      }
      while (t.n > 0 && (t.p[t.n - 1] == ' ' || t.p[t.n - 1] == '\t')) t.n--;
      int bad = t.n == 0;
      if (!bad) {
        for (size_t q = 0; q < t.n; q++) {
          if (t.p[q] == '"' || t.p[q] == '[' || t.p[q] == ']') bad = 1;
        }
      }
      if (bad) {
        buf_addz(err, "key \"tags\": an empty tag, or one holding \" [ or ]");
        return 0;
      }
      if (cnt == cap) {
        /* cannot grow arena arrays; over-allocate: tags lists are short,
           but be safe by spilling to a bigger fresh array */
        size_t nc = cap * 2;
        Str *nt = (Str *)arena_alloc(a, nc * sizeof(Str));
        memcpy(nt, tags, cnt * sizeof(Str));
        tags = nt;
        cap = nc;
      }
      tags[cnt++] = t;
      if (comma >= inner.n) break;
      pos = comma + 1;
    }
    d->tags = tags;
    d->ntags = cnt;
    d->has_tags = 1;
    return 1;
  }
  if (str_eq(key, str_of("draft", 5))) {
    if (str_eq(raw, str_of("true", 4))) {
      d->draft = 1;
      d->has_draft = 1;
      return 1;
    }
    if (str_eq(raw, str_of("false", 5))) {
      d->draft = 0;
      d->has_draft = 1;
      return 1;
    }
    buf_addz(err, "key \"draft\": true or false");
    return 0;
  }
  if (str_eq(key, str_of("layout", 6))) {
    int ok = raw.n > 0;
    for (size_t i = 0; ok && i < raw.n; i++) {
      char c = raw.p[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
    }
    if (!ok) {
      buf_addz(err, "key \"layout\": not a template name");
      return 0;
    }
    d->layout = raw;
    d->has_layout = 1;
    return 1;
  }
  if (str_eq(key, str_of("slug", 4))) {
    int ok = raw.n > 0 && raw.n <= 64;
    if (ok) {
      /* /^[a-z0-9]+(?:-[a-z0-9]+)*$/ */
      size_t i = 0;
      while (ok && i < raw.n) {
        size_t j = i;
        while (j < raw.n && ((raw.p[j] >= 'a' && raw.p[j] <= 'z') ||
                             (raw.p[j] >= '0' && raw.p[j] <= '9')))
          j++;
        if (j == i) ok = 0; /* empty run */
        if (ok && j < raw.n) {
          if (raw.p[j] != '-') ok = 0;
          i = j + 1;
          if (i >= raw.n) ok = 0; /* trailing '-' */
        } else {
          i = j;
        }
      }
    }
    if (!ok) {
      buf_addz(err, "key \"slug\": not a slug");
      return 0;
    }
    d->slug = raw;
    d->has_slug = 1;
    return 1;
  }
  buf_addz(err, "unknown key \"");
  buf_adds(err, key);
  buf_addc(err, '"');
  return 0;
}

/* Split text into lines (views, no '\n' included). Returns line count.
   Newlines are found with memchr, not a byte loop. */
static size_t split_lines(Str text, Str **lines_out, Arena *a) {
  size_t cap = 16, cnt = 0;
  Str *lines = (Str *)arena_alloc(a, cap * sizeof(Str));
  size_t i = 0;
  while (i <= text.n) {
    const char *nl = (const char *)memchr(text.p + i, '\n', text.n - i);
    size_t j = nl ? (size_t)(nl - text.p) : text.n;
    if (cnt == cap) {
      size_t nc = cap * 2;
      Str *nl2 = (Str *)arena_alloc(a, nc * sizeof(Str));
      memcpy(nl2, lines, cnt * sizeof(Str));
      lines = nl2;
      cap = nc;
    }
    lines[cnt++] = str_of(text.p + i, j - i);
    if (j >= text.n) break;
    i = j + 1;
  }
  *lines_out = lines;
  return cnt;
}

/* Parse front matter: 1 ok (data filled, body set), 0 refused (err set). */
static int parse_front_matter(Arena *a, Str text, FmData *d, Str *body, Buf *err) {
  memset(d, 0, sizeof *d);
  Str *lines;
  size_t nlines = split_lines(text, &lines, a);
  if (nlines == 0 || !str_eq(lines[0], str_of("---", 3))) {
    buf_addz(err, FM_NO_FM);
    return 0;
  }
  long close = -1;
  for (size_t i = 1; i < nlines; i++) {
    if (str_eq(lines[i], str_of("---", 3))) {
      close = (long)i;
      break;
    }
  }
  if (close < 0) {
    buf_addz(err, FM_NOT_CLOSED);
    return 0;
  }
  static const char *KEYS[7] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};
  for (long i = 1; i < close; i++) {
    Str line = lines[i];
    int blank = 1;
    for (size_t q = 0; q < line.n; q++) {
      if (line.p[q] != ' ' && line.p[q] != '\t') {
        blank = 0;
        break;
      }
    }
    if (blank) continue;
    if (line.n > 0 && line.p[0] == '#') continue;
    /* LINE = /^([a-z_]+):(?: (.*))?$/ */
    size_t k = 0;
    while (k < line.n && is_key_char(line.p[k])) k++;
    if (k == 0 || k >= line.n || line.p[k] != ':' ||
        (k + 1 < line.n && line.p[k + 1] != ' ')) {
      buf_addz(err, "line ");
      char tmp[32];
      int w = snprintf(tmp, sizeof tmp, "%ld", i + 1);
      buf_addn(err, tmp, (size_t)w);
      buf_addz(err, " is not \"key: value\"");
      return 0;
    }
    Str key = str_of(line.p, k);
    Str raw;
    if (k + 1 >= line.n) {
      raw = str_of(line.p + k + 1, 0);
    } else {
      raw = str_of(line.p + k + 2, line.n - (k + 2));
    }
    /* drop trailing spaces/tabs of the value */
    while (raw.n > 0 && (raw.p[raw.n - 1] == ' ' || raw.p[raw.n - 1] == '\t')) raw.n--;
    int known = 0;
    for (int q = 0; q < 7; q++) {
      if (str_eq(key, str_of(KEYS[q], strlen(KEYS[q])))) known = 1;
    }
    if (!known) {
      buf_addz(err, "unknown key \"");
      buf_adds(err, key);
      buf_addc(err, '"');
      return 0;
    }
    int dup = 0;
    if (str_eq(key, str_of("title", 5))) dup = d->has_title;
    else if (str_eq(key, str_of("date", 4))) dup = d->has_date;
    else if (str_eq(key, str_of("tags", 4))) dup = d->has_tags;
    else if (str_eq(key, str_of("draft", 5))) dup = d->has_draft;
    else if (str_eq(key, str_of("layout", 6))) dup = d->has_layout;
    else if (str_eq(key, str_of("slug", 4))) dup = d->has_slug;
    else if (str_eq(key, str_of("summary", 7))) dup = d->has_summary;
    if (dup) {
      buf_addz(err, "key \"");
      buf_adds(err, key);
      buf_addz(err, "\" is given twice");
      return 0;
    }
    if (raw.n == 0) {
      buf_addz(err, "key \"");
      buf_adds(err, key);
      buf_addz(err, "\" has no value");
      return 0;
    }
    buf_clear(err);
    if (!fm_value(a, key, raw, d, err)) return 0;
  }
  if (!d->has_title) {
    buf_addz(err, FM_NO_TITLE);
    return 0;
  }
  /* body = lines after close, joined with '\n' */
  if ((size_t)close + 1 >= nlines) {
    *body = str_of(text.p + text.n, 0);
  } else {
    const char *b0 = lines[close + 1].p;
    size_t bn = (size_t)(text.p + text.n - b0);
    *body = str_of(b0, bn);
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Markdown (lib/markdown.js)                                         */
/* ------------------------------------------------------------------ */

/* A link's href resolution, supplied by the pipeline. */
typedef struct LinkEnv LinkEnv;
struct LinkEnv {
  void *ctx;
  /* Resolve target -> href (a fresh arena string, or the target itself).
     Reports broken links into the ctx. */
  Str (*link)(LinkEnv *env, Str target);
};

typedef struct {
  Str html; /* full block HTML of the body, each block + '\n' */
  Str excerpt;
  /* toc entries: level, id, html (the heading's inline HTML) */
  int *toc_level;
  Str *toc_id;
  Str *toc_html;
  size_t ntoc, toc_cap;
} MdResult;

static int is_blank_line(Str l) {
  for (size_t i = 0; i < l.n; i++) {
    if (l.p[i] != ' ' && l.p[i] != '\t') return 0;
  }
  return 1;
}
static Str trim_line(Str l) {
  while (l.n > 0 && (l.p[0] == ' ' || l.p[0] == '\t')) {
    l.p++;
    l.n--;
  }
  while (l.n > 0 && (l.p[l.n - 1] == ' ' || l.p[l.n - 1] == '\t')) l.n--;
  return l;
}

/* Fence open: ``` + lang [A-Za-z0-9_+-]* + spaces/tabs. Returns lang or
   NULL when the line is not a fence open. */
static int fence_lang(Str l, Str *lang) {
  if (l.n < 3 || l.p[0] != '`' || l.p[1] != '`' || l.p[2] != '`') return 0;
  size_t i = 3;
  while (i < l.n) {
    char c = l.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-')
      i++;
    else break;
  }
  *lang = str_of(l.p + 3, i - 3);
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}
static int fence_close(Str l) {
  if (l.n < 3 || l.p[0] != '`' || l.p[1] != '`' || l.p[2] != '`') return 0;
  size_t i = 3;
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}
/* Heading: 1-6 '#' + space/tab + non-empty text (trailing space/tab cut). */
static int heading_of(Str l, int *depth, Str *text) {
  size_t i = 0;
  while (i < l.n && l.p[i] == '#') i++;
  if (i == 0 || i > 6) return 0;
  if (i >= l.n || (l.p[i] != ' ' && l.p[i] != '\t')) return 0;
  size_t j = i;
  while (j < l.n && (l.p[j] == ' ' || l.p[j] == '\t')) j++;
  Str t = str_of(l.p + j, l.n - j);
  while (t.n > 0 && (t.p[t.n - 1] == ' ' || t.p[t.n - 1] == '\t')) t.n--;
  if (t.n == 0) return 0;
  *depth = (int)i;
  *text = t;
  return 1;
}
static int is_hr(Str l) {
  if (l.n < 3) return 0;
  if (!((l.p[0] == '-' && l.p[1] == '-' && l.p[2] == '-') ||
        (l.p[0] == '*' && l.p[1] == '*' && l.p[2] == '*')))
    return 0;
  size_t i = 3;
  while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
  return i == l.n;
}
static int ends_paragraph(Str l) {
  int d;
  Str t;
  return is_blank_line(l) || fence_lang(l, &t) || heading_of(l, &d, &t) || is_hr(l);
}

static const char ESCAPABLE[] = "\\`*_[]()#+-.!>";

static int is_escapable(char c) { return strchr(ESCAPABLE, c) != NULL; }

/* First index >= from where s holds `what` outside a backslash escape. */
static long find_unescaped(Str s, const char *what, size_t wlen, size_t from) {
  for (size_t j = from; j + wlen <= s.n; j++) {
    if (s.p[j] == '\\' && j + 1 < s.n && is_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (memcmp(s.p + j, what, wlen) == 0) return (long)j;
  }
  return -1;
}
/* First single '*' (one whose run is not **) outside an escape. */
static long find_single_star(Str s, size_t from) {
  for (size_t j = from; j < s.n; j++) {
    char c = s.p[j];
    if (c == '\\' && j + 1 < s.n && is_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < s.n && s.p[j + 1] == '*') {
      j++;
      continue;
    }
    return (long)j;
  }
  return -1;
}

/* JS /\s/ (whitespace) test on one byte of a target (targets are ASCII
   in practice, but the reference tests the whole string). */
static int target_has_ws(Str t) {
  size_t i = 0;
  while (i < t.n) {
    unsigned char c = (unsigned char)t.p[i];
    uint32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else {
      n = utf8_decode(t.p + i, t.n - i, &cp);
    }
    if (js_is_space(cp)) return 1;
    i += n;
  }
  return 0;
}

static void md_inline(Buf *out, Str s, LinkEnv *env, Arena *a);

static void md_inline(Buf *out, Str s, LinkEnv *env, Arena *a) {
  size_t i = 0;
  while (i < s.n) {
    char c = s.p[i];
    if (c == '\\' && i + 1 < s.n && is_escapable(s.p[i + 1])) {
      escape_html_into_cstr(out, s.p + i + 1, 1);
      i += 2;
      continue;
    }
    if (c == '`') {
      size_t j = i + 1;
      while (j < s.n && s.p[j] != '`') j++;
      if (j < s.n && j > i + 1) {
        buf_addz(out, "<code>");
        escape_html_into_cstr(out, s.p + i + 1, j - (i + 1));
        buf_addz(out, "</code>");
        i = j + 1;
      } else {
        buf_addc(out, '`');
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < s.n && s.p[i + 1] == '*') {
        long j = find_unescaped(s, "**", 2, i + 2);
        if (j > (long)(i + 2)) {
          buf_addz(out, "<strong>");
          md_inline(out, str_of(s.p + i + 2, (size_t)j - (i + 2)), env, a);
          buf_addz(out, "</strong>");
          i = (size_t)j + 2;
        } else {
          buf_addz(out, "**");
          i += 2;
        }
        continue;
      }
      long j = find_single_star(s, i + 1);
      if (j > (long)(i + 1)) {
        buf_addz(out, "<em>");
        md_inline(out, str_of(s.p + i + 1, (size_t)j - (i + 1)), env, a);
        buf_addz(out, "</em>");
        i = (size_t)j + 1;
      } else {
        buf_addc(out, '*');
        i++;
      }
      continue;
    }
    if (c == '[') {
      long k = find_unescaped(s, "]", 1, i + 1);
      long m = -1;
      Str target = str_of("", 0);
      if (k != -1 && (size_t)k + 1 < s.n && s.p[k + 1] == '(') {
        size_t r = (size_t)k + 2;
        while (r < s.n && s.p[r] != ')') r++;
        if (r < s.n) {
          m = (long)r;
          target = str_of(s.p + k + 2, (size_t)m - ((size_t)k + 2));
        }
      }
      if (m != -1 && target.n > 0 && !target_has_ws(target)) {
        Str href = env->link(env, target);
        buf_addz(out, "<a href=\"");
        escape_html_into(out, href);
        buf_addz(out, "\">");
        md_inline(out, str_of(s.p + i + 1, (size_t)k - (i + 1)), env, a);
        buf_addz(out, "</a>");
        i = (size_t)m + 1;
        continue;
      }
      buf_addc(out, '[');
      i++;
      continue;
    }
    /* Plain run: everything up to the next inline-syntax char, escaped
       in one call (the escaper copies runs between &<>"' itself). The
       syntax chars are left for their branches above, which render them
       literally when they do not open a span. */
    size_t j = i + 1;
    while (j < s.n) {
      char d = s.p[j];
      if (d == '\\' || d == '`' || d == '*' || d == '[') break;
      j++;
    }
    escape_html_into_cstr(out, s.p + i, j - i);
    i = j;
  }
}

/* Render a body to HTML + toc + excerpt. */
static void render_markdown(Arena *a, Str src, LinkEnv *env, MdResult *res) {
  memset(res, 0, sizeof *res);
  Str *lines;
  size_t nlines = split_lines(src, &lines, a);
  Buf html;
  buf_init(&html);
  /* Rendered HTML runs about the body size; pre-size likewise. */
  buf_reserve(&html, src.n + 1024);
  Buf tmp;
  buf_init(&tmp);
  /* heading ids, unique per page */
  Str *ids = NULL;
  size_t nids = 0, ids_cap = 0;
  size_t i = 0;
  int have_excerpt = 0;
  while (i < nlines) {
    Str line = lines[i];
    if (is_blank_line(line)) {
      i++;
      continue;
    }
    Str lang;
    if (fence_lang(line, &lang)) {
      i++;
      buf_clear(&tmp);
      buf_addz(&tmp, "<pre><code");
      if (lang.n > 0) {
        buf_addz(&tmp, " class=\"language-");
        escape_html_into(&tmp, lang);
        buf_addc(&tmp, '"');
      }
      buf_addc(&tmp, '>');
      while (i < nlines && !fence_close(lines[i])) {
        escape_html_into(&tmp, lines[i]);
        buf_addc(&tmp, '\n');
        i++;
      }
      i++; /* past the close (or past the end) */
      buf_addz(&tmp, "</code></pre>");
      buf_adds(&html, str_of(tmp.buf, tmp.len));
      buf_addc(&html, '\n');
      continue;
    }
    int depth;
    Str htext;
    if (heading_of(line, &depth, &htext)) {
      /* id = unique slug of the heading text */
      Buf ib;
      buf_init(&ib);
      slugify_into(&ib, htext);
      /* uniqueSlug against ids */
      Str base = str_of(ib.buf, ib.len);
      int n = 2;
      Buf ub;
      buf_init(&ub);
      buf_adds(&ub, base);
      for (;;) {
        int taken = 0;
        for (size_t q = 0; q < nids; q++) {
          if (str_eq(ids[q], str_of(ub.buf, ub.len))) {
            taken = 1;
            break;
          }
        }
        if (!taken) break;
        buf_clear(&ub);
        buf_adds(&ub, base);
        buf_addc(&ub, '-');
        char nb[24];
        int w = snprintf(nb, sizeof nb, "%d", n++);
        buf_addn(&ub, nb, (size_t)w);
      }
      if (nids == ids_cap) {
        size_t nc = ids_cap == 0 ? 8 : ids_cap * 2;
        Str *ni = (Str *)arena_alloc(a, nc * sizeof(Str));
        if (nids > 0) memcpy(ni, ids, nids * sizeof(Str));
        ids = ni;
        ids_cap = nc;
      }
      char *idcopy = arena_dup(a, ub.buf, ub.len);
      ids[nids++] = str_of(idcopy, ub.len);
      /* heading's inline HTML */
      buf_clear(&tmp);
      md_inline(&tmp, htext, env, a);
      Str inner = str_of(arena_dup(a, tmp.buf, tmp.len), tmp.len);
      if (depth == 2 || depth == 3) {
        if (res->ntoc == res->toc_cap) {
          size_t nc = res->toc_cap == 0 ? 8 : res->toc_cap * 2;
          int *nl = (int *)arena_alloc(a, nc * sizeof(int));
          Str *ni2 = (Str *)arena_alloc(a, nc * sizeof(Str));
          Str *nh = (Str *)arena_alloc(a, nc * sizeof(Str));
          if (res->ntoc > 0) {
            memcpy(nl, res->toc_level, res->ntoc * sizeof(int));
            memcpy(ni2, res->toc_id, res->ntoc * sizeof(Str));
            memcpy(nh, res->toc_html, res->ntoc * sizeof(Str));
          }
          res->toc_level = nl;
          res->toc_id = ni2;
          res->toc_html = nh;
          res->toc_cap = nc;
        }
        res->toc_level[res->ntoc] = depth;
        res->toc_id[res->ntoc] = str_of(idcopy, ub.len);
        res->toc_html[res->ntoc] = inner;
        res->ntoc++;
      }
      buf_addz(&html, "<h");
      buf_addc(&html, (char)('0' + depth));
      buf_addz(&html, " id=\"");
      buf_adds(&html, str_of(ub.buf, ub.len));
      buf_addz(&html, "\">");
      buf_adds(&html, inner);
      buf_addz(&html, "</h");
      buf_addc(&html, (char)('0' + depth));
      buf_addz(&html, ">\n");
      free(ib.buf);
      free(ub.buf);
      i++;
      continue;
    }
    if (is_hr(line)) {
      buf_addz(&html, "<hr>\n");
      i++;
      continue;
    }
    if (line.n > 0 && line.p[0] == '>') {
      /* collect quote lines: '>' + one space dropped, trimmed, empties
         dropped, joined by '\n' */
      buf_clear(&tmp);
      int first = 1;
      while (i < nlines && lines[i].n > 0 && lines[i].p[0] == '>') {
        Str l = lines[i];
        l.p++;
        l.n--;
        if (l.n > 0 && l.p[0] == ' ') {
          l.p++;
          l.n--;
        }
        l = trim_line(l);
        if (l.n > 0) {
          if (!first) buf_addc(&tmp, '\n');
          first = 0;
          buf_adds(&tmp, l);
        }
        i++;
      }
      if (tmp.len == 0) {
        buf_addz(&html, "<blockquote>\n</blockquote>\n");
      } else {
        buf_addz(&html, "<blockquote>\n<p>");
        md_inline(&html, str_of(tmp.buf, tmp.len), env, a);
        buf_addz(&html, "</p>\n</blockquote>\n");
      }
      continue;
    }
    /* list? bullet first, then ordered */
    {
      int is_ordered = 0;
      long start_num = 1;
      Str first_item;
      int is_list = 0;
      if (line.n >= 2 && (line.p[0] == '-' || line.p[0] == '*') && line.p[1] == ' ') {
        is_list = 1;
        is_ordered = 0;
        first_item = str_of(line.p + 2, line.n - 2);
      } else {
        /* /^(\d{1,9})\. (.*)$/ */
        size_t d = 0;
        while (d < line.n && d < 9 && line.p[d] >= '0' && line.p[d] <= '9') d++;
        if (d >= 1 && d + 1 < line.n && line.p[d] == '.' && line.p[d + 1] == ' ') {
          is_list = 1;
          is_ordered = 1;
          long v = 0;
          for (size_t q = 0; q < d; q++) v = v * 10 + (line.p[q] - '0');
          start_num = v;
          first_item = str_of(line.p + d + 2, line.n - (d + 2));
        }
      }
      if (is_list) {
        /* collect items as (part-count, parts) in the arena */
        typedef struct {
          Str *parts;
          size_t nparts, cap;
        } Item;
        size_t icap = 4, icnt = 0;
        Item *its = (Item *)arena_alloc(a, icap * sizeof(Item));
        /* first item */
        its[0].parts = (Str *)arena_alloc(a, 2 * sizeof(Str));
        its[0].parts[0] = trim_line(first_item);
        its[0].nparts = 1;
        its[0].cap = 2;
        icnt = 1;
        i++;
        while (i < nlines) {
          Str l = lines[i];
          int m = 0;
          Str it;
          if (!is_ordered && l.n >= 2 && (l.p[0] == '-' || l.p[0] == '*') && l.p[1] == ' ') {
            m = 1;
            it = str_of(l.p + 2, l.n - 2);
          } else if (is_ordered) {
            size_t d = 0;
            while (d < l.n && d < 9 && l.p[d] >= '0' && l.p[d] <= '9') d++;
            if (d >= 1 && d + 1 < l.n && l.p[d] == '.' && l.p[d + 1] == ' ') {
              m = 1;
              it = str_of(l.p + d + 2, l.n - (d + 2));
            }
          }
          if (m) {
            if (icnt == icap) {
              size_t nc = icap * 2;
              Item *ni = (Item *)arena_alloc(a, nc * sizeof(Item));
              memcpy(ni, its, icnt * sizeof(Item));
              its = ni;
              icap = nc;
            }
            its[icnt].parts = (Str *)arena_alloc(a, 2 * sizeof(Str));
            its[icnt].parts[0] = trim_line(it);
            its[icnt].nparts = 1;
            its[icnt].cap = 2;
            icnt++;
            i++;
          } else if (l.n >= 2 && l.p[0] == ' ' && l.p[1] == ' ') {
            /* CONTINUED = /^ {2,}\S/: 2+ spaces, then a non-space that
               is not JS whitespace either (a tab after spaces fails). */
            size_t q = 0;
            while (q < l.n && l.p[q] == ' ') q++;
            if (q >= 2 && q < l.n) {
              unsigned char qc = (unsigned char)l.p[q];
              uint32_t qcp;
              int qn;
              if (qc < 0x80) {
                qcp = qc;
                qn = 1;
              } else {
                qn = utf8_decode(l.p + q, l.n - q, &qcp);
              }
              (void)qn;
              if (!js_is_space(qcp)) {
                Item *last = &its[icnt - 1];
                if (last->nparts == last->cap) {
                  size_t nc = last->cap * 2;
                  Str *np = (Str *)arena_alloc(a, nc * sizeof(Str));
                  memcpy(np, last->parts, last->nparts * sizeof(Str));
                  last->parts = np;
                  last->cap = nc;
                }
                last->parts[last->nparts++] = trim_line(l);
                i++;
              } else {
                break;
              }
            } else {
              break;
            }
          } else {
            break;
          }
        }
        if (is_ordered && start_num != 1) {
          buf_addz(&html, "<ol start=\"");
          char nb[24];
          int w = snprintf(nb, sizeof nb, "%ld", start_num);
          buf_addn(&html, nb, (size_t)w);
          buf_addz(&html, "\">\n");
        } else if (is_ordered) {
          buf_addz(&html, "<ol>\n");
        } else {
          buf_addz(&html, "<ul>\n");
        }
        for (size_t q = 0; q < icnt; q++) {
          buf_addz(&html, "<li>");
          buf_clear(&tmp);
          for (size_t r = 0; r < its[q].nparts; r++) {
            if (r > 0) buf_addc(&tmp, '\n');
            buf_adds(&tmp, its[q].parts[r]);
          }
          md_inline(&html, str_of(tmp.buf, tmp.len), env, a);
          buf_addz(&html, "</li>\n");
        }
        buf_addz(&html, is_ordered ? "</ol>\n" : "</ul>\n");
        continue;
      }
    }
    /* paragraph */
    {
      buf_clear(&tmp);
      buf_adds(&tmp, trim_line(line));
      i++;
      while (i < nlines && !ends_paragraph(lines[i])) {
        buf_addc(&tmp, '\n');
        buf_adds(&tmp, trim_line(lines[i]));
        i++;
      }
      Buf inner;
      buf_init(&inner);
      md_inline(&inner, str_of(tmp.buf, tmp.len), env, a);
      if (!have_excerpt) {
        res->excerpt = str_of(arena_dup(a, inner.buf, inner.len), inner.len);
        have_excerpt = 1;
      }
      buf_addz(&html, "<p>");
      buf_adds(&html, str_of(inner.buf, inner.len));
      buf_addz(&html, "</p>\n");
      free(inner.buf);
    }
  }
  res->html = str_of(arena_dup(a, html.buf, html.len), html.len);
  free(html.buf);
  free(tmp.buf);
}

/* ------------------------------------------------------------------ */
/* Templates (lib/template.js)                                        */
/* ------------------------------------------------------------------ */

typedef enum { TN_TEXT, TN_VAR, TN_RAW, TN_FOR, TN_IF, TN_INCLUDE, TN_ROOT } TNKind;

typedef struct TNode TNode;
struct TNode {
  TNKind k;
  Str text; /* TN_TEXT */
  Str *path; /* TN_VAR/TN_RAW/TN_FOR/TN_IF */
  size_t npath;
  Str name; /* TN_FOR (loop var), TN_INCLUDE (template name) */
  int not;  /* TN_IF */
  TNode **body;
  size_t nbody, body_cap; /* TN_FOR body, TN_IF then-branch */
  TNode **els;
  size_t nels, els_cap; /* TN_IF else-branch */
  int has_else;
};

static TNode *tn_new(Arena *a, TNKind k) {
  TNode *n = (TNode *)arena_alloc(a, sizeof(TNode));
  memset(n, 0, sizeof *n);
  n->k = k;
  return n;
}

static void body_push(Arena *a, TNode ***items, size_t *n, size_t *cap, TNode *node) {
  if (*n == *cap) {
    size_t nc = *cap == 0 ? 8 : *cap * 2;
    TNode **ni = (TNode **)arena_alloc(a, nc * sizeof(TNode *));
    if (*n > 0) memcpy(ni, *items, *n * sizeof(TNode *));
    *items = ni;
    *cap = nc;
  }
  (*items)[(*n)++] = node;
}

/* A path is names joined by dots (template.js PATH). */
static int path_valid(Str s) {
  if (s.n == 0) return 0;
  size_t i = 0;
  for (;;) {
    char c = s.p[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0;
    i++;
    while (i < s.n) {
      c = s.p[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_')
        i++;
      else break;
    }
    if (i == s.n) return 1;
    if (s.p[i] != '.') return 0;
    i++;
    if (i >= s.n) return 0;
  }
}
static Str *path_split(Arena *a, Str s, size_t *n_out) {
  size_t cnt = 1;
  for (size_t i = 0; i < s.n; i++) {
    if (s.p[i] == '.') cnt++;
  }
  Str *parts = (Str *)arena_alloc(a, cnt * sizeof(Str));
  size_t n = 0, start = 0;
  for (size_t i = 0; i <= s.n; i++) {
    if (i == s.n || s.p[i] == '.') {
      parts[n++] = str_of(s.p + start, i - start);
      start = i + 1;
    }
  }
  *n_out = n;
  return parts;
}

/* JS String trim (whitespace incl. unicode) of a slice. */
static Str js_trim(Str s) {
  size_t i = 0;
  while (i < s.n) {
    unsigned char c = (unsigned char)s.p[i];
    uint32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else {
      n = utf8_decode(s.p + i, s.n - i, &cp);
    }
    if (!js_is_space(cp)) break;
    i += n;
  }
  size_t j = s.n;
  while (j > i) {
    /* step back one UTF-8 char */
    size_t k = j - 1;
    while (k > i && (s.p[k] & 0xc0) == 0x80) k--;
    uint32_t cp;
    utf8_decode(s.p + k, j - k, &cp);
    if (!js_is_space(cp)) break;
    j = k;
  }
  return str_of(s.p + i, j - i);
}

static void tpl_err(Buf *err, Str name, const char *msg) {
  buf_addz(err, "template \"");
  buf_adds(err, name);
  buf_addz(err, "\": ");
  buf_addz(err, msg);
}

typedef struct {
  int type; /* 0 text, 1 var, 2 raw, 3 tag */
  Str text;
  Str *path;
  size_t npath;
  Str tag;
} Tok;

static Tok *tokenize(Arena *a, Str name, Str src, Buf *err, size_t *ntok_out) {
  size_t cap = 16, cnt = 0;
  Tok *toks = (Tok *)arena_alloc(a, cap * sizeof(Tok));
  Buf text;
  buf_init(&text);
  size_t i = 0;
  while (i < src.n) {
    /* open = indexOf('{', i) */
    size_t open = i;
    while (open < src.n && src.p[open] != '{') open++;
    if (open >= src.n || open == src.n - 1) {
      buf_addn(&text, src.p + i, src.n - i);
      break;
    }
    char next = src.p[open + 1];
    if (next != '{' && next != '%') {
      buf_addn(&text, src.p + i, open + 1 - i);
      i = open + 1;
      continue;
    }
    buf_addn(&text, src.p + i, open - i);
    if (next == '{') {
      int raw = open + 2 < src.n && src.p[open + 2] == '{';
      const char *close_s = raw ? "}}}" : "}}";
      size_t clen = raw ? 3 : 2;
      size_t start = open + clen;
      size_t close = start;
      int found = 0;
      while (close + clen <= src.n) {
        if (memcmp(src.p + close, close_s, clen) == 0) {
          found = 1;
          break;
        }
        close++;
      }
      if (!found) {
        tpl_err(err, name, raw ? "a {{{ is not closed" : "a {{ is not closed");
        free(text.buf);
        return NULL;
      }
      Str inner = js_trim(str_of(src.p + start, close - start));
      if (!path_valid(inner)) {
        buf_addz(err, "template \"");
        buf_adds(err, name);
        buf_addz(err, "\": ");
        json_escape_into(err, inner);
        buf_addz(err, " is not a path");
        free(text.buf);
        return NULL;
      }
      if (text.len > 0) {
        if (cnt == cap) {
          size_t nc = cap * 2;
          Tok *nt = (Tok *)arena_alloc(a, nc * sizeof(Tok));
          memcpy(nt, toks, cnt * sizeof(Tok));
          toks = nt;
          cap = nc;
        }
        toks[cnt].type = 0;
        toks[cnt].text = str_of(arena_dup(a, text.buf, text.len), text.len);
        cnt++;
        buf_clear(&text);
      }
      if (cnt == cap) {
        size_t nc = cap * 2;
        Tok *nt = (Tok *)arena_alloc(a, nc * sizeof(Tok));
        memcpy(nt, toks, cnt * sizeof(Tok));
        toks = nt;
        cap = nc;
      }
      toks[cnt].type = raw ? 2 : 1;
      toks[cnt].path = path_split(a, inner, &toks[cnt].npath);
      cnt++;
      i = close + clen;
      continue;
    }
    /* {% %} */
    size_t start = open + 2;
    size_t close = start;
    int found = 0;
    while (close + 2 <= src.n) {
      if (src.p[close] == '%' && src.p[close + 1] == '}') {
        found = 1;
        break;
      }
      close++;
    }
    if (!found) {
      tpl_err(err, name, "a {% is not closed");
      free(text.buf);
      return NULL;
    }
    size_t end = close + 2;
    size_t line_start = open;
    while (line_start > 0 && src.p[line_start - 1] != '\n') line_start--;
    size_t nl = end;
    while (nl < src.n && src.p[nl] != '\n') nl++;
    size_t line_end = nl >= src.n ? src.n : nl;
    int before_ok = 1, after_ok = 1;
    for (size_t q = line_start; q < open; q++) {
      if (src.p[q] != ' ' && src.p[q] != '\t') before_ok = 0;
    }
    for (size_t q = end; q < line_end; q++) {
      if (src.p[q] != ' ' && src.p[q] != '\t') after_ok = 0;
    }
    int standalone = before_ok && after_ok;
    if (standalone) {
      /* drop the line's leading whitespace from the text so far */
      text.len -= (open - line_start);
    }
    if (text.len > 0) {
      if (cnt == cap) {
        size_t nc = cap * 2;
        Tok *nt = (Tok *)arena_alloc(a, nc * sizeof(Tok));
        memcpy(nt, toks, cnt * sizeof(Tok));
        toks = nt;
        cap = nc;
      }
      toks[cnt].type = 0;
      toks[cnt].text = str_of(arena_dup(a, text.buf, text.len), text.len);
      cnt++;
    }
    buf_clear(&text);
    if (cnt == cap) {
      size_t nc = cap * 2;
      Tok *nt = (Tok *)arena_alloc(a, nc * sizeof(Tok));
      memcpy(nt, toks, cnt * sizeof(Tok));
      toks = nt;
      cap = nc;
    }
    toks[cnt].type = 3;
    toks[cnt].tag = js_trim(str_of(src.p + start, close - start));
    cnt++;
    i = standalone ? (nl >= src.n ? src.n : nl + 1) : end;
  }
  if (text.len > 0) {
    if (cnt == cap) {
      size_t nc = cap * 2;
      Tok *nt = (Tok *)arena_alloc(a, nc * sizeof(Tok));
      memcpy(nt, toks, cnt * sizeof(Tok));
      toks = nt;
      cap = nc;
    }
    toks[cnt].type = 0;
    toks[cnt].text = str_of(arena_dup(a, text.buf, text.len), text.len);
    cnt++;
  }
  free(text.buf);
  *ntok_out = cnt;
  return toks;
}

/* Match ^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$ on a trimmed tag. */
static int match_for(Str tag, Str *vname, Str *path) {
  if (tag.n < 8 || memcmp(tag.p, "for ", 4) != 0) return 0;
  size_t i = 4;
  if (i >= tag.n) return 0;
  char c = tag.p[i];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return 0;
  size_t s0 = i;
  i++;
  while (i < tag.n) {
    c = tag.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_')
      i++;
    else break;
  }
  *vname = str_of(tag.p + s0, i - s0);
  if (i + 4 > tag.n || memcmp(tag.p + i, " in ", 4) != 0) return 0;
  i += 4;
  if (i >= tag.n) return 0;
  for (size_t q = i; q < tag.n; q++) {
    if (tag.p[q] == ' ' || tag.p[q] == '\t' || tag.p[q] == '\n' || tag.p[q] == '\r')
      return 0;
  }
  *path = str_of(tag.p + i, tag.n - i);
  return 1;
}
/* Match ^if (not )?(\S+)$ on a trimmed tag. */
static int match_if(Str tag, int *not, Str *path) {
  if (tag.n < 4 || memcmp(tag.p, "if ", 3) != 0) return 0;
  size_t i = 3;
  *not = 0;
  if (i + 4 <= tag.n && memcmp(tag.p + i, "not ", 4) == 0) {
    *not = 1;
    i += 4;
  }
  if (i >= tag.n) return 0;
  for (size_t q = i; q < tag.n; q++) {
    if (tag.p[q] == ' ' || tag.p[q] == '\t' || tag.p[q] == '\n' || tag.p[q] == '\r')
      return 0;
  }
  *path = str_of(tag.p + i, tag.n - i);
  return 1;
}
/* Match ^include "([a-z0-9-]+)"$ on a trimmed tag. */
static int match_include(Str tag, Str *name) {
  if (tag.n < 11 || memcmp(tag.p, "include \"", 9) != 0) return 0;
  if (tag.p[tag.n - 1] != '"') return 0;
  Str inner = str_of(tag.p + 9, tag.n - 10);
  if (inner.n == 0) return 0;
  for (size_t i = 0; i < inner.n; i++) {
    char c = inner.p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
  }
  *name = inner;
  return 1;
}

typedef struct {
  TNode **nodes;
  size_t count;
} TplBody;

/* Parse a template source into nodes; NULL + err on TemplateError. */
static TplBody *parse_template(Arena *a, Str name, Str src, Buf *err) {
  size_t ntok;
  Tok *toks = tokenize(a, name, src, err, &ntok);
  if (!toks) return NULL;
  TNode *root = tn_new(a, TN_ROOT);
  /* stack of open for/if nodes; body pointer per level */
  TNode *stack[512];
  int depth = 0;
  /* current body */
  TNode ***cur_items = &root->body;
  size_t *cur_n = &root->nbody;
  size_t *cur_cap = &root->body_cap;
  /* root is a pseudo node holding the top-level node list */
  TNode *pseudo = root;
  stack[0] = pseudo;
  depth = 1;
  for (size_t ti = 0; ti < ntok; ti++) {
    Tok *tok = &toks[ti];
    if (tok->type == 0) {
      TNode *n = tn_new(a, TN_TEXT);
      n->text = tok->text;
      body_push(a, cur_items, cur_n, cur_cap, n);
    } else if (tok->type == 1 || tok->type == 2) {
      TNode *n = tn_new(a, tok->type == 1 ? TN_VAR : TN_RAW);
      n->path = tok->path;
      n->npath = tok->npath;
      body_push(a, cur_items, cur_n, cur_cap, n);
    } else {
      Str tag = tok->tag;
      TNode *top = stack[depth - 1];
      Str vname, ptext;
      int notflag;
      Str iname;
      if (match_for(tag, &vname, &ptext)) {
        if (!path_valid(ptext)) {
          buf_addz(err, "template \"");
          buf_adds(err, name);
          buf_addz(err, "\": ");
          json_escape_into(err, ptext);
          buf_addz(err, " is not a path");
          return NULL;
        }
        TNode *n = tn_new(a, TN_FOR);
        n->name = vname;
        n->path = path_split(a, ptext, &n->npath);
        body_push(a, cur_items, cur_n, cur_cap, n);
        if (depth >= 512) {
          tpl_err(err, name, "a for is not closed");
          return NULL;
        }
        stack[depth++] = n;
        cur_items = &n->body;
        cur_n = &n->nbody;
        cur_cap = &n->body_cap;
      } else if (match_if(tag, &notflag, &ptext)) {
        if (!path_valid(ptext)) {
          buf_addz(err, "template \"");
          buf_adds(err, name);
          buf_addz(err, "\": ");
          json_escape_into(err, ptext);
          buf_addz(err, " is not a path");
          return NULL;
        }
        TNode *n = tn_new(a, TN_IF);
        n->not = notflag;
        n->path = path_split(a, ptext, &n->npath);
        body_push(a, cur_items, cur_n, cur_cap, n);
        if (depth >= 512) {
          tpl_err(err, name, "a if is not closed");
          return NULL;
        }
        stack[depth++] = n;
        cur_items = &n->body;
        cur_n = &n->nbody;
        cur_cap = &n->body_cap;
      } else if (str_eq(tag, str_of("else", 4))) {
        if (top->k != TN_IF || top->has_else) {
          tpl_err(err, name, "an else outside an if");
          return NULL;
        }
        top->has_else = 1;
        cur_items = &top->els;
        cur_n = &top->nels;
        cur_cap = &top->els_cap;
      } else if (str_eq(tag, str_of("endfor", 6)) || str_eq(tag, str_of("endif", 5))) {
        int want_for = str_eq(tag, str_of("endfor", 6));
        if (top->k != (want_for ? TN_FOR : TN_IF)) {
          tpl_err(err, name, want_for ? "an endfor with no open for"
                                      : "an endif with no open if");
          return NULL;
        }
        depth--;
        TNode *parent = stack[depth - 1];
        if (parent->k == TN_FOR) {
          cur_items = &parent->body;
          cur_n = &parent->nbody;
          cur_cap = &parent->body_cap;
        } else if (parent->k == TN_IF) {
          if (parent->has_else) {
            cur_items = &parent->els;
            cur_n = &parent->nels;
            cur_cap = &parent->els_cap;
          } else {
            cur_items = &parent->body;
            cur_n = &parent->nbody;
            cur_cap = &parent->body_cap;
          }
        } else {
          /* root pseudo */
          cur_items = &pseudo->body;
          cur_n = &pseudo->nbody;
          cur_cap = &pseudo->body_cap;
        }
      } else if (match_include(tag, &iname)) {
        TNode *n = tn_new(a, TN_INCLUDE);
        n->name = iname;
        body_push(a, cur_items, cur_n, cur_cap, n);
      } else {
        buf_addz(err, "template \"");
        buf_adds(err, name);
        buf_addz(err, "\": {% ");
        buf_adds(err, tag);
        buf_addz(err, " %} is not a tag");
        return NULL;
      }
    }
  }
  if (depth > 1) {
    TNode *top = stack[depth - 1];
    tpl_err(err, name, top->k == TN_FOR ? "a for is not closed" : "a if is not closed");
    return NULL;
  }
  TplBody *b = (TplBody *)arena_alloc(a, sizeof(TplBody));
  b->nodes = pseudo->body;
  b->count = pseudo->nbody;
  return b;
}

/* ------------------------------------------------------------------ */
/* Template rendering, compiled (lib/template.js renderNodes)         */
/*                                                                    */
/* Each template is lowered once to a flat op array and rendered by   */
/* an iterative interpreter: no tree walk, no per-node calls, and     */
/* path lookups use a verified (shape, index) hint per segment, so a  */
/* repeat lookup is one name compare instead of a field scan. The     */
/* hint is verified every time, so a wrong hint can only cost a       */
/* rescan, never a wrong value.                                       */
/* ------------------------------------------------------------------ */

typedef enum {
  OP_TEXT,    /* literal text */
  OP_VAR,     /* {{ path }}, escaped */
  OP_RAW,     /* {{{ path }}}, raw */
  OP_IF,      /* {% if [not] path %}: target = else pc, or end pc */
  OP_JUMP,    /* end of a then-branch: target = end pc */
  OP_FOR,     /* {% for x in path %}: target = pc past ENDFOR */
  OP_ENDFOR,  /* target = for pc */
  OP_INCLUDE, /* {% include "name" %} */
  OP_END      /* end of a template */
} OpKind;

/* A template path is at most 8 segments in practice; longer paths
   still render, they just do not use the cache. */
#define OP_MAX_PATH 8

typedef struct {
  OpKind k;
  Str text;            /* OP_TEXT */
  Str *path;           /* OP_VAR/OP_RAW/OP_IF/OP_FOR */
  size_t npath;
  int not;             /* OP_IF */
  Str name;            /* OP_FOR loop var, OP_INCLUDE template name */
  int target;          /* jump target (see OpKind) */
  int cshape[OP_MAX_PATH]; /* cached parent shape per segment */
  int cidx[OP_MAX_PATH];   /* cached field index per segment */
} Op;

typedef struct {
  Op *ops;
  size_t count;
} OpProg;

typedef struct {
  Str *names;
  TplBody **bodies;
  OpProg **progs;
  size_t count, cap;
} TplMap;

static TplBody *tpl_lookup(TplMap *m, Str name) {
  for (size_t i = 0; i < m->count; i++) {
    if (str_eq(m->names[i], name)) return m->bodies[i];
  }
  return NULL;
}

static OpProg *tpl_prog_lookup(TplMap *m, Str name) {
  for (size_t i = 0; i < m->count; i++) {
    if (str_eq(m->names[i], name)) return m->progs[i];
  }
  return NULL;
}

static void op_clear(Op *op) {
  memset(op, 0, sizeof *op);
  for (int q = 0; q < OP_MAX_PATH; q++) {
    op->cshape[q] = -1;
    op->cidx[q] = -1;
  }
}

static void op_push(Arena *a, Op **ops, size_t *n, size_t *cap, Op *op) {
  if (*n == *cap) {
    size_t nc = *cap == 0 ? 32 : *cap * 2;
    Op *no = (Op *)arena_alloc(a, nc * sizeof(Op));
    if (*n > 0) memcpy(no, *ops, *n * sizeof(Op));
    *ops = no;
    *cap = nc;
  }
  (*ops)[(*n)++] = *op;
}

/* Lower a node list to ops. */
static void compile_nodes(Arena *a, TNode **nodes, size_t count, Op **ops, size_t *n, size_t *cap) {
  for (size_t i = 0; i < count; i++) {
    TNode *nd = nodes[i];
    Op op;
    op_clear(&op);
    switch (nd->k) {
      case TN_TEXT:
        op.k = OP_TEXT;
        op.text = nd->text;
        op_push(a, ops, n, cap, &op);
        break;
      case TN_VAR:
      case TN_RAW:
        op.k = nd->k == TN_VAR ? OP_VAR : OP_RAW;
        op.path = nd->path;
        op.npath = nd->npath;
        op_push(a, ops, n, cap, &op);
        break;
      case TN_IF: {
        op.k = OP_IF;
        op.path = nd->path;
        op.npath = nd->npath;
        op.not = nd->not;
        size_t if_pc = *n;
        op_push(a, ops, n, cap, &op);
        compile_nodes(a, nd->body, nd->nbody, ops, n, cap);
        if (nd->has_else) {
          Op jmp;
          op_clear(&jmp);
          jmp.k = OP_JUMP;
          size_t jmp_pc = *n;
          op_push(a, ops, n, cap, &jmp);
          (*ops)[if_pc].target = (int)*n;
          compile_nodes(a, nd->els, nd->nels, ops, n, cap);
          (*ops)[jmp_pc].target = (int)*n;
        } else {
          (*ops)[if_pc].target = (int)*n;
        }
        break;
      }
      case TN_FOR: {
        op.k = OP_FOR;
        op.path = nd->path;
        op.npath = nd->npath;
        op.name = nd->name;
        size_t for_pc = *n;
        op_push(a, ops, n, cap, &op);
        compile_nodes(a, nd->body, nd->nbody, ops, n, cap);
        Op end;
        op_clear(&end);
        end.k = OP_ENDFOR;
        end.target = (int)for_pc;
        op_push(a, ops, n, cap, &end);
        (*ops)[for_pc].target = (int)*n;
        break;
      }
      case TN_INCLUDE:
        op.k = OP_INCLUDE;
        op.name = nd->name;
        op_push(a, ops, n, cap, &op);
        break;
      default:
        break;
    }
  }
}

static OpProg *compile_template(Arena *a, TplBody *body) {
  Op *ops = NULL;
  size_t n = 0, cap = 0;
  compile_nodes(a, body->nodes, body->count, &ops, &n, &cap);
  Op end;
  op_clear(&end);
  end.k = OP_END;
  op_push(a, &ops, &n, &cap, &end);
  OpProg *prog = (OpProg *)arena_alloc(a, sizeof(OpProg));
  prog->ops = ops;
  prog->count = n;
  return prog;
}

/* One object-field lookup through a verified hint: the hint (shape,
   index) is checked by a single name compare; on any mismatch, scan
   the fields and refresh the hint. Correct with or without a hit. */
static VVal *obj_get_cached(VVal *obj, Str name, int *shape, int *idx) {
  if (obj && obj->k == V_OBJ && *shape >= 0 && *idx >= 0 &&
      obj->shape == *shape && (size_t)*idx < obj->nfields &&
      str_eq(obj->names[*idx], name))
    return obj->vals[*idx];
  VVal *v = v_get(obj, name);
  if (v && obj && obj->k == V_OBJ) {
    *shape = obj->shape;
    *idx = -1;
    for (size_t i = 0; i < obj->nfields; i++) {
      if (str_eq(obj->names[i], name)) {
        *idx = (int)i;
        break;
      }
    }
  }
  return v;
}

/* Resolve the first `count` segments of op->path in scope. Segment 0
   is found by a scope scan (one v_get per frame); later segments use
   the op's cached hints. */
static VVal *op_lookup_base(Scope *scope, Op *op, size_t count) {
  Scope *s = scope;
  VVal *v = NULL;
  while (s) {
    v = v_get(s->vars, op->path[0]);
    if (v) break;
    s = s->parent;
  }
  if (!v) return NULL;
  for (size_t k = 1; k < count; k++) {
    if (v->k != V_OBJ) return NULL;
    if (k >= (size_t)OP_MAX_PATH) {
      v = v_get(v, op->path[k]);
    } else {
      v = obj_get_cached(v, op->path[k], &op->cshape[k], &op->cidx[k]);
    }
    if (!v) return NULL;
  }
  return v;
}

/* Resolve op->path, with a trailing ".length" on an array meaning its
   count (lookup_full in the reference). */
static VVal *op_lookup(Scope *scope, Op *op, long long *len_out, int *is_len) {
  *is_len = 0;
  size_t npath = op->npath;
  if (npath == 0) return NULL;
  if (npath >= 2 && str_eq(op->path[npath - 1], str_of("length", 6))) {
    VVal *base = op_lookup_base(scope, op, npath - 1);
    if (base && base->k == V_ARR) {
      *is_len = 1;
      *len_out = (long long)base->count;
      return NULL;
    }
  }
  return op_lookup_base(scope, op, npath);
}

/* The interpreter's stacks. Includes nest at most 8 deep; fors nest
   as deeply as the template nests them (the parser caps it at 512). */
#define OP_MAX_CALL 16
#define OP_MAX_LOOP 512

typedef struct {
  Op *ops;
  size_t pc;
  int depth;
} CallFrame;

typedef struct {
  Op *ops;
  size_t for_pc;
  size_t body_pc;
  size_t end_pc;
  VVal *arr;
  size_t idx;
  Scope scope;
  Scope *parent;
  VVal *idx_v;
  VVal *first_v;
  VVal *last_v;
  Str varname;
} LoopFrame;

static void op_render(Buf *out, OpProg *prog, Scope *root, TplMap *templates, Arena *a) {
  CallFrame calls[OP_MAX_CALL];
  LoopFrame loops[OP_MAX_LOOP];
  int ncall = 0, nloop = 0;
  Op *ops = prog->ops;
  size_t pc = 0;
  int depth = 0;
  Scope *scope = root;
  for (;;) {
    Op *op = &ops[pc];
    switch (op->k) {
      case OP_TEXT:
        buf_adds(out, op->text);
        pc++;
        break;
      case OP_VAR:
      case OP_RAW: {
        long long len = 0;
        int is_len = 0;
        VVal *v = op_lookup(scope, op, &len, &is_len);
        if (is_len) {
          text_of_len(out, len);
        } else if (op->k == OP_RAW) {
          text_of(out, v, a);
        } else {
          text_of_escaped(out, v);
        }
        pc++;
        break;
      }
      case OP_IF: {
        long long len = 0;
        int is_len = 0;
        VVal *v = op_lookup(scope, op, &len, &is_len);
        int truth = is_len ? len != 0 : truthy(v);
        pc = (truth != op->not) ? pc + 1 : (size_t)op->target;
        break;
      }
      case OP_JUMP:
        pc = (size_t)op->target;
        break;
      case OP_FOR: {
        VVal *xs = op_lookup_base(scope, op, op->npath);
        if (!xs || xs->k != V_ARR || xs->count == 0 || nloop >= OP_MAX_LOOP) {
          pc = (size_t)op->target;
          break;
        }
        LoopFrame *lf = &loops[nloop++];
        lf->ops = ops;
        lf->for_pc = pc;
        lf->body_pc = pc + 1;
        lf->end_pc = (size_t)op->target;
        lf->arr = xs;
        lf->idx = 0;
        lf->varname = op->name;
        lf->parent = scope;
        /* One loop/vars pair for the whole loop, scalars mutated in
           place each iteration: O(1) arena per loop. */
        VVal *loop = v_obj(a, 4);
        loop->shape = SH_LOOP;
        VVal *idx_v = v_int(a, 1);
        VVal *first_v = v_bool(a, 1);
        VVal *last_v = v_bool(a, xs->count == 1);
        VVal *len_v = v_int(a, (long long)xs->count);
        v_set(loop, str_of("index", 5), idx_v);
        v_set(loop, str_of("first", 5), first_v);
        v_set(loop, str_of("last", 4), last_v);
        v_set(loop, str_of("length", 6), len_v);
        VVal *vars = v_obj(a, 2);
        v_set(vars, str_of("loop", 4), loop);
        v_set(vars, op->name, xs->items[0]);
        lf->scope.vars = vars;
        lf->scope.parent = scope;
        lf->idx_v = idx_v;
        lf->first_v = first_v;
        lf->last_v = last_v;
        scope = &lf->scope;
        pc = pc + 1;
        break;
      }
      case OP_ENDFOR: {
        LoopFrame *lf = &loops[nloop - 1];
        lf->idx++;
        if (lf->idx < lf->arr->count) {
          lf->idx_v->n = (long long)lf->idx + 1;
          lf->first_v->b = 0;
          lf->last_v->b = lf->idx == lf->arr->count - 1;
          v_set(lf->scope.vars, lf->varname, lf->arr->items[lf->idx]);
          pc = lf->body_pc;
        } else {
          nloop--;
          scope = lf->parent;
          pc = lf->end_pc;
        }
        break;
      }
      case OP_INCLUDE: {
        OpProg *t = tpl_prog_lookup(templates, op->name);
        if (t && depth < 8 && ncall < OP_MAX_CALL) {
          calls[ncall].ops = ops;
          calls[ncall].pc = pc + 1;
          calls[ncall].depth = depth;
          ncall++;
          ops = t->ops;
          pc = 0;
          depth++;
        } else {
          pc++;
        }
        break;
      }
      case OP_END:
        if (ncall == 0) return;
        ncall--;
        ops = calls[ncall].ops;
        pc = calls[ncall].pc;
        depth = calls[ncall].depth;
        break;
    }
  }
}

/* ------------------------------------------------------------------ */
/* The pipeline (index.js)                                            */
/* ------------------------------------------------------------------ */

typedef struct Tag Tag;
typedef struct Source Source;

struct Tag {
  Str name, slug, url;
  Source **posts;
  size_t nposts, cap;
};

struct Source {
  Str path;
  Str text; /* normalized (\n) source text */
  FmData fm;
  Str body;
  int post;
  Str layout;
  Str slug, out_dir, url;
  Str html, excerpt;
  int *toc_level;
  Str *toc_id, *toc_html;
  size_t ntoc;
  Source *newer, *older;
  Tag **tag_refs;
  size_t ntag_refs;
  VVal *card; /* memoized cardOf */
};

typedef struct {
  Arena *a;
  Str base;
  JVal *input;
  TplMap templates;
  Source **sources;
  size_t nsources;
  Str *statics_path;
  Str *statics_content;
  size_t nstatics;
  Source **posts;
  size_t nposts;
  Tag **tags;
  size_t ntags;
  /* report lines (refusals, drafts, template errors, shadowed) */
  Str *report;
  size_t nreport, report_cap;
  /* broken links, as a set */
  Str *broken;
  size_t nbroken, broken_cap;
  /* built output paths, for the shadowed check */
  Str *built;
  size_t nbuilt, built_cap;
  /* answers output */
  Buf *out;
  int out_first;
} Case;

static void case_report(Case *c, Str line) {
  if (c->nreport == c->report_cap) {
    size_t nc = c->report_cap == 0 ? 32 : c->report_cap * 2;
    Str *nr = (Str *)arena_alloc(c->a, nc * sizeof(Str));
    if (c->nreport > 0) memcpy(nr, c->report, c->nreport * sizeof(Str));
    c->report = nr;
    c->report_cap = nc;
  }
  c->report[c->nreport++] = line;
}
static void case_report_fmt(Case *c, Buf *tmp) {
  case_report(c, str_of(arena_dup(c->a, tmp->buf, tmp->len), tmp->len));
}
static void case_broken(Case *c, Str line) {
  for (size_t i = 0; i < c->nbroken; i++) {
    if (str_eq(c->broken[i], line)) return;
  }
  if (c->nbroken == c->broken_cap) {
    size_t nc = c->broken_cap == 0 ? 32 : c->broken_cap * 2;
    Str *nr = (Str *)arena_alloc(c->a, nc * sizeof(Str));
    if (c->nbroken > 0) memcpy(nr, c->broken, c->nbroken * sizeof(Str));
    c->broken = nr;
    c->broken_cap = nc;
  }
  c->broken[c->nbroken++] = line;
}
static int case_built_has(Case *c, Str path) {
  for (size_t i = 0; i < c->nbuilt; i++) {
    if (str_eq(c->built[i], path)) return 1;
  }
  return 0;
}
static void case_built_add(Case *c, Str path) {
  if (c->nbuilt == c->built_cap) {
    size_t nc = c->built_cap == 0 ? 64 : c->built_cap * 2;
    Str *nr = (Str *)arena_alloc(c->a, nc * sizeof(Str));
    if (c->nbuilt > 0) memcpy(nr, c->built, c->nbuilt * sizeof(Str));
    c->built = nr;
    c->built_cap = nc;
  }
  c->built[c->nbuilt++] = path;
}

/* Emit one output file into the answers object. */
static void emit_file(Case *c, Str path, Str content) {
  if (!c->out_first) buf_addc(c->out, ',');
  c->out_first = 0;
  json_escape_into(c->out, path);
  buf_addc(c->out, ':');
  json_escape_into(c->out, content);
  case_built_add(c, path);
}

static const char *dir_of(Str path, size_t *len_out) {
  size_t i = path.n;
  while (i > 0 && path.p[i - 1] != '/') i--;
  if (i == 0) {
    *len_out = 0;
    return path.p;
  }
  *len_out = i - 1;
  return path.p;
}

/* resolvePath(dir, rel) from index.js; NULL (as a flag) when it climbs out. */
static int resolve_path(Str dir, Str rel, Buf *out) {
  /* split dir into segments on the stack */
  Str segs[64];
  size_t nseg = 0;
  size_t i = 0;
  if (dir.n > 0) {
    while (i <= dir.n) {
      size_t j = i;
      while (j < dir.n && dir.p[j] != '/') j++;
      if (j > i) {
        if (nseg >= 64) return 0;
        segs[nseg++] = str_of(dir.p + i, j - i);
      }
      if (j >= dir.n) break;
      i = j + 1;
    }
  }
  i = 0;
  while (i <= rel.n) {
    size_t j = i;
    while (j < rel.n && rel.p[j] != '/') j++;
    Str seg = str_of(rel.p + i, j - i);
    if (seg.n == 0 || str_eq(seg, str_of(".", 1))) {
      /* skip */
    } else if (str_eq(seg, str_of("..", 2))) {
      if (nseg == 0) return 0;
      nseg--;
    } else {
      if (nseg >= 64) return 0;
      segs[nseg++] = seg;
    }
    if (j >= rel.n) break;
    i = j + 1;
  }
  buf_clear(out);
  for (size_t q = 0; q < nseg; q++) {
    if (q > 0) buf_addc(out, '/');
    buf_adds(out, segs[q]);
  }
  return 1;
}

/* Binary search a sorted array of sources by path. */
static Source *by_source(Case *c, Str path) {
  size_t lo = 0, hi = c->nsources;
  while (lo < hi) {
    size_t mid = (lo + hi) >> 1;
    int cmp = str_cmp(path, c->sources[mid]->path);
    if (cmp == 0) return c->sources[mid];
    if (cmp < 0) hi = mid;
    else lo = mid + 1;
  }
  return NULL;
}
static int static_has(Case *c, Str path) {
  size_t lo = 0, hi = c->nstatics;
  while (lo < hi) {
    size_t mid = (lo + hi) >> 1;
    int cmp = str_cmp(path, c->statics_path[mid]);
    if (cmp == 0) return 1;
    if (cmp < 0) hi = mid;
    else lo = mid + 1;
  }
  return 0;
}

typedef struct {
  Case *c;
  Source *src;
} LinkCtx;

static Str case_link(LinkEnv *env, Str target) {
  LinkCtx *lx = (LinkCtx *)env->ctx;
  Case *c = lx->c;
  Source *s = lx->src;
  /* scheme, absolute, fragment: kept as written */
  if (target.n > 0) {
    /* /^[A-Za-z][A-Za-z0-9+.-]*:/ */
    size_t i = 0;
    if (target.p[0] >= 'A' && target.p[0] <= 'Z' ||
        target.p[0] >= 'a' && target.p[0] <= 'z') {
      i = 1;
      while (i < target.n) {
        char ch = target.p[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.')
          i++;
        else break;
      }
      if (i < target.n && target.p[i] == ':') return target;
    }
    if (target.p[0] == '/' || target.p[0] == '#') return target;
  }
  /* split at the first '#' */
  size_t hash = 0;
  int has_hash = 0;
  for (size_t q = 0; q < target.n; q++) {
    if (target.p[q] == '#') {
      hash = q;
      has_hash = 1;
      break;
    }
  }
  Str rel = has_hash ? str_of(target.p, hash) : target;
  Str frag = has_hash ? str_of(target.p + hash, target.n - hash) : str_of("", 0);
  size_t dlen;
  const char *dp = dir_of(s->path, &dlen);
  Str dir = str_of(dp, dlen);
  Buf pb;
  buf_init(&pb);
  int ok = 1;
  if (rel.n == 0) {
    ok = 0; /* path === null in the reference */
  } else {
    ok = resolve_path(dir, rel, &pb);
  }
  if (ok) {
    Str path = str_of(pb.buf, pb.len);
    Source *hit = by_source(c, path);
    if (hit) {
      Buf hb;
      buf_init(&hb);
      buf_adds(&hb, hit->url);
      buf_adds(&hb, frag);
      char *d = arena_dup(c->a, hb.buf, hb.len);
      Str r = str_of(d, hb.len);
      free(hb.buf);
      free(pb.buf);
      return r;
    }
    if (static_has(c, path)) {
      Buf hb;
      buf_init(&hb);
      buf_adds(&hb, c->base);
      buf_addc(&hb, '/');
      buf_adds(&hb, path);
      buf_adds(&hb, frag);
      char *d = arena_dup(c->a, hb.buf, hb.len);
      Str r = str_of(d, hb.len);
      free(hb.buf);
      free(pb.buf);
      return r;
    }
  }
  free(pb.buf);
  /* broken link: `broken-link ${s.path} -> ${target}`, once */
  Buf lb;
  buf_init(&lb);
  buf_addz(&lb, "broken-link ");
  buf_adds(&lb, s->path);
  buf_addz(&lb, " -> ");
  buf_adds(&lb, target);
  case_broken(c, str_of(arena_dup(c->a, lb.buf, lb.len), lb.len));
  free(lb.buf);
  return target;
}

static int cmp_source_path(const void *x, const void *y) {
  Source *a = *(Source *const *)x, *b = *(Source *const *)y;
  return str_cmp(a->path, b->path);
}
static int cmp_str(const void *x, const void *y) {
  return str_cmp(*(Str *)x, *(Str *)y);
}
static int cmp_post(const void *x, const void *y) {
  Source *a = *(Source *const *)x, *b = *(Source *const *)y;
  int c = str_cmp(b->fm.date, a->fm.date); /* newest first */
  if (c != 0) return c;
  return str_cmp(a->slug, b->slug);
}
static int cmp_tag(const void *x, const void *y) {
  Tag *a = *(Tag *const *)x, *b = *(Tag *const *)y;
  if (a->nposts != b->nposts) return a->nposts > b->nposts ? -1 : 1;
  return str_cmp(a->slug, b->slug);
}

/* uniqueSlug against a taken list (linear; directories are small). */
static Str unique_slug(Arena *a, Str slug, Str **taken, size_t *ntaken, size_t *cap) {
  Buf b;
  buf_init(&b);
  buf_adds(&b, slug);
  int n = 2;
  for (;;) {
    Str cand = str_of(b.buf, b.len);
    int has = 0;
    for (size_t i = 0; i < *ntaken; i++) {
      if (str_eq((*taken)[i], cand)) {
        has = 1;
        break;
      }
    }
    if (!has) break;
    buf_clear(&b);
    buf_adds(&b, slug);
    buf_addc(&b, '-');
    char nb[24];
    int w = snprintf(nb, sizeof nb, "%d", n++);
    buf_addn(&b, nb, (size_t)w);
  }
  if (*ntaken == *cap) {
    size_t nc = *cap == 0 ? 16 : *cap * 2;
    Str *nt = (Str *)arena_alloc(a, nc * sizeof(Str));
    if (*ntaken > 0) memcpy(nt, *taken, *ntaken * sizeof(Str));
    *taken = nt;
    *cap = nc;
  }
  char *d = arena_dup(a, b.buf, b.len);
  (*taken)[(*ntaken)++] = str_of(d, b.len);
  Str r = str_of(d, b.len);
  free(b.buf);
  return r;
}

/* A card value for a source (memoized on the source). */
static VVal *card_of(Case *c, Source *s);
static VVal *tag_view(Case *c, Tag *t);

static VVal *card_of(Case *c, Source *s) {
  if (s->card) return s->card;
  Arena *a = c->a;
  VVal *card = v_obj(a, 8);
  card->shape = SH_CARD;
  v_set(card, str_of("title", 5), v_str(a, s->fm.title));
  v_set(card, str_of("url", 3), v_str(a, s->url));
  v_set(card, str_of("date", 4), v_str(a, s->fm.has_date ? s->fm.date : str_of("", 0)));
  Buf db;
  buf_init(&db);
  if (s->fm.has_date) long_date_into(&db, s->fm.date);
  v_set(card, str_of("date_long", 9), v_str(a, str_of(arena_dup(a, db.buf, db.len), db.len)));
  free(db.buf);
  v_set(card, str_of("summary", 7),
        v_str(a, s->fm.has_summary ? s->fm.summary : str_of("", 0)));
  v_set(card, str_of("excerpt", 7), v_str(a, s->excerpt));
  long mins = (words_of(s->body) + 199) / 200;
  if (mins < 1) mins = 1;
  v_set(card, str_of("reading_minutes", 15), v_int(a, mins));
  VVal **titems = NULL;
  if (s->ntag_refs > 0) titems = (VVal **)arena_alloc(a, s->ntag_refs * sizeof(VVal *));
  for (size_t i = 0; i < s->ntag_refs; i++) {
    Tag *t = s->tag_refs[i];
    VVal *tv = v_obj(a, 3);
    tv->shape = SH_CARDTAG;
    v_set(tv, str_of("name", 4), v_str(a, t->name));
    v_set(tv, str_of("slug", 4), v_str(a, t->slug));
    v_set(tv, str_of("url", 3), v_str(a, t->url));
    titems[i] = tv;
  }
  v_set(card, str_of("tags", 4), v_arr(a, titems, s->ntag_refs));
  s->card = card;
  return card;
}

static VVal *tag_view(Case *c, Tag *t) {
  Arena *a = c->a;
  VVal *tv = v_obj(a, 5);
  tv->shape = SH_TAGVIEW;
  v_set(tv, str_of("name", 4), v_str(a, t->name));
  v_set(tv, str_of("slug", 4), v_str(a, t->slug));
  v_set(tv, str_of("url", 3), v_str(a, t->url));
  v_set(tv, str_of("count", 5), v_int(a, (long long)t->nposts));
  VVal **items = NULL;
  if (t->nposts > 0) items = (VVal **)arena_alloc(a, t->nposts * sizeof(VVal *));
  for (size_t i = 0; i < t->nposts; i++) items[i] = card_of(c, t->posts[i]);
  v_set(tv, str_of("posts", 5), v_arr(a, items, t->nposts));
  return tv;
}

/* Render a template by name with vars; NULL when the template is absent. */
static int render_tpl(Case *c, Str name, VVal *vars, Buf *out) {
  OpProg *t = tpl_prog_lookup(&c->templates, name);
  if (!t) return 0;
  Scope scope;
  scope.vars = vars;
  scope.parent = NULL;
  op_render(out, t, &scope, &c->templates, c->a);
  return 1;
}

/* qsort comparator for the file-sort entries (file scope, used by build_case). */
typedef struct {
  Str key;
  size_t i;
} FileEnt;
int cmp_ent_file(const void *x, const void *y) {
  return str_cmp(((FileEnt *)x)->key, ((FileEnt *)y)->key);
}

int build_case(Arena *a, JVal *input, Buf *out) {
  Case c;
  memset(&c, 0, sizeof c);
  c.a = a;
  c.out = out;
  c.out_first = 1;
  c.input = input;
  JVal *site = json_get(input, "site");
  JVal *templates_j = json_get(input, "templates");
  JVal *files_j = json_get(input, "files");
  if (!site || site->k != J_OBJ || !templates_j || templates_j->k != J_OBJ ||
      !files_j || files_j->k != J_ARR)
    return 1;
  JVal *base_j = json_get(site, "base_url");
  JVal *per_j = json_get(site, "per_page");
  JVal *recent_j = json_get(site, "recent");
  if (!base_j || base_j->k != J_STR || !per_j || per_j->k != J_NUM ||
      !recent_j || recent_j->k != J_NUM)
    return 1;
  /* base = base_url with trailing slashes dropped */
  Str base = base_j->s;
  while (base.n > 0 && base.p[base.n - 1] == '/') base.n--;
  c.base = base;
  long per_page = per_j->num_is_int ? per_j->num : (long)per_j->dbl;
  long recent_n = recent_j->num_is_int ? recent_j->num : (long)recent_j->dbl;

  /* 2. templates, in code-unit order of name */
  {
    size_t nt = templates_j->nfields;
    /* sort indices by key (keys point into the input buffer and are not
       NUL-terminated, so json_get cannot be used with them) */
    FileEnt *tents = (FileEnt *)arena_alloc(a, (nt > 0 ? nt : 1) * sizeof(FileEnt));
    for (size_t i = 0; i < nt; i++) {
      tents[i].key = templates_j->keys[i];
      tents[i].i = i;
    }
    qsort(tents, nt, sizeof(FileEnt), cmp_ent_file);
    c.templates.names = (Str *)arena_alloc(a, (nt > 0 ? nt : 1) * sizeof(Str));
    c.templates.bodies = (TplBody **)arena_alloc(a, (nt > 0 ? nt : 1) * sizeof(TplBody *));
    c.templates.progs = (OpProg **)arena_alloc(a, (nt > 0 ? nt : 1) * sizeof(OpProg *));
    c.templates.count = 0;
    c.templates.cap = nt;
    Buf err;
    buf_init(&err);
    for (size_t i = 0; i < nt; i++) {
      Str tname = tents[i].key;
      JVal *tv = templates_j->vals[tents[i].i];
      if (!tv || tv->k != J_STR) continue;
      buf_clear(&err);
      TplBody *body = parse_template(a, tname, tv->s, &err);
      if (!body) {
        case_report_fmt(&c, &err);
        continue;
      }
      c.templates.names[c.templates.count] = tname;
      c.templates.bodies[c.templates.count] = body;
      c.templates.progs[c.templates.count] = compile_template(a, body);
      c.templates.count++;
    }
    free(err.buf);
  }

  /* 1. sources, sorted by path */
  {
    size_t nf = files_j->count;
    /* sort the file objects by path (FileEnt + cmp_ent_file, file scope) */
    JVal **fs = (JVal **)arena_alloc(a, (nf > 0 ? nf : 1) * sizeof(JVal *));
    for (size_t i = 0; i < nf; i++) fs[i] = files_j->items[i];
    FileEnt *ents = (FileEnt *)arena_alloc(a, (nf > 0 ? nf : 1) * sizeof(FileEnt));
    for (size_t i = 0; i < nf; i++) {
      JVal *p = json_get(fs[i], "path");
      ents[i].key = (p && p->k == J_STR) ? p->s : str_of("", 0);
      ents[i].i = i;
    }
    qsort(ents, nf, sizeof(FileEnt), cmp_ent_file);
    size_t nsrc = 0, nstat = 0;
    c.sources = (Source **)arena_alloc(a, (nf > 0 ? nf : 1) * sizeof(Source *));
    c.statics_path = (Str *)arena_alloc(a, (nf > 0 ? nf : 1) * sizeof(Str));
    c.statics_content = (Str *)arena_alloc(a, (nf > 0 ? nf : 1) * sizeof(Str));
    for (size_t e = 0; e < nf; e++) {
      JVal *f = fs[ents[e].i];
      JVal *p = json_get(f, "path");
      JVal *ct = json_get(f, "content");
      if (!p || p->k != J_STR || !ct || ct->k != J_STR) continue;
      Str path = p->s, content = ct->s;
      if (path.n >= 3 && memcmp(path.p + path.n - 3, ".md", 3) == 0) {
        Source *s = (Source *)arena_alloc(a, sizeof(Source));
        memset(s, 0, sizeof *s);
        s->path = path;
        /* \r\n? -> \n */
        int has_cr = 0;
        for (size_t q = 0; q < content.n; q++) {
          if (content.p[q] == '\r') {
            has_cr = 1;
            break;
          }
        }
        if (!has_cr) {
          s->text = content;
        } else {
          Buf nb;
          buf_init(&nb);
          for (size_t q = 0; q < content.n; q++) {
            if (content.p[q] == '\r') {
              if (q + 1 < content.n && content.p[q + 1] == '\n') q++;
              buf_addc(&nb, '\n');
            } else {
              buf_addc(&nb, content.p[q]);
            }
          }
          s->text = str_of(arena_dup(a, nb.buf, nb.len), nb.len);
          free(nb.buf);
        }
        c.sources[nsrc++] = s;
      } else {
        c.statics_path[nstat] = path;
        c.statics_content[nstat] = content;
        nstat++;
      }
    }
    c.nsources = nsrc;
    c.nstatics = nstat;
  }

  /* front matter + the ordered checks */
  {
    Source **kept = (Source **)arena_alloc(a, (c.nsources > 0 ? c.nsources : 1) * sizeof(Source *));
    size_t nkeep = 0;
    Buf err;
    buf_init(&err);
    for (size_t i = 0; i < c.nsources; i++) {
      Source *s = c.sources[i];
      buf_clear(&err);
      Str body;
      if (!parse_front_matter(a, s->text, &s->fm, &body, &err)) {
        Buf rb;
        buf_init(&rb);
        buf_addz(&rb, "refused ");
        buf_adds(&rb, s->path);
        buf_addz(&rb, ": ");
        buf_adds(&rb, str_of(err.buf, err.len));
        case_report_fmt(&c, &rb);
        free(rb.buf);
        continue;
      }
      s->body = body;
      s->post = s->path.n >= 6 && memcmp(s->path.p, "posts/", 6) == 0;
      const char *own = NULL;
      if (s->path.n >= 5 && memcmp(s->path.p, "tags/", 5) == 0) own = "tags";
      else if (s->path.n >= 5 && memcmp(s->path.p, "page/", 5) == 0) own = "page";
      if (own) {
        Buf rb;
        buf_init(&rb);
        buf_addz(&rb, "refused ");
        buf_adds(&rb, s->path);
        buf_addz(&rb, ": ");
        buf_addz(&rb, own);
        buf_addz(&rb, "/ is the generator's own");
        case_report_fmt(&c, &rb);
        free(rb.buf);
        continue;
      }
      if (s->post && !s->fm.has_date) {
        Buf rb;
        buf_init(&rb);
        buf_addz(&rb, "refused ");
        buf_adds(&rb, s->path);
        buf_addz(&rb, ": a post needs a date");
        case_report_fmt(&c, &rb);
        free(rb.buf);
        continue;
      }
      Str layout = s->fm.has_layout ? s->fm.layout
                                    : str_of(s->post ? "post" : "page", 4);
      if (!tpl_lookup(&c.templates, layout)) {
        Buf rb;
        buf_init(&rb);
        buf_addz(&rb, "refused ");
        buf_adds(&rb, s->path);
        buf_addz(&rb, ": layout \"");
        buf_adds(&rb, layout);
        buf_addz(&rb, "\" is no template that parses");
        case_report_fmt(&c, &rb);
        free(rb.buf);
        continue;
      }
      s->layout = layout;
      if (s->fm.has_draft && s->fm.draft) {
        Buf rb;
        buf_init(&rb);
        buf_addz(&rb, "draft ");
        buf_adds(&rb, s->path);
        case_report_fmt(&c, &rb);
        free(rb.buf);
        continue;
      }
      kept[nkeep++] = s;
    }
    free(err.buf);
    c.sources = kept;
    c.nsources = nkeep;
  }

  /* permalinks */
  {
    /* taken sets per directory: keep (dir, list) pairs */
    typedef struct {
      Str dir;
      Str *taken;
      size_t n, cap;
    } DirTaken;
    size_t ndirs = 0, dcap = 0;
    DirTaken *dirs = NULL;
    for (size_t i = 0; i < c.nsources; i++) {
      Source *s = c.sources[i];
      size_t dlen;
      const char *dp = dir_of(s->path, &dlen);
      Str dir = s->post ? str_of("posts", 5) : str_of(dp, dlen);
      /* find or create the dir's taken set */
      DirTaken *dt = NULL;
      for (size_t q = 0; q < ndirs; q++) {
        if (str_eq(dirs[q].dir, dir)) {
          dt = &dirs[q];
          break;
        }
      }
      if (!dt) {
        if (ndirs == dcap) {
          size_t nc = dcap == 0 ? 8 : dcap * 2;
          DirTaken *nd = (DirTaken *)arena_alloc(a, nc * sizeof(DirTaken));
          if (ndirs > 0) memcpy(nd, dirs, ndirs * sizeof(DirTaken));
          dirs = nd;
          dcap = nc;
        }
        dt = &dirs[ndirs++];
        memset(dt, 0, sizeof *dt);
        dt->dir = dir;
        if (dir.n == 0) {
          /* root: posts, tags, page taken from the start */
          dt->taken = (Str *)arena_alloc(a, 4 * sizeof(Str));
          dt->taken[0] = str_of("posts", 5);
          dt->taken[1] = str_of("tags", 4);
          dt->taken[2] = str_of("page", 4);
          dt->n = 3;
          dt->cap = 4;
        }
      }
      Str base_slug;
      if (s->fm.has_slug) {
        base_slug = s->fm.slug;
      } else {
        base_slug = slugify_str(a, s->fm.title);
      }
      Str slug = unique_slug(a, base_slug, &dt->taken, &dt->n, &dt->cap);
      s->slug = slug;
      Buf ob;
      buf_init(&ob);
      if (dir.n > 0) {
        buf_adds(&ob, dir);
        buf_addc(&ob, '/');
      }
      buf_adds(&ob, slug);
      s->out_dir = str_of(arena_dup(a, ob.buf, ob.len), ob.len);
      free(ob.buf);
      Buf ub;
      buf_init(&ub);
      buf_adds(&ub, c.base);
      buf_addc(&ub, '/');
      buf_adds(&ub, s->out_dir);
      buf_addc(&ub, '/');
      s->url = str_of(arena_dup(a, ub.buf, ub.len), ub.len);
      free(ub.buf);
    }
  }

  /* markdown */
  for (size_t i = 0; i < c.nsources; i++) {
    Source *s = c.sources[i];
    LinkCtx lx;
    lx.c = &c;
    lx.src = s;
    LinkEnv env;
    env.ctx = &lx;
    env.link = case_link;
    MdResult md;
    render_markdown(a, s->body, &env, &md);
    s->html = md.html;
    s->excerpt = md.excerpt;
    s->toc_level = md.toc_level;
    s->toc_id = md.toc_id;
    s->toc_html = md.toc_html;
    s->ntoc = md.ntoc;
  }

  /* collections */
  {
    c.posts = (Source **)arena_alloc(a, (c.nsources > 0 ? c.nsources : 1) * sizeof(Source *));
    size_t np = 0;
    for (size_t i = 0; i < c.nsources; i++) {
      if (c.sources[i]->post) c.posts[np++] = c.sources[i];
    }
    c.nposts = np;
    qsort(c.posts, np, sizeof(Source *), cmp_post);
    for (size_t k = 0; k < np; k++) {
      c.posts[k]->newer = k == 0 ? NULL : c.posts[k - 1];
      c.posts[k]->older = k == np - 1 ? NULL : c.posts[k + 1];
    }
  }

  /* tags */
  {
    Tag **by_slug = (Tag **)arena_alloc(a, (c.nposts > 0 ? c.nposts : 1) * sizeof(Tag *));
    size_t ntags = 0;
    /* first pass: posts in collection order */
    for (size_t pi = 0; pi < c.nposts; pi++) {
      Source *p = c.posts[pi];
      /* ownTags: dedup by slug, first spelling kept */
      Str *seen = (Str *)arena_alloc(a, (p->fm.ntags > 0 ? p->fm.ntags : 1) * sizeof(Str));
      size_t nseen = 0;
      p->tag_refs = (Tag **)arena_alloc(a, (p->fm.ntags > 0 ? p->fm.ntags : 1) * sizeof(Tag *));
      p->ntag_refs = 0;
      for (size_t ti = 0; ti < p->fm.ntags; ti++) {
        Str tname = p->fm.tags[ti];
        Str tsl = slugify_str(a, tname);
        int dup = 0;
        for (size_t q = 0; q < nseen; q++) {
          if (str_eq(seen[q], tsl)) dup = 1;
        }
        if (dup) continue;
        seen[nseen++] = tsl;
        Tag *t = NULL;
        for (size_t q = 0; q < ntags; q++) {
          if (str_eq(by_slug[q]->slug, tsl)) {
            t = by_slug[q];
            break;
          }
        }
        if (!t) {
          t = (Tag *)arena_alloc(a, sizeof(Tag));
          memset(t, 0, sizeof *t);
          t->name = tname;
          t->slug = tsl;
          Buf ub;
          buf_init(&ub);
          buf_adds(&ub, c.base);
          buf_addz(&ub, "/tags/");
          buf_adds(&ub, tsl);
          buf_addc(&ub, '/');
          t->url = str_of(arena_dup(a, ub.buf, ub.len), ub.len);
          free(ub.buf);
          by_slug[ntags++] = t;
        }
        if (t->nposts == t->cap) {
          size_t nc = t->cap == 0 ? 8 : t->cap * 2;
          Source **np2 = (Source **)arena_alloc(a, nc * sizeof(Source *));
          if (t->nposts > 0) memcpy(np2, t->posts, t->nposts * sizeof(Source *));
          t->posts = np2;
          t->cap = nc;
        }
        t->posts[t->nposts++] = p;
        p->tag_refs[p->ntag_refs++] = t;
      }
    }
    /* pages: link to the posts' tag of the same slug, or url "" */
    for (size_t i = 0; i < c.nsources; i++) {
      Source *s = c.sources[i];
      if (s->post) continue;
      Str *seen = (Str *)arena_alloc(a, (s->fm.ntags > 0 ? s->fm.ntags : 1) * sizeof(Str));
      size_t nseen = 0;
      s->tag_refs = (Tag **)arena_alloc(a, (s->fm.ntags > 0 ? s->fm.ntags : 1) * sizeof(Tag *));
      s->ntag_refs = 0;
      for (size_t ti = 0; ti < s->fm.ntags; ti++) {
        Str tname = s->fm.tags[ti];
        Str tsl = slugify_str(a, tname);
        int dup = 0;
        for (size_t q = 0; q < nseen; q++) {
          if (str_eq(seen[q], tsl)) dup = 1;
        }
        if (dup) continue;
        seen[nseen++] = tsl;
        Tag *t = NULL;
        for (size_t q = 0; q < ntags; q++) {
          if (str_eq(by_slug[q]->slug, tsl)) {
            t = by_slug[q];
            break;
          }
        }
        if (!t) {
          t = (Tag *)arena_alloc(a, sizeof(Tag));
          memset(t, 0, sizeof *t);
          t->name = tname;
          t->slug = tsl;
          t->url = str_of("", 0);
          t->posts = NULL;
          t->nposts = 0;
          t->cap = 0;
        }
        s->tag_refs[s->ntag_refs++] = t;
      }
    }
    c.tags = by_slug;
    c.ntags = ntags;
    qsort(c.tags, ntags, sizeof(Tag *), cmp_tag);
  }

  /* layouts */
  {
    VVal *site_v = v_obj(a, 2);
    site_v->shape = SH_SITE;
    JVal *title_j = json_get(site, "title");
    v_set(site_v, str_of("title", 5),
          v_str(a, title_j && title_j->k == J_STR ? title_j->s : str_of("", 0)));
    v_set(site_v, str_of("base_url", 8), v_str(a, c.base));
    VVal **tag_items = NULL;
    if (c.ntags > 0) tag_items = (VVal **)arena_alloc(a, c.ntags * sizeof(VVal *));
    for (size_t i = 0; i < c.ntags; i++) {
      Tag *t = c.tags[i];
      VVal *tv = v_obj(a, 4);
      tv->shape = SH_TAGCARD;
      v_set(tv, str_of("name", 4), v_str(a, t->name));
      v_set(tv, str_of("slug", 4), v_str(a, t->slug));
      v_set(tv, str_of("url", 3), v_str(a, t->url));
      v_set(tv, str_of("count", 5), v_int(a, (long long)t->nposts));
      tag_items[i] = tv;
    }
    VVal *all_tags = v_arr(a, tag_items, c.ntags);
    size_t nrecent = recent_n > 0 ? (size_t)recent_n : 0;
    if (nrecent > c.nposts) nrecent = c.nposts;
    VVal **ritems = NULL;
    if (nrecent > 0) ritems = (VVal **)arena_alloc(a, nrecent * sizeof(VVal *));
    for (size_t i = 0; i < nrecent; i++) ritems[i] = card_of(&c, c.posts[i]);
    VVal *recent_v = v_arr(a, ritems, nrecent);

    Buf hb;
    buf_init(&hb);
    /* every page through its layout */
    for (size_t i = 0; i < c.nsources; i++) {
      Source *s = c.sources[i];
      VVal *page = v_obj(a, 13);
      page->shape = SH_PAGE;
      VVal *card = card_of(&c, s);
      /* page = card fields + slug, content, toc, newer, older, is_post */
      for (size_t f = 0; f < card->nfields; f++) v_set(page, card->names[f], card->vals[f]);
      v_set(page, str_of("slug", 4), v_str(a, s->slug));
      v_set(page, str_of("content", 7), v_str(a, s->html));
      VVal **toc_items = NULL;
      if (s->ntoc > 0) toc_items = (VVal **)arena_alloc(a, s->ntoc * sizeof(VVal *));
      for (size_t q = 0; q < s->ntoc; q++) {
        VVal *tv = v_obj(a, 3);
        tv->shape = SH_TOC;
        v_set(tv, str_of("level", 5), v_int(a, s->toc_level[q]));
        v_set(tv, str_of("id", 2), v_str(a, s->toc_id[q]));
        v_set(tv, str_of("html", 4), v_str(a, s->toc_html[q]));
        toc_items[q] = tv;
      }
      v_set(page, str_of("toc", 3), v_arr(a, toc_items, s->ntoc));
      /* newer / older link records, or null */
      VVal *nv = NULL, *ov = NULL;
      if (s->newer) {
        nv = v_obj(a, 4);
        nv->shape = SH_LINK;
        v_set(nv, str_of("title", 5), v_str(a, s->newer->fm.title));
        v_set(nv, str_of("url", 3), v_str(a, s->newer->url));
        v_set(nv, str_of("date", 4),
              v_str(a, s->newer->fm.has_date ? s->newer->fm.date : str_of("", 0)));
        Buf db2;
        buf_init(&db2);
        if (s->newer->fm.has_date) long_date_into(&db2, s->newer->fm.date);
        v_set(nv, str_of("date_long", 9),
              v_str(a, str_of(arena_dup(a, db2.buf, db2.len), db2.len)));
        free(db2.buf);
      }
      if (s->older) {
        ov = v_obj(a, 4);
        ov->shape = SH_LINK;
        v_set(ov, str_of("title", 5), v_str(a, s->older->fm.title));
        v_set(ov, str_of("url", 3), v_str(a, s->older->url));
        v_set(ov, str_of("date", 4),
              v_str(a, s->older->fm.has_date ? s->older->fm.date : str_of("", 0)));
        Buf db2;
        buf_init(&db2);
        if (s->older->fm.has_date) long_date_into(&db2, s->older->fm.date);
        v_set(ov, str_of("date_long", 9),
              v_str(a, str_of(arena_dup(a, db2.buf, db2.len), db2.len)));
        free(db2.buf);
      }
      v_set(page, str_of("newer", 5), nv ? nv : v_new(a, V_UNDEF));
      v_set(page, str_of("older", 5), ov ? ov : v_new(a, V_UNDEF));
      v_set(page, str_of("is_post", 7), v_bool(a, s->post));
      VVal *vars = v_obj(a, 4);
      v_set(vars, str_of("site", 4), site_v);
      v_set(vars, str_of("tags", 4), all_tags);
      v_set(vars, str_of("recent", 6), recent_v);
      v_set(vars, str_of("page", 4), page);
      buf_clear(&hb);
      if (render_tpl(&c, s->layout, vars, &hb)) {
        Buf pb;
        buf_init(&pb);
        buf_adds(&pb, s->out_dir);
        buf_addz(&pb, "/index.html");
        emit_file(&c, str_of(arena_dup(a, pb.buf, pb.len), pb.len),
                  str_of(hb.buf, hb.len));
        free(pb.buf);
      }
    }
    /* index pages */
    {
      long pages = 1;
      if (per_page > 0 && c.nposts > 0) {
        pages = (long)((c.nposts + (size_t)per_page - 1) / (size_t)per_page);
        if (pages < 1) pages = 1;
      }
      for (long k = 1; k <= pages; k++) {
        size_t from = (size_t)(k - 1) * (size_t)(per_page > 0 ? per_page : 0);
        size_t to = from + (size_t)(per_page > 0 ? per_page : 0);
        if (to > c.nposts) to = c.nposts;
        VVal **pitems = NULL;
        size_t npc = to > from ? to - from : 0;
        if (npc > 0) pitems = (VVal **)arena_alloc(a, npc * sizeof(VVal *));
        for (size_t q = from; q < to; q++) pitems[q - from] = card_of(&c, c.posts[q]);
        VVal *posts_v = v_arr(a, pitems, npc);
        Buf u1, u2, u3;
        buf_init(&u1);
        buf_init(&u2);
        buf_init(&u3);
        if (k == 1) {
          buf_adds(&u1, c.base);
          buf_addc(&u1, '/');
        } else {
          buf_adds(&u1, c.base);
          buf_addz(&u1, "/page/");
          char nb[24];
          int w = snprintf(nb, sizeof nb, "%ld", k);
          buf_addn(&u1, nb, (size_t)w);
          buf_addc(&u1, '/');
        }
        if (k == 1) {
          /* newer_url "" */
        } else if (k - 1 == 1) {
          buf_adds(&u2, c.base);
          buf_addc(&u2, '/');
        } else {
          buf_adds(&u2, c.base);
          buf_addz(&u2, "/page/");
          char nb[24];
          int w = snprintf(nb, sizeof nb, "%ld", k - 1);
          buf_addn(&u2, nb, (size_t)w);
          buf_addc(&u2, '/');
        }
        if (k == pages) {
          /* older_url "" */
        } else {
          buf_adds(&u3, c.base);
          buf_addz(&u3, "/page/");
          char nb[24];
          int w = snprintf(nb, sizeof nb, "%ld", k + 1);
          buf_addn(&u3, nb, (size_t)w);
          buf_addc(&u3, '/');
        }
        VVal *pager = v_obj(a, 5);
        v_set(pager, str_of("number", 6), v_int(a, k));
        v_set(pager, str_of("total", 5), v_int(a, pages));
        v_set(pager, str_of("url", 3),
              v_str(a, str_of(arena_dup(a, u1.buf, u1.len), u1.len)));
        v_set(pager, str_of("newer_url", 9),
              v_str(a, str_of(arena_dup(a, u2.buf, u2.len), u2.len)));
        v_set(pager, str_of("older_url", 9),
              v_str(a, str_of(arena_dup(a, u3.buf, u3.len), u3.len)));
        free(u1.buf);
        free(u2.buf);
        free(u3.buf);
        VVal *vars = v_obj(a, 5);
        v_set(vars, str_of("site", 4), site_v);
        v_set(vars, str_of("tags", 4), all_tags);
        v_set(vars, str_of("recent", 6), recent_v);
        v_set(vars, str_of("posts", 5), posts_v);
        v_set(vars, str_of("pager", 5), pager);
        buf_clear(&hb);
        if (render_tpl(&c, str_of("index", 5), vars, &hb)) {
          if (k == 1) {
            emit_file(&c, str_of("index.html", 10),
                      str_of(hb.buf, hb.len));
          } else {
            Buf pb;
            buf_init(&pb);
            buf_addz(&pb, "page/");
            char nb[24];
            int w = snprintf(nb, sizeof nb, "%ld", k);
            buf_addn(&pb, nb, (size_t)w);
            buf_addz(&pb, "/index.html");
            emit_file(&c, str_of(arena_dup(a, pb.buf, pb.len), pb.len),
                      str_of(hb.buf, hb.len));
            free(pb.buf);
          }
        }
      }
    }
    /* tag pages */
    for (size_t ti = 0; ti < c.ntags; ti++) {
      Tag *t = c.tags[ti];
      VVal *vars = v_obj(a, 4);
      v_set(vars, str_of("site", 4), site_v);
      v_set(vars, str_of("tags", 4), all_tags);
      v_set(vars, str_of("recent", 6), recent_v);
      v_set(vars, str_of("tag", 3), tag_view(&c, t));
      buf_clear(&hb);
      if (render_tpl(&c, str_of("tag", 3), vars, &hb)) {
        Buf pb;
        buf_init(&pb);
        buf_addz(&pb, "tags/");
        buf_adds(&pb, t->slug);
        buf_addz(&pb, "/index.html");
        emit_file(&c, str_of(arena_dup(a, pb.buf, pb.len), pb.len),
                  str_of(hb.buf, hb.len));
        free(pb.buf);
      }
    }
    /* tags page */
    {
      VVal *vars = v_obj(a, 3);
      v_set(vars, str_of("site", 4), site_v);
      v_set(vars, str_of("tags", 4), all_tags);
      v_set(vars, str_of("recent", 6), recent_v);
      buf_clear(&hb);
      if (render_tpl(&c, str_of("tags", 4), vars, &hb)) {
        emit_file(&c, str_of("tags/index.html", 15),
                  str_of(arena_dup(a, hb.buf, hb.len), hb.len));
      }
    }
    free(hb.buf);
    /* statics */
    for (size_t i = 0; i < c.nstatics; i++) {
      Str path = c.statics_path[i];
      if (case_built_has(&c, path)) {
        Buf rb;
        buf_init(&rb);
        buf_addz(&rb, "shadowed ");
        buf_adds(&rb, path);
        buf_addz(&rb, ": a built page has its path");
        case_report_fmt(&c, &rb);
        free(rb.buf);
      } else {
        emit_file(&c, path, c.statics_content[i]);
      }
    }
  }

  /* report */
  {
    size_t total = c.nreport + c.nbroken;
    Str *lines = (Str *)arena_alloc(a, (total > 0 ? total : 1) * sizeof(Str));
    size_t n = 0;
    for (size_t i = 0; i < c.nreport; i++) lines[n++] = c.report[i];
    for (size_t i = 0; i < c.nbroken; i++) lines[n++] = c.broken[i];
    qsort(lines, n, sizeof(Str), cmp_str);
    Buf rb;
    buf_init(&rb);
    for (size_t i = 0; i < n; i++) {
      buf_adds(&rb, lines[i]);
      buf_addc(&rb, '\n');
    }
    size_t npages = 0;
    for (size_t i = 0; i < c.nsources; i++) {
      if (!c.sources[i]->post) npages++;
    }
    char tail[128];
    int w = snprintf(tail, sizeof tail, "built %zu pages, %zu posts, %zu tags and %zu static files\n",
                     npages, c.nposts, c.ntags, c.nstatics);
    buf_addn(&rb, tail, (size_t)w);
    emit_file(&c, str_of("build-report.txt", 16),
              str_of(arena_dup(a, rb.buf, rb.len), rb.len));
    free(rb.buf);
  }

  return 0;
}
