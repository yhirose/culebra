// objects: literals, prototypes, keys, in/delete, spread
const x = 1, y = 2;
const o = { x, y, ['k' + x]: 'computed', 'quoted key': true, 3: 'three', nested: { deep: [1, { z: 0 }] } };
show('literal', o);
show('keys', Object.keys(o));
show('values', Object.values({ a: 1, b: 'two' }));
show('entries', Object.entries({ a: 1 }));
show('missing', o.nope);
show('missing nested', thrown(() => o.nope.deeper));
show('in', ['x' in o, 'nope' in o, 3 in o, 'toString' in o]);
o.added = 'later';
delete o.x;
show('mutation', Object.keys(o));
show('hasOwn', [o.hasOwnProperty('y'), o.hasOwnProperty('toString')]);

const base = { kind: 'base', describe() { return this.kind + '/' + this.extra; } };
const derived = Object.create(base);
derived.extra = 'more';
show('proto chain', [derived.describe(), derived.kind, Object.getPrototypeOf(derived) === base]);
derived.kind = 'own';
show('shadowing', [derived.describe(), base.kind]);

const merged = Object.assign({}, { a: 1 }, { b: 2 }, { a: 3 });
show('assign', merged);
const spread = { ...merged, c: 4, a: 0 };
show('spread', spread);

const counter = { n: 0, bump() { this.n++; return this; } };
show('chain', counter.bump().bump().n);

const dyn = {};
dyn['a b'] = 1;
dyn[42] = 'num key';
dyn[true] = 'bool key';
show('dynamic keys', [Object.keys(dyn), dyn['42'], dyn.true]);

show('equality', [{} === {}, o === o, { a: 1 } == { a: 1 }]);
show('typeof', [typeof o, typeof null, typeof o.nested.deep]);
show('to string', ['' + { a: 1 }, String([1, [2, 3]]), [] + [], {} + '']);

function Point(px, py) { this.px = px; this.py = py; }
Point.prototype.len = function () { return Math.sqrt(this.px * this.px + this.py * this.py); };
Point.origin = function () { return new Point(0, 0); };
const p = new Point(3, 4);
show('constructor', [p.len(), p instanceof Point, p instanceof Object, Point.origin().px, p.constructor === Point]);
show('proto props', [Object.keys(p), typeof p.len, p.hasOwnProperty('len')]);

const getterish = { get() { return 'g'; } };
show('method named get', getterish.get());

const ck = 'dyn';
const computed = {
  get [ck]() { return this.v; },
  set [ck](x) { this.v = x * 2; },
  [ck + 'M']() { var inner = 'own'; return inner; },
  'two words'() { return 'quoted method'; },
  *[ck + 'G']() { yield 1; },
};
computed.dyn = 4;
show('computed members', [computed.dyn, computed.dynM(), computed['two words'](), [...computed.dynG()]]);
show('computed method scope', thrown(() => inner));
show('assign many', Object.assign({ z: 0 }, { a: 1 }, null, { b: 2 }, { c: 3 }, { d: 4 }));
