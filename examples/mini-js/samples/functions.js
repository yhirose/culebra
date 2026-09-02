// functions: closures, arrows and this, defaults, rest, call/apply/bind
function counter() {
  let c = 0;
  return { inc: () => ++c, get: () => c };
}
const ctr = counter();
ctr.inc(); ctr.inc();
show('closure', ctr.get());

function adder(a, b = 10, c = a + b) { return [a, b, c]; }
show('defaults', [adder(1), adder(1, 2), adder(1, undefined, 5)]);

function rest(first, ...others) { return [first, others.length, others]; }
show('rest', [rest(1), rest(1, 2, 3)]);

const obj = {
  name: 'obj',
  regular() { return this.name; },
  arrow: () => typeof this,
  nested() { return [1].map(() => this.name); },
};
show('this', [obj.regular(), obj.arrow(), obj.nested()]);

const detached = obj.regular;
show('detached this', thrown(() => detached()));

function greet(greeting, punct) { return greeting + ', ' + this.who + punct; }
show('call', greet.call({ who: 'call' }, 'hi', '!'));
show('apply', greet.apply({ who: 'apply' }, ['yo', '?']));
const bound = greet.bind({ who: 'bound' }, 'hey');
show('bind', bound('.'));

show('iife', (function (x) { return x * 2; })(21));
show('arrow iife', ((x) => x + 1)(1));
show('fn length/name', [adder.name, (function () {}).name, ((a, b) => 0).length === undefined]);

function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
show('recursion', fib(15));

const compose = (f, g) => x => f(g(x));
show('compose', compose(x => x + 1, x => x * 2)(5));

let calls = [];
function tracked(x) { calls.push(x); return x; }
tracked(1) || tracked(2);
tracked(0) && tracked(3);
show('short-circuit', calls);

function outer() {
  let v = 'outer';
  function inner() { return v; }
  v = 'changed';
  return inner();
}
show('late binding', outer());

show('arity', [(function (a, b) { return arguments_len(a, b); })(1)]);
function arguments_len(a, b) { return (a === undefined ? 0 : 1) + (b === undefined ? 0 : 1); }

show('return undefined', (function () {})());
show('typeof fn', typeof (() => 1));
show('fn as value', [1, 2, 3].map(String));
