// Labeled break/continue: naming the loop (or switch) to leave or re-test,
// across any number of nested loops and switches in between.

let seen = [];
outer:
for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    if (j === 1) continue outer;
    if (i === 2) break outer;
    seen.push([i, j]);
  }
  seen.push('after inner ' + i);
}
show('nested for, labeled break/continue', seen);

let log = [];
loop1: while (true) {
  loop2: while (true) {
    log.push('enter2');
    break loop1;
  }
  log.push('unreachable');
}
show('break from an inner loop straight past an outer one', log);

let sres = [];
sw: switch (2) {
  case 1:
    sres.push('one');
    break sw;
  case 2:
    sres.push('two');
    break sw;
  case 3:
    sres.push('unreachable');
}
show('a labeled switch is a break target', sres);

// continue/break through a switch sitting between the statement and its
// labeled loop target -- the switch is itself a loop_ctx frame the depth
// has to cross, unlike unlabeled continue's own flag-forwarding path.
let through = [];
top:
for (let i = 0; i < 4; i++) {
  switch (i) {
    case 1:
      continue top;
    case 2:
      break top;
  }
  through.push(i);
}
show('labeled continue/break reaching past a switch', through);

// Multiple labels on the same loop; either one reaches it.
let multi = [];
a: b: for (let i = 0; i < 3; i++) {
  if (i === 0) { continue b; }
  if (i === 2) { break a; }
  multi.push(i);
}
show('two labels on one loop', multi);

// A labeled for-of / for-in, and continuing an outer for-of from inside a
// nested for-in.
let pairs = [];
outer2:
for (const k of ['x', 'y']) {
  for (const p in { a: 1, b: 2 }) {
    if (p === 'b') continue outer2;
    pairs.push(k + p);
  }
}
show('labeled for-of/for-in', pairs);

// do-while can be labeled too.
let n = 0;
let steps = [];
retry:
do {
  n = n + 1;
  if (n === 2) continue retry;
  steps.push(n);
} while (n < 3);
show('labeled do-while', steps);
