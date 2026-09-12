// Hold the Playground's project model to fixtures: how a ?src= URL is read,
// which files a jsDelivr tree contributes, and what copy-frontend.sh derives
// for the catalog. Needs only node.
//
//   node tools/playground/project_test.mjs

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { parseSourceUrl, projectFiles, cdnBase, treeUrl } from "../../playground/project.js";

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "../..");

// --- parseSourceUrl: the three GitHub spellings agree ---------------------

const want = { owner: "yhirose", repo: "culebra", ref: "master", path: "examples/games/retro-run/retro-run.cul" };
for (const url of [
  "https://github.com/yhirose/culebra/blob/master/examples/games/retro-run/retro-run.cul",
  "https://github.com/yhirose/culebra/raw/master/examples/games/retro-run/retro-run.cul",
  "https://github.com/yhirose/culebra/raw/refs/heads/master/examples/games/retro-run/retro-run.cul",
  "https://github.com/yhirose/culebra/blob/master/examples/games/retro-run/retro-run.cul?raw=true#L12",
  "https://raw.githubusercontent.com/yhirose/culebra/master/examples/games/retro-run/retro-run.cul",
  "https://raw.githubusercontent.com/yhirose/culebra/refs/heads/master/examples/games/retro-run/retro-run.cul",
  "https://cdn.jsdelivr.net/gh/yhirose/culebra@master/examples/games/retro-run/retro-run.cul",
]) assert.deepEqual(parseSourceUrl(url), want, url);

assert.deepEqual(parseSourceUrl("https://raw.githubusercontent.com/o/r/refs/tags/v0.4.0/main.cul"),
                 { owner: "o", repo: "r", ref: "v0.4.0", path: "main.cul" });
// Paths come back decoded: the tree API names files as they are.
assert.deepEqual(parseSourceUrl("https://github.com/o/r/blob/main/my%20dir/%E6%97%A5%E6%9C%AC.cul"),
                 { owner: "o", repo: "r", ref: "main", path: "my dir/日本.cul" });
assert.equal(cdnBase("o", "r", "v1"), "https://cdn.jsdelivr.net/gh/o/r@v1/");
assert.equal(treeUrl("o", "r", "v1"), "https://data.jsdelivr.com/v1/packages/gh/o/r@v1?structure=flat");

// Anywhere else is one file, query and fragment dropped.
assert.deepEqual(parseSourceUrl("https://gist.githubusercontent.com/u/abc/raw/def/prog.cul?x=1"),
                 { url: "https://gist.githubusercontent.com/u/abc/raw/def/prog.cul" });

// A GitHub URL that is not a file, a non-https one, and a non-URL all refuse.
assert.throws(() => parseSourceUrl("https://github.com/yhirose/culebra"), /blob/);
assert.throws(() => parseSourceUrl("https://github.com/yhirose/culebra/tree/master/examples"), /blob/);
assert.throws(() => parseSourceUrl("http://github.com/o/r/blob/main/a.cul"), /https/);
assert.throws(() => parseSourceUrl("retro-run.cul"), /not a URL/);

// --- projectFiles: the entry's directory, recursively, minus the entry ------

const tree = { files: [
  { name: "/README.md", size: 10 },
  { name: "/examples/games/rocci-bird.cul", size: 100 },
  { name: "/examples/games/retro-run/retro-run.cul", size: 50931 },
  { name: "/examples/games/retro-run/README.md", size: 3771 },
  { name: "/examples/games/retro-run/assets/sprites.png", size: 89302 },
  { name: "/examples/games/retro-run/tools/atlas.cul", size: 2197 },
  { name: "/examples/games/retro-runner/other.cul", size: 1 },   // a sibling with the same prefix
] };
assert.deepEqual(projectFiles(tree, "examples/games/retro-run/retro-run.cul"), [
  "examples/games/retro-run/README.md",
  "examples/games/retro-run/assets/sprites.png",
  "examples/games/retro-run/tools/atlas.cul",
]);
// A file beside others at its level takes them too (a directory is the unit).
assert.deepEqual(projectFiles(tree, "examples/games/rocci-bird.cul").length, 5);
// The repo root is the whole repo, and the caps say no.
assert.throws(() => projectFiles(tree, "README.md", { maxFiles: 3 }), /own directory/);
assert.throws(() => projectFiles(tree, "README.md", { maxBytes: 1000 }), /own directory/);
assert.equal(projectFiles(tree, "README.md").length, 6);

// --- the catalog's derived file lists ----------------------------------------
// copy-frontend.sh writes `assets` for every example that lives in its own
// directory (<name>/<name>.cul); the source examples.json carries none.

const source = JSON.parse(fs.readFileSync(path.join(root, "playground/examples.json"), "utf8"));
const served = JSON.parse(fs.readFileSync(path.join(root, "site/playground/examples.json"), "utf8"));
const entries = (catalog) => Object.fromEntries(
  catalog.categories.flatMap((c) => c.examples.map((e) => [e.path, e])));
const src = entries(source), out = entries(served);
assert.deepEqual(Object.keys(src), Object.keys(out), "same entries, same order");
for (const [p, e] of Object.entries(src)) {
  assert.equal(e.assets, undefined, `${p}: assets are derived, not written`);
  const m = /^(.*\/)?([^/]+)\/\2\.cul$/.exec(p);
  if (!m) { assert.deepEqual(out[p].assets, [], p); continue; }
  const dir = path.join(root, (m[1] ?? "") + m[2]);
  const onDisk = fs.readdirSync(dir, { recursive: true, withFileTypes: true })
    .filter((d) => d.isFile())
    .map((d) => path.relative(root, path.join(d.parentPath ?? d.path, d.name)))
    // No dotfile in any segment, the same rule copy-frontend.sh applies.
    .filter((f) => f !== p && !path.relative(dir, path.join(root, f)).split(path.sep).some((s) => s.startsWith(".")))
    .sort();
  assert.deepEqual(out[p].assets, onDisk, p);
  assert.ok(onDisk.length > 0, `${p}: the directory has files beside the entry`);
}
assert.ok(out["examples/games/retro-run/retro-run.cul"].assets.includes("examples/games/retro-run/assets/sprites.png"));

console.log("project: ok");
