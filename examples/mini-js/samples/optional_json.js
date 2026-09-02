// optional chaining, accessors, JSON
const deep = { a: { b: { c: 1, f() { return this.c; } } }, arr: [1, 2] };
show('optional', [deep?.a?.b?.c, deep.x?.y?.z, deep?.arr?.[1], deep.nope?.[0], deep.a.b.f?.(), deep.missing?.()]);
show('optional on null', [null?.x, undefined?.[0], (null)?.()]);
show('optional short-circuits', (function () { let calls = 0; const r = undefined?.[calls++]; return [r, calls]; })());

const temp = {
  celsius: 25,
  get fahrenheit() { return this.celsius * 9 / 5 + 32; },
  set fahrenheit(f) { this.celsius = (f - 32) * 5 / 9; },
  get ro() { return 'read only'; },
};
show('getter', temp.fahrenheit);
temp.fahrenheit = 212;
show('setter', temp.celsius);
temp.ro = 'x';
show('getter without setter', temp.ro);
show('accessor keys', Object.keys(temp));
show('accessor in', 'fahrenheit' in temp);
const child = Object.create(temp);
child.celsius = 0;
show('inherited accessor', child.fahrenheit);
child.fahrenheit = 32;
show('inherited setter writes receiver', [child.celsius, temp.celsius, child.hasOwnProperty('celsius')]);

const data = { name: 'n', n: 1.5, ok: true, nothing: null, list: [1, 'two', null, undefined], nested: { deep: [] }, f() {}, u: undefined };
show('stringify', JSON.stringify(data));
show('stringify indent', JSON.stringify({ a: [1, { b: 2 }], c: {} }, null, 2));
show('stringify scalars', [JSON.stringify('s"q\n'), JSON.stringify(NaN), JSON.stringify(undefined), JSON.stringify([undefined, () => 1]), JSON.stringify(null)]);
const parsed = JSON.parse('{"a": [1, 2.5, -3e2, "x\\ny", true, null], "b": {"c": {}}, "d": "\\u0041"}');
show('parse', parsed);
show('parse types', [typeof parsed.a[0], parsed.a[5] === null, parsed.d]);
show('roundtrip', JSON.stringify(JSON.parse(JSON.stringify(data))));
show('parse error', thrown(() => JSON.parse('{bad')));
show('parse error 2', thrown(() => JSON.parse('[1,]')));
show('toJSON', JSON.stringify({ toJSON() { return 'custom'; } }));
