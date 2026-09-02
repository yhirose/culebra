// switch (with fall-through, default in the middle, continue through it), for-in, comma
function kind(v) {
  switch (typeof v) {
    case 'number':
      if (v > 10) return 'big';
    case 'string':
      return 'small or string';
    default:
      return 'other';
    case 'boolean':
      return 'bool';
  }
}
show('switch', [kind(50), kind(1), kind('s'), kind(true), kind(null)]);

let out = [];
for (let i = 0; i < 6; i++) {
  switch (i % 3) {
    case 0:
      out.push('zero');
      break;
    case 1:
      continue;
    default:
      out.push('two:' + i);
  }
  out.push('after' + i);
}
show('switch in loop', out);

function fall(n) {
  let log = [];
  switch (n) {
    case 1: log.push(1);
    case 2: log.push(2); break;
    case 3: log.push(3);
  }
  return log;
}
show('fall-through', [fall(1), fall(2), fall(3), fall(4)]);

switch (3) { }
show('empty switch', 'ok');
show('switch scope', (function () { switch (1) { case 1: let z = 'scoped'; return z; } })());
show('strict compare', (function () { switch ('1') { case 1: return 'number'; case '1': return 'string'; } })());

const o = { b: 2, a: 1, 10: 'ten', 2: 'two' };
let keys = [];
for (const k in o) keys.push(k);
show('for-in order', keys);
const proto = { inherited: true };
const child = Object.create(proto);
child.own = 1;
let ks2 = [];
for (const k in child) ks2.push(k);
show('for-in proto', ks2);
let idx = [];
for (const i in ['a', 'b']) idx.push(i + typeof i);
show('for-in array', idx);
let cnt = 0;
for (var k in {}) cnt++;
show('for-in empty', cnt);

let seq = (1, 2, 3);
show('comma', seq);
for (let i = 0, j = 10; i < j; i += 3, j -= 3) seq = seq + i + j;
show('comma in for', seq);
