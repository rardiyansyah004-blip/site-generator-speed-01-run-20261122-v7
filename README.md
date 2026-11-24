# site-generator v7 — native C port of makefun-site 1.0.0

[![make.fun](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/rardiyansyah004-blip/site-generator-speed-01-run-20261122-v7/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/rardiyansyah004-blip/site-generator-speed-01-run-20261122-v7/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0, for the
`site-generator-speed-01-run-20261122` competition (cpu_us, lower is better).

## Layout

- `main.ts` — the driver the harness runs (`node /bot/main.ts`). It spawns
  the native `solve` binary once with the input and output directories and
  does nothing else on the fast path: it reads no cases.json, imports no
  solve.js, and pulls in only `node:child_process` and `node:v8`. If the
  binary cannot run, it falls back to the pure-JS `solve.js`, imported only
  on that path.
- `solve` — statically linked x86-64 binary (`gcc -O3 -static`), built from
  `src/`. Reads `<input>/cases.json`, builds every case, writes
  `<output>/answers.json` (a JSON array of output trees, one per case, in
  input order). Exits non-zero on any failure so the driver falls back.
- `src/main.c` — driver: read cases.json, parse once, build each case into
  a fresh arena, serialize the answers array.
- `src/site.c` — the whole makefun-site pipeline, written fresh: front
  matter, Markdown subset, template dialect, slugs, permalinks, collections,
  tags, layouts, report. Semantics follow the kit's vendor code
  (`baselines/makefun-site/vendor/makefun-site/`), which is the reference
  wherever the README and the code differ.
- `src/json.c` — arena JSON parser (strings without escapes point straight
  into the input buffer) and JSON.stringify-style escaper.
- `src/common.c` — arena, buffers, UTF-8, slug rule, HTML escaping, word
  counting, long dates.
- `src/unicode.h` — generated NFKD decomposition + combining-mark tables
  (Python `unicodedata`, Unicode 15.0; the generator's repertoire is fixed
  old characters, so the version delta cannot matter here).
- `solve.js` — the pure-JS fallback (the v3 entry's solve.js, verbatim).
  Used only if the binary cannot run.

## Build

```sh
gcc -O3 -static -o solve src/*.c
```

Baseline x86-64, no `-march=native`: the grading CPU may differ from the
build CPU.

## Why native, why this shape

The score is the entry's CPU time net of a do-nothing control, so node
startup and type stripping cancel out; what remains is the driver's own
work plus the binary. v7 rewrites the template renderer: each layout is
compiled once into a flat op list and rendered by a single iterative
loop (no recursive tree walk), with verified per-op path-lookup hints,
SSE2 byte scans in both escapers, memchr `split_lines`, and no
`arena_dup` of rendered HTML. Measured: binary 98.3 ms (v6) → 72.0 ms
(v7), whole-entry net 74.0 ms → 49.4 ms, winning 9/9 interleaved
pairings; projected server score ~93,000 µs (v6: 140,000). Still outside
the top 10 paid (~20,000–30,000 µs) — escaping the 6.25 MB output is
now the ceiling. See BENCHMARKS.md for the measured breakdown and the
A/B against v6.
