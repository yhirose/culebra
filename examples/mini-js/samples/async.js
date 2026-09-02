// Promise and async/await: settlement, chaining, ordering on the job queue
const log = [];
const tick = (label) => log.push(label);

tick('sync 1');
Promise.resolve('r').then(v => tick('then ' + v));
queueMicrotask(() => tick('microtask'));
(async () => {
  tick('async start');
  await null;
  tick('after await');
  await new Promise(res => res());
  tick('after await 2');
})();
tick('sync 2');
new Promise((res, rej) => { tick('executor'); res(1); rej(2); res(3); })
  .then(v => { tick('chain ' + v); return v + 1; })
  .then(v => { tick('chain ' + v); throw new RangeError('stop'); })
  .then(() => tick('skipped'))
  .catch(e => { tick('caught ' + e.constructor.name); return 'recovered'; })
  .finally(() => tick('finally'))
  .then(v => tick('after finally ' + v));

async function add(a, b) { return a + (await b); }
add(1, Promise.resolve(2)).then(v => tick('add ' + v));

async function failing() { throw new TypeError('bad'); }
failing().catch(e => tick('rejected async ' + e.constructor.name));

async function tryCatch() {
  try {
    await Promise.reject(new Error('inner'));
  } catch (e) {
    tick('try/catch ' + e.message);
    return 'handled';
  } finally {
    tick('async finally');
  }
}
tryCatch().then(v => tick('tryCatch -> ' + v));

const all = Promise.all([1, Promise.resolve(2), new Promise(r => queueMicrotask(() => r(3)))]);
all.then(vs => tick('all ' + vs.join(',')));
Promise.all([Promise.reject(new Error('x')), 1]).catch(e => tick('all rejected ' + e.message));
Promise.race([new Promise(() => {}), Promise.resolve('fast')]).then(v => tick('race ' + v));
Promise.allSettled([Promise.resolve(1), Promise.reject('no')]).then(rs => tick('settled ' + rs.map(r => r.status).join('/')));

class Api {
  static async fetch(n) { await null; return n * 2; }
  async sum(...xs) { let s = 0; for (const x of xs) s += await Api.fetch(x); return s; }
}
new Api().sum(1, 2, 3).then(v => tick('class async ' + v));

const thenable = { then(res) { res('thenable value'); } };
Promise.resolve(thenable).then(v => tick(v));
(async () => tick('await thenable ' + (await thenable)))();

const p = Promise.resolve(1);
tick('types ' + [typeof p.then, p instanceof Promise, Promise.resolve(p) === p, String(p), typeof Promise].join(' '));

// the queue drains after the script: every line above lands before this prints
Promise.resolve().then(() => {}).then(() => {}).then(() => {}).then(() => {}).then(() => {}).then(() => {
  console.log(log.join('\n'));
});
