// generators
function* count(n) {
  for (let i = 1; i <= n; i++) yield i;
  return 'done';
}
const g = count(2);
show('protocol', [g.next(), g.next(), g.next(), g.next()]);
show('spread', [...count(4)]);
show('for-of', (function () { let s = 0; for (const v of count(3)) s += v; return s; })());
show('destructure', (function () { const [a, , c] = count(5); return [a, c]; })());
show('typeof', [typeof count, typeof g, typeof g.next, g[Symbol.iterator]() === g]);

function* echo() {
  let received = [];
  while (true) {
    const v = yield received.length;
    if (v === undefined) break;
    received.push(v);
  }
  return received;
}
const e = echo();
e.next('ignored');
show('send', [e.next('a'), e.next('b'), e.next()]);

function* inner() { yield 1; yield 2; return 'inner result'; }
function* outer() {
  const r = yield* inner();
  yield r;
  yield* [10, 20];
  yield* 'ab';
}
show('delegation', [...outer()]);

function* withFinally() {
  try {
    yield 1;
    yield 2;
  } finally {
    log.push('cleanup');
  }
}
let log = [];
const wf = withFinally();
wf.next();
show('return', [wf.return('early'), wf.next(), log]);
for (const v of withFinally()) { if (v === 1) break; }
show('break runs finally', log);

function* thrower() {
  try {
    yield 'first';
  } catch (err) {
    yield 'caught ' + err;
  }
  yield 'after';
}
const t = thrower();
show('throw', [t.next().value, t.throw('boom').value, t.next().value, t.next().done]);
show('throw uncaught', thrown(() => { const t2 = thrower(); t2.next(); t2.next(); t2.throw(new RangeError('x')); }));

const obj = {
  *pairs() { yield ['a', 1]; yield ['b', 2]; },
  *[Symbol.iterator]() { yield* this.pairs(); },
};
show('object generator method', [Object.fromEntriesLike ? 0 : [...obj].length, [...obj.pairs()][1]]);

class Tree {
  constructor(v, kids = []) { this.v = v; this.kids = kids; }
  *walk() {
    yield this.v;
    for (const k of this.kids) yield* k.walk();
  }
  [Symbol.iterator]() { return this.walk(); }
}
const tree = new Tree(1, [new Tree(2, [new Tree(3)]), new Tree(4)]);
show('class generator method', [...tree]);
show('lazy', (function () {
  let produced = 0;
  function* nat() { let i = 0; while (true) { produced++; yield i++; } }
  const it = nat();
  it.next(); it.next();
  return produced;
})());
show('generator expression', [...(function* () { yield 'x'; })()]);
show('Array.from', Array.from(count(3), x => x * x));
show('done after return', (function () { const c = count(1); c.next(); c.next(); return c.next(); })());

let dflt_runs = 0;
function nextTag() { dflt_runs++; return 'tag' + dflt_runs; }
function* tagged(tag = nextTag(), n = 2) { while (n > 0) { yield tag; n--; } }
show('default once', [[...tagged()], [...tagged('given')], dflt_runs, tagged.length]);
