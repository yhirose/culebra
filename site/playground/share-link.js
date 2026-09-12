// The share link's payload: the editor's source as base64url in the URL
// fragment, gzip-compressed when that comes out shorter.
//
// Self-describing rather than flagged: gzip's magic (0x1f 0x8b) cannot begin
// a culebra source — 0x1f is a control character — so the first two bytes say
// which form this is, and no `compress=` parameter has to travel with the
// payload and stay in agreement with it. zlib framing would save the 18-byte
// gzip envelope and was rejected: its header check is `(CMF*256+FLG) % 31 ==
// 0`, which `x ` (0x78 0x20) satisfies, so a source starting `x = 1` would be
// taken for a compressed stream.
//
// No DOM and no imports, so node can load this for the round-trip test in
// tools/playground/ and the encoder and decoder are held to one contract.

// base64url: `-` `_` for `+` `/`, no padding. The page reads the fragment
// through URLSearchParams, which turns `+` into a space — this alphabet is
// what lets the payload through it intact.
function toBase64url(bytes) {
  let bin = "";
  // Not String.fromCharCode.apply: a whole program overflows the argument list.
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function fromBase64url(text) {
  const b64 = text.replace(/-/g, "+").replace(/_/g, "/");
  const padded = b64 + "=".repeat((4 - (b64.length % 4)) % 4);
  return Uint8Array.from(atob(padded), (c) => c.charCodeAt(0));
}

async function pipe(bytes, transform) {
  const stream = new Blob([bytes]).stream().pipeThrough(transform);
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// Inflating, which is the direction that takes a stranger's bytes. Read in
// chunks and stop at the cap rather than handing the whole stream to
// Response: gzip reaches about 1000:1, so a payload inside MAX_PARAM_CHARS
// can ask for hundreds of megabytes, and a link should not be able to take
// the tab down before the program it carries has even been looked at.
async function inflate(bytes) {
  const reader = new Blob([bytes]).stream()
    .pipeThrough(new DecompressionStream("gzip")).getReader();
  const parts = [];
  let size = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    size += value.length;
    if (size > MAX_SOURCE_BYTES) {
      await reader.cancel();
      throw new RangeError(`share payload expands past ${MAX_SOURCE_BYTES >> 20} MB`);
    }
    parts.push(value);
  }
  const out = new Uint8Array(size);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

function isGzip(bytes) {
  return bytes[0] === 0x1f && bytes[1] === 0x8b;
}

// The fragment is untrusted input, capped at both ends: the encoded payload
// before it is read, and the source it expands into. The largest example
// ships well under 8 KB encoded, and no program anyone writes by hand comes
// near either limit.
const MAX_PARAM_CHARS = 1 << 20;
const MAX_SOURCE_BYTES = 4 << 20;

// Whichever form is shorter. gzip loses on a snippet, where its envelope is
// most of the payload (73-byte greeting.cul: 98 chars plain, 102 gzipped), and
// wins by about 3x on a whole program.
export async function encodeShareParam(source) {
  const raw = new TextEncoder().encode(source);
  const plain = toBase64url(raw);
  if (typeof CompressionStream !== "function") return plain;
  const packed = toBase64url(await pipe(raw, new CompressionStream("gzip")));
  return packed.length < plain.length ? packed : plain;
}

// Throws on anything that is not a payload this module produced (or a plain
// base64url one from before it existed); the caller decides what an unreadable
// link falls back to.
export async function decodeShareParam(param) {
  if (!param) return null;
  if (param.length > MAX_PARAM_CHARS) throw new RangeError("share payload too large");
  let bytes = fromBase64url(param);
  if (isGzip(bytes)) bytes = await inflate(bytes);
  // atob gave one char per byte; the source is UTF-8, so decode it as such.
  return new TextDecoder().decode(bytes);
}
