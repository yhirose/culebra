// throw, try/catch/finally, the Error classes, runtime TypeErrors
show('throw value', thrown(() => { throw 42; }));
show('throw error', thrown(() => { throw new Error('boom'); }));
show('type error', thrown(() => { throw new TypeError('bad'); }));
show('range error', thrown(() => { throw new RangeError('out'); }));

let e1;
try { throw new Error('msg'); } catch (e) { e1 = e; }
show('caught', [e1.message, e1.name, e1 instanceof Error, e1 instanceof TypeError, String(e1)]);

let e2;
try { null.x; } catch (e) { e2 = e; }
show('null access', [e2 instanceof TypeError, e2.constructor.name]);
show('undefined call', thrown(() => undefined()));
show('not a function', thrown(() => ({}).nope()));
show('undefined prop read', thrown(() => undefined.x));
show('undefined var', thrown(() => notDefined));
show('typeof undefined var', typeof notDefined);
show('new non-ctor', thrown(() => new 3()));

let log = [];
function withFinally(fail) {
  try {
    log.push('try');
    if (fail) throw new Error('x');
    return 'returned';
  } catch (e) {
    log.push('catch');
    return 'caught';
  } finally {
    log.push('finally');
  }
}
show('finally', [withFinally(false), withFinally(true), log]);

function nested() {
  try {
    try {
      throw new Error('inner');
    } finally {
      log.push('inner finally');
    }
  } catch (e) {
    return 'outer got ' + e.message;
  }
}
show('nested finally', nested());

show('rethrow', thrown(() => { try { throw new RangeError('r'); } catch (e) { throw e; } }));
show('catch no binding', (function () { try { throw 1; } catch { return 'ok'; } })());
show('error props', (function () { const e = new Error('m'); e.code = 7; return [e.code, Object.keys(e)]; })());
show('custom name', (function () { const e = new Error('m'); e.name = 'Custom'; return String(e); })());
show('no message', [String(new Error()), new TypeError().message === '']);
show('throw in callback', thrown(() => [1].map(x => { throw new TypeError('cb'); })));
show('deep recursion', thrown(() => { function r(n) { return r(n + 1) + 1; } return r(0); }));
show('finally overrides', (function () { try { return 'try'; } finally { log.push('f'); } })());
show('error in finally path', log.length);
