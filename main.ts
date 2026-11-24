// site-generator v6 driver: spawn the native `solve` binary once and let it
// do all the work. The node side reads and parses nothing on the fast path:
// no cases.json read, no solve.js import, no path/url helpers. If the binary
// cannot run (missing, non-zero exit), fall back to the pure-JS solve.js,
// loaded only then.
//
// Written fresh in this session for the site-generator v6 entry.

import { execFileSync } from "node:child_process";
import { setFlagsFromString } from "node:v8";

// V8's incremental marking off, as the house baseline runs it.
setFlagsFromString("--no-incremental-marking");

// Resolve ./solve from this file's own path, by string ops only.
const self = process.argv[1] || "";
const cut = self.lastIndexOf("/");
const dir = cut > 0 ? self.slice(0, cut) : ".";
const bin = dir + "/solve";

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR || "/input";
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR || "/output";

try {
  execFileSync(bin, [INPUT_DIR, OUTPUT_DIR], { stdio: "ignore" });
} catch {
  // Fallback: pure-JS solve.js, imported only on this path.
  const fs = await import("node:fs");
  const mod = await import("./solve.js");
  const cases = JSON.parse(
    fs.readFileSync(INPUT_DIR + "/cases.json", "utf8"),
  ) as { id: string; input: unknown }[];
  const answers: unknown[] = [];
  for (const c of cases) answers.push(mod.run(c.input));
  fs.writeFileSync(OUTPUT_DIR + "/answers.json", JSON.stringify(answers));
}
