/* makefun-site 1.0.0, reimplemented in C for speed. The semantics follow
   the kit's vendor README.md and its reference code (the oracle); this file
   is my own fresh implementation of those rules.
   build_case renders one case's output tree as a JSON object into `out`. */
#ifndef SITE_H
#define SITE_H

#include "common.h"
#include "json.h"

/* Render one case (the "input" object: site/templates/files) as a JSON
   object of path -> text, appended to out. Returns 0 on success. */
int build_case(Arena *a, JVal *input, Buf *out);

#endif
