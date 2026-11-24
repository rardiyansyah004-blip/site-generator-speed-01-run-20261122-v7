# site-generator v7 — benchmarks

All numbers local, this VM, node v24.22.1 unless noted, pinned to 1 CPU,
median CPU ms over interleaved runs (`~/workspace/makefun/scratch-v7/`,
total CPU incl. reaped children — the same quantity the heat reads).
Local numbers are only comparable with entries measured beside them on this
machine, never with a cpu_us on the board.

## v7 binary, phase split (in-process timers, practice set, 3 cases)

| Phase | CPU ms |
|---|---|
| JSON parse (cases.json) | 9.0 |
| Build (all 3 cases) | 55.16 |
| Write answers.json (6.25 MB) | ~7 |
| **Binary total (median CPU)** | **71.0–72.0** |

Checkpoint trail (binary median CPU): v6 103.8 → SWAR escapes + memchr
split_lines + SSE2 words_of 77.3 → op-list renderer + arena_dup drop
74.5 → SSE2 escapers 71.0. Fresh profile of build_case:
`text_of_escaped` 28%, `escape_html_into_cstr` 19%, `json_escape_into`
19% — escaping the 6.25 MB output is now the ceiling; further gains
need a different output strategy, not more scan speed.

## A/B: v7 vs v6 (same harness, interleaved, practice set)

Binary alone, 7 runs each:
- v6 median CPU **98.3 ms**, v7 median CPU **72.0 ms** (~27% faster).

Whole entry (`node main.ts`), 9 runs each:
- v6 median wall **261.3 ms** / CPU 260.6 ms.
- v7 median wall **236.8 ms** / CPU 236.1 ms.
- Control median wall **187.9 ms** / CPU 186.6 ms.
- Net vs control: v6 73.4 ms wall / 74.0 ms CPU; v7 48.9 ms wall /
  49.4 ms CPU — v7 won **9/9** pairings, 33% faster net.

Projected server score at the v6 ratio (~1.89): v7 ≈ **93,000 µs**
(v6: 140,000, rank 88) — a real improvement, roughly rank ~70–80, but
still outside the top 10 paid (top line ~20,000–30,000 µs).

## v6 reference (from the v6 build)

## Phase breakdown (measured first, before optimizing)

| Phase | CPU ms |
|---|---|
| Bare `node -e ''` | ~100 |
| Empty `.ts` (type strip) | ~146–163 (strip costs ~50) |
| Imports (fs+path+child_process+v8+url) | ~+2 |
| Null control (parse cases.json + write `[]`) | ~195–211 |
| House baseline main.ts, gross | ~1,304 |
| v5 main.ts, gross | ~309–315 |
| v5 `./solve` binary alone | ~115–129 |
| v5 driver gap (gross − node − binary) | ~36–49 |

The score is gross minus the control, so node startup and type stripping
cancel out. What remains is the binary plus the driver gap. v5's server
score was cpu_us 220,000 (rank 90), i.e. server/local-net ratio ≈ 1.85
(local net ≈ 119 ms).

## v6 binary, phase split (in-process timers, practice set, 3 cases)

| Phase | CPU ms |
|---|---|
| JSON parse (cases.json) | ~9–11 |
| Build (all 3 cases) | ~103 |
| — front matter | ~5 |
| — permalinks | ~1 |
| — markdown | ~26 |
| — collections / tags | ~1 |
| — layouts (template render + emit) | ~67 |
| — report | ~2 |
| Write answers.json (6.25 MB) | ~7–8 |
| **Binary total** | **~99–122** |

## What v6 changed vs v5 (measured, not claimed)

1. **Driver gap**: main.ts imports only `node:child_process` + `node:v8`,
   reads/parses nothing, never imports solve.js on the fast path, resolves
   `./solve` by string ops. (v5's driver gap ~36–49 ms; v6's ~27 ms.)
2. **md_inline batching**: the first build escaped one character per
   function call (~2M calls). Batching plain runs up to the next syntax
   char took the binary from ~180 ms to ~131 ms.
3. **Per-`{% for %}` arena blow-up**: the first build allocated a fresh
   loop/vars VVal pair (plus 4 scalar VVals) per iteration — 37 MB of
   scratch on the biggest fresh case, overflowing the 32 MB arena
   (exit 71). One pair per loop, scalars mutated in place: O(1) arena
   per loop. Scratch per case is now ~5–7 MB.
4. **Lazy buffers + direct-write rendering**: `buf_init` used to malloc
   64 KiB eagerly; `render_value` built a 64 KiB temp buffer per
   `{{ }}` interpolation (~100k+ times) and copied twice. Buffers are
   lazy now and values render straight into the output (escaped as
   written). Binary ~131 ms → ~99 ms.
5. **Pre-sized big buffers**: answers (~2x input) and per-page HTML
   (~body size) are pre-reserved instead of doubling up from 256 bytes.
6. **Bug fixes found by differential testing**: `v_set` grew object
   field arrays instead of writing past them (corrupted `page.title`);
   front-matter value errors now carry the `key "<k>": ` prefix.

## A/B: v6 vs v5 (same harness, interleaved, practice set)

Binary alone, 7 runs each (two batches):
- Batch 1: v5 median ~123 ms, v6 median ~99 ms.
- Batch 2: v5 median ~129 ms, v6 median ~115 ms (machine loaded).

Whole entry (`node main.ts`), 9 runs each:
- v5 median **308.6 ms**, v6 median **275.9 ms** (v6 won 7 of 9 pairings).
- Control median ~211 ms in the same session (earlier ~195 ms).
- Net: v5 ~98 ms, v6 ~65 ms.

Projected server score at the v5 ratio (1.85): v6 ≈ **120,000 µs**
(v5: 220,000) — a real improvement, roughly rank ~55–65, but still
outside the top 10 paid (top line ~10,000–30,000 µs; reaching it would
need local net ≤ ~16 ms, i.e. binary ≤ ~50 ms, which this build does
not achieve — the remaining cost is the template engine rendering
6.25 MB of output, and cutting that 2–3x more needs a rewrite of the
renderer, not another micro-fix).

## Correctness

- Practice set: **3/3 cases byte-identical values** vs the reference
  (also deterministic: two runs byte-identical).
- Fresh generated sets (4 seeds, kit generator): **12/12 cases
  identical** vs the oracle.
- `grade-local --driver process`: see the handoff for the result.
