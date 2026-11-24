/* main.c -- driver for the site-generator v6 native binary.
 *
 * Usage: solve <input_dir> <output_dir>
 * Reads <input_dir>/cases.json (a JSON array of {"id","input"}), builds
 * every case with the C port in site.c, and writes <output_dir>/answers.json
 * (a JSON array of output trees, one per case, in input order).
 *
 * Written fresh in this session for the site-generator v6 entry.
 * Exit code is non-zero on any failure, so the node driver falls back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "json.h"
#include "site.h"

static char *read_file(const char *path, size_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) {
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  *len_out = (size_t)n;
  return buf;
}

static int write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return 1;
  size_t put = fwrite(data, 1, len, f);
  int rc = fclose(f);
  return (put == len && rc == 0) ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: solve <input_dir> <output_dir>\n");
    return 2;
  }
  /* <input>/cases.json */
  Buf in_path, out_path;
  buf_init(&in_path);
  buf_init(&out_path);
  buf_addz(&in_path, argv[1]);
  buf_addc(&in_path, '/');
  buf_addz(&in_path, "cases.json");
  buf_addc(&in_path, '\0'); /* fopen needs a C string */
  buf_addz(&out_path, argv[2]);
  buf_addc(&out_path, '/');
  buf_addz(&out_path, "answers.json");
  buf_addc(&out_path, '\0'); /* fopen needs a C string */

  size_t in_len = 0;
  char *in = read_file(in_path.buf, &in_len);
  if (!in) {
    fprintf(stderr, "solve: cannot read %s\n", in_path.buf);
    return 2;
  }

  Arena dom_a, scratch_a;
  arena_init(&dom_a, 1 << 23);     /* 8 MiB: the cases.json DOM */
  arena_init(&scratch_a, 1 << 25); /* 32 MiB: per-case scratch */
  Arena *dom = &dom_a;
  Arena *scratch = &scratch_a;
  JVal *cases = json_parse(dom, in, in_len);
  if (!cases || cases->k != J_ARR) {
    fprintf(stderr, "solve: cases.json is not a JSON array\n");
    return 2;
  }

  Buf answers;
  buf_init(&answers);
  /* Answers run ~2x the input size; pre-size so the buffer does not
     double-copy its way up from 256 bytes. */
  buf_reserve(&answers, in_len * 2 + 1024);
  buf_addc(&answers, '[');
  for (size_t i = 0; i < cases->count; i++) {
    JVal *cs = cases->items[i];
    JVal *input = json_get(cs, "input");
    if (!input) {
      fprintf(stderr, "solve: case %zu has no input\n", i);
      return 2;
    }
    if (i > 0) buf_addc(&answers, ',');
    buf_addc(&answers, '{');
    size_t mark = arena_mark(scratch);
    if (build_case(scratch, input, &answers) != 0) {
      fprintf(stderr, "solve: build_case failed on case %zu\n", i);
      return 2;
    }
    arena_reset(scratch, mark);
    buf_addc(&answers, '}');
  }
  buf_addc(&answers, ']');

  int rc = write_file(out_path.buf, answers.buf, answers.len);
  if (rc != 0) {
    fprintf(stderr, "solve: cannot write %s\n", out_path.buf);
    return 2;
  }
  return 0;
}
