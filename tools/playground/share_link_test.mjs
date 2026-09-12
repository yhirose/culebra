// Round-trip the Playground's share-link codec through the module the page
// itself loads. Needs only node: the codec has no DOM and no imports.
//
//   node tools/playground/share_link_test.mjs

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { encodeShareParam, decodeShareParam } from "../../playground/share-link.js";

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), "../..");
const example = (rel) => fs.readFileSync(path.join(root, "examples", rel), "utf8");

const GZIP_PREFIX = "H4sI";   // base64 of 1f 8b 08
const B64URL = /^[A-Za-z0-9_-]*$/;

async function roundTrip(src) {
  const param = await encodeShareParam(src);
  assert.match(param, B64URL, "payload must be base64url with no padding");
  assert.equal(await decodeShareParam(param), src);
  return param;
}

// Plain, non-ASCII, and the whole programs a link exists for.
await roundTrip('print("hello")\n');
await roundTrip("# こんにちは 🐍\nlet s = \"日本語\"\n");
await roundTrip(example("basics/greeting.cul"));
await roundTrip(example("games/rocci-bird.cul"));

// The picker: a snippet stays plain, a program is gzipped, and the choice is
// the shorter of the two.
assert.doesNotMatch(await roundTrip(example("basics/greeting.cul")), new RegExp("^" + GZIP_PREFIX));
const big = await roundTrip(example("games/samegame.cul"));
assert.match(big, new RegExp("^" + GZIP_PREFIX));
const plainBig = Buffer.from(example("games/samegame.cul")).toString("base64url");
assert.ok(big.length < plainBig.length, `gzip ${big.length} must beat plain ${plainBig.length}`);

// The zlib-sniffing trap: `x ` (0x78 0x20) passes zlib's header check. The
// codec sniffs gzip only, so this must survive as source. A long plain
// payload only ever comes from a legacy link (the encoder gzips anything past
// a few dozen bytes), so hand the decoder one directly.
await roundTrip("x = 1\n");
const zlibTrap = "x " + "y".repeat(4000) + "\n";
assert.equal(await decodeShareParam(Buffer.from(zlibTrap).toString("base64url")), zlibTrap);

// Links from before this codec existed: plain base64url of the UTF-8 bytes.
assert.equal(await decodeShareParam("cHJpbnQoImhpIikK"), 'print("hi")\n');
// Every padding residue (length mod 4 of 2 and 3 as well as 0).
assert.equal(await decodeShareParam("YQ"), "a");
assert.equal(await decodeShareParam("YWI"), "ab");
assert.equal(await decodeShareParam("YWJj"), "abc");

// Nothing to seed with.
assert.equal(await decodeShareParam(null), null);
assert.equal(await decodeShareParam(""), null);

// Unreadable payloads reject rather than yield something.
await assert.rejects(decodeShareParam("!!!!"));
const gz = await encodeShareParam(example("games/samegame.cul"));
await assert.rejects(decodeShareParam(gz.slice(0, 40)), "a truncated gzip stream");
await assert.rejects(decodeShareParam("H4sIAAAA" + "AAAA".repeat(4)), "gzip magic over garbage");
await assert.rejects(decodeShareParam("A".repeat((1 << 20) + 4)), "over the size bound");

// A gzip bomb: inside the encoded bound, hundreds of megabytes once inflated.
// The decoder has to give up while inflating rather than after, so the test is
// that this returns at all — it used to buffer the whole expansion first.
const bomb = await encodeShareParam("\0".repeat(64 << 20));
assert.ok(bomb.length < (1 << 20), `a 64 MB run of zeros encodes to ${bomb.length} chars`);
await assert.rejects(decodeShareParam(bomb), "expands past the source bound");

// And the cap does not catch a program anyone would actually write: every
// example in the catalogue has to survive the round trip, the largest
// included.
for (const name of ["games/samegame.cul", "games/rocci-bird.cul"]) {
  await roundTrip(example(name));
}

console.log("share-link: ok");
