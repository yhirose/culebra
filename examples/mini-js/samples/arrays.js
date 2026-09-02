// arrays and their methods
const a = [3, 1, 2];
show('literal', [a, a.length, a[0], a[5], a[-1]]);
a.push(4, 5);
show('push', [a, a.pop(), a]);
show('shift/unshift', [a.shift(), a.unshift(0), a]);
show('map/filter', [a.map(x => x * 2), a.filter(x => x % 2)]);
show('reduce', [a.reduce((s, x) => s + x, 0), a.reduce((s, x) => s + x), thrown(() => [].reduce((s, x) => s))]);
show('find', [a.find(x => x > 1), a.findIndex(x => x > 1), a.find(x => x > 100)]);
show('indexOf', [a.indexOf(2), a.indexOf(9), a.includes(2), [NaN].includes(NaN), [NaN].indexOf(NaN)]);
show('join', [a.join(), a.join('-'), [1, [2, 3]].join(';'), [null, undefined, 1].join()]);
show('slice', [a.slice(1), a.slice(1, 2), a.slice(-2), a.slice(10)]);
show('concat', [[1].concat([2, 3], 4), [1, 2].concat()]);
show('reverse', [[1, 2, 3].reverse()]);
show('sort', [[3, 1, 10, 2].sort(), [3, 1, 10, 2].sort((p, q) => p - q), ['b', 'a', 'c'].sort()]);
show('splice', (function () { const s = [1, 2, 3, 4, 5]; const r = s.splice(1, 2, 'x'); return [s, r]; })());
show('some/every', [a.some(x => x > 2), a.every(x => x > 0), [].every(x => false), [].some(x => true)]);
show('forEach', (function () { let acc = []; ['p', 'q'].forEach((v, i) => acc.push(i + v)); return acc; })());
show('isArray', [Array.isArray([]), Array.isArray({}), Array.isArray('s')]);
show('Array()', [Array(3).length, Array.of(7), Array.from('ab'), Array.from([1, 2], x => x * 3)]);
show('length write', (function () { const s = [1, 2, 3]; s.length = 1; s[3] = 'gap'; return [s.length, s[2], s[3]]; })());
show('sparse write', (function () { const s = []; s[2] = 'c'; return [s.length, s[0] === undefined, s[2]]; })());
show('nested', [[1, [2, [3]]].flat(), [[1, 2], [3]].map(r => r.length)]);
show('spread', [[...[1, 2], ...'ab', 3], Math.max(...[4, 9, 2])]);
show('fill', [new Array(3).fill(0), [1, 2].fill('z')]);
show('destructure via index', (function () { const s = [10, 20]; const p = s[0], q = s[1]; return p + q; })());
show('equality', [[1] === [1], a === a, [1, 2] == '1,2']);
show('array in bool', [!![], [] == false, [0] == false]);
show('string methods on arrays', thrown(() => a.toUpperCase()));

const decoy = { src: 'not an array', i: 0, next() { this.i++; return this.i > 2 ? { done: true } : { value: this.i, done: false }; } };
show('own iterator named src', [...{ [Symbol.iterator]() { return decoy; } }]);

show('push many', (function () { const s = []; const n = s.push(1, 2, 3, 4, 5, 6, 7, 8); return [n, s]; })());
show('unshift many', (function () { const s = [9]; const n = s.unshift(1, 2, 3); return [n, s]; })());
show('concat many', [1].concat([2], 3, [4], [5], 6, [7]));
show('splice many', (function () { const s = [1, 2, 3]; const cut = s.splice(1, 1, 'a', 'b', 'c', 'd'); return [cut, s]; })());
show('Array many', [Array(1, 2, 3), Array('one'), Array(3).length]);
show('Math.max many', [Math.max(1, 9, 3, 8, 2, 7), Math.min(4, 2, 6, 1, 5, 3)]);

show('for-in reaches Array.prototype', (function () {
  const s = [7, 8];
  Array.prototype.injected = 'p';
  const withIt = [];
  for (const k in s) withIt.push(k);
  delete Array.prototype.injected;
  const without = [];
  for (const k in s) without.push(k);
  return [withIt, without, Object.keys(s)];
})());
