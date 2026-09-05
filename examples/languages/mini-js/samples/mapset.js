// Map, Set, iteration protocol via Symbol.iterator
const m = new Map([['a', 1], ['b', 2]]);
m.set('c', 3).set('a', 10);
show('map', [m.size, m.get('a'), m.get('zzz'), m.has('b'), m.delete('b'), m.delete('b'), m.size]);
const keyObj = {};
m.set(keyObj, 'object key').set(NaN, 'nan').set(1, 'one').set('1', 'string one');
show('map keys', [m.get(keyObj), m.get({}), m.get(NaN), m.get(1), m.get('1')]);
show('map iteration', [[...m.keys()].length, [...m.values()].slice(0, 2), Array.from(m.entries())[0]]);
let pairs = [];
for (const [k, v] of m) if (typeof k === 'string') pairs.push(k + '=' + v);
show('for-of map', pairs);
let seen = [];
m.forEach((v, k) => { if (typeof k === 'number') seen.push(v); });
show('forEach', seen);
m.clear();
show('clear', m.size);

const s = new Set([3, 1, 3, 2, 1]);
show('set', [s.size, [...s], s.has(2), s.has(9), s.add(4).has(4), s.delete(1), [...s]]);
show('set dedupe', [...new Set('mississippi')].join(''));
const union = new Set([...new Set([1, 2]), ...new Set([2, 3])]);
show('union', [...union]);
show('set of objects', (function () { const o = {}; const t = new Set([o, o, {}]); return t.size; })());
show('to string', [String(new Map()), '' + new Set()]);

class Range {
  constructor(lo, hi) { this.lo = lo; this.hi = hi; }
  [Symbol.iterator]() {
    let cur = this.lo;
    const hi = this.hi;
    return { next: () => cur <= hi ? { value: cur++, done: false } : { value: undefined, done: true } };
  }
}
show('custom iterable', [[...new Range(1, 4)], Array.from(new Range(2, 3), x => x * 10)]);
let total = 0;
for (const n of new Range(1, 3)) total += n;
show('for-of iterable', total);
show('destructure iterable', (function () { const [x, y] = new Range(7, 9); return x + y; })());
const iterableObj = { [Symbol.iterator]() { let i = 0; return { next: () => ({ value: i, done: i++ >= 2 }) }; } };
show('object iterable', [...iterableObj]);
show('not iterable', thrown(() => [...{}]));
show('map from object', new Map(Object.entries({ p: 1, q: 2 })).get('q'));
show('built-ins do not enumerate', [Object.keys(Math), Object.keys(Symbol), Object.keys(Map.prototype), Object.keys(Boolean.prototype), (function () { const ks = []; for (const k in new Map()) ks.push(k); return ks; })()]);
show('nor does an iterator of theirs', [Object.keys(new Map([['a', 1]]).entries()), Object.keys(new Set([1]).values())]);
show('but console is the host, not the language', ['log', 'warn', 'error', 'info'].map(k => Object.keys(console).indexOf(k) >= 0));

// being a Map is a slot the constructor fills, not something the prototype says
show('a foreign receiver', [thrown(() => Object.create(Map.prototype).size), thrown(() => Map.prototype.get.call({}, 'a')), thrown(() => Set.prototype.has.call({}, 1)), thrown(() => Map.prototype.forEach.call([], () => {}))]);
show('and how it reads', (function () { try { Map.prototype.get.call({}, 'a'); return 'no throw'; } catch (e) { return e.message; } })());

// keys of every kind, and the positions they keep as members come and go
const ok1 = {};
const ok2 = {};
const ak = [1, 2];
const mk = new Map();
mk.set(ok1, 'a');
mk.set(ok2, 'b');
mk.set(ak, 'c');
mk.set([1, 2], 'd');
mk.set(NaN, 'n');
mk.set(-0, 'z');
mk.set('1', 's');
mk.set(1, 'num');
show('every key stands for itself', [mk.size, mk.get(ok1), mk.get(ok2), mk.get(ak), mk.get([1, 2]), mk.get(NaN), mk.get(0), mk.get('1'), mk.get(1)]);
mk.delete(ok1);
mk.delete(NaN);
show('and the rest keep their places', [mk.size, mk.get(ok2), mk.get(ak), mk.get(1), [...mk.values()]]);
show('a key is not marked by being one', [Object.keys(ok1), JSON.stringify(ok2), Object.keys(ak).length]);
const gap = new Set([1, 2, 3, 4]);
gap.delete(2);
gap.add(5);
gap.add(3);
show('a set closes over the gap', [[...gap], gap.has(3), gap.has(2), gap.size]);
