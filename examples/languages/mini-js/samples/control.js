// control flow, with closures observing per-iteration bindings
let out = [];
for (let i = 0; i < 5; i++) {
  if (i === 1) continue;
  if (i === 4) break;
  out.push(i);
}
show('for', out);

let fs = [];
for (let i = 0; i < 3; i++) fs.push(() => i);
show('per-iteration let', fs.map(f => f()));

var gs = [];
for (var j = 0; j < 3; j++) gs.push(function () { return j; });
show('shared var', gs.map(f => f()));

let n = 0;
while (true) {
  n += 1;
  if (n > 3) break;
}
show('while', n);

let d = 0;
do { d++; } while (d < 0);
show('do once', d);

let k = 0;
do {
  k++;
  if (k < 3) continue;
  break;
} while (true);
show('do continue', k);

let sum = 0;
for (const x of [1, 2, 3, 4]) {
  if (x % 2 === 0) continue;
  sum += x;
}
show('for-of', sum);

let chars = [];
for (const c of 'abc') chars.push(c);
show('for-of string', chars);

let hs = [];
for (const v of [10, 20]) hs.push(() => v);
show('for-of closures', hs.map(h => h()));

let nested = [];
for (let a = 0; a < 3; a++) {
  for (let b = 0; b < 3; b++) {
    if (b === a) continue;
    if (a === 2) break;
    nested.push(a * 10 + b);
  }
}
show('nested', nested);

let grade = (s) => {
  if (s >= 90) return 'A';
  else if (s >= 80) return 'B';
  else return 'C';
};
show('if-else chain', [grade(95), grade(85), grade(10)]);

let blockScoped = 'outer';
{
  let blockScoped = 'inner';
  show('block inner', blockScoped);
}
show('block outer', blockScoped);

function hoisted() { return early(); }
function early() { return 'hoisted ok'; }
show('hoisting', hoisted());
show('var hoist', typeof laterVar);
var laterVar = 1;

let sw = [];
for (let i = 0; i < 4; i++) {
  sw.push(i % 2 === 0 ? 'even' : 'odd');
}
show('ternary loop', sw);

let count = 0;
let fact = function f(m) { count++; return m <= 1 ? 1 : m * f(m - 1); };
show('named fn expr', [fact(5), count]);
