// strings
const s = 'Hello, World';
show('basics', [s.length, s[0], s.charAt(7), s.charAt(99), s.charCodeAt(0), String.fromCharCode(97)]);
show('search', [s.indexOf('o'), s.indexOf('o', 5), s.indexOf('z'), s.includes('World'), s.startsWith('Hell'), s.endsWith('d')]);
show('slice', [s.slice(7), s.slice(-5), s.slice(0, 5), s.substring(7, 0), s.at(-1)]);
show('case', [s.toUpperCase(), s.toLowerCase()]);
show('split', [s.split(', '), 'a,b,,c'.split(','), 'abc'.split(''), 'abc'.split()]);
show('trim/pad', ['  x '.trim() + '|', '5'.padStart(3, '0'), 'ab'.padEnd(5, '.') + '|']);
show('repeat/replace', ['ab'.repeat(3), 'a-b-c'.replace('-', '+'), 'aaa'.replace('a', 'b')]);
show('concat', ['a'.concat('b'), 'x' + 1 + true + null + undefined, `${1}${2}`]);
show('escapes', ['tab\there', 'quote\'s', "dq\"s", 'back\\slash', 'nl\n2'.split('\n')]);
show('compare', ['a' < 'b', 'B' < 'a', '10' < '9', 'abc' === 'abc', 'abc' == 'abd']);
show('template', (function () { const name = 'T'; const n = 2; return `${name} has ${n * 2} items ${n > 1 ? 's' : ''}`; })());
show('multiline template', `line1
line2`);
show('template expr', `${[1, 2].map(x => x + 1)} and ${{ a: 1 }}`);
show('to string', [String(null), String(undefined), String(12.5), (12).toString(), String([1, 2]), String({})]);
show('number strings', ['3' * '4', '3' + 4, +'3.5', -'2', +'', +' 12 ', +'1e3', +'0x10', +'abc' !== +'abc']);
show('char loop', (function () { let r = ''; for (let i = s.length - 1; i >= 0; i--) r += s[i]; return r; })());
show('immutable', (function () { const t = 'abc'; t[0] = 'z'; return t; })());
show('concat many', 'a'.concat('b', 1, true, null, 'e'));
