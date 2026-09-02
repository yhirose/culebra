// The one formatting function the samples print through, so that Node and
// mini-js compare on the language rather than on console.log's own
// rendering of objects. Numbers go through String(n) (ES Number::toString),
// strings are quoted inside containers, and objects print their own keys
// in insertion order.
function fmt(v) {
  if (v === null) return 'null';
  if (v === undefined) return 'undefined';
  if (typeof v === 'number' || typeof v === 'boolean') return String(v);
  if (typeof v === 'string') return v;
  if (typeof v === 'function') return '[Function' + (v.name ? ': ' + v.name : '') + ']';
  if (Array.isArray(v)) return '[' + v.map(fmtIn).join(', ') + ']';
  if (v instanceof Error) return v.name + ': ' + v.message;
  var keys = Object.keys(v);
  if (keys.length === 0) return '{}';
  return '{ ' + keys.map(function (k) { return k + ': ' + fmtIn(v[k]); }).join(', ') + ' }';
}
function fmtIn(v) {
  if (typeof v === 'string') return "'" + v + "'";
  return fmt(v);
}
function show(label, v) {
  console.log(label + ': ' + fmt(v));
}
// what a thrower throws, by constructor name (the message is Node's own wording)
function thrown(f) {
  try {
    f();
    return 'no throw';
  } catch (e) {
    if (e instanceof Error) return e.constructor.name;
    return 'threw ' + fmt(e);
  }
}
