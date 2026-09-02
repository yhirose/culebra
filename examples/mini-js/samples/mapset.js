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
