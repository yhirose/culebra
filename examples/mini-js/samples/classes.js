// classes: constructors, inheritance, super, statics, accessors, fields
class Animal {
  legs = 4;
  static count = 0;
  constructor(name) {
    this.name = name;
    Animal.count++;
  }
  speak() { return `${this.name} makes a sound`; }
  get description() { return `${this.name} with ${this.legs} legs`; }
  set nickname(n) { this.nick = n.toUpperCase(); }
  static create(name) { return new this(name); }
  toString() { return `Animal(${this.name})`; }
}
class Dog extends Animal {
  tricks = [];
  constructor(name, breed) {
    super(name);
    this.breed = breed;
  }
  speak() { return super.speak() + ' (woof)'; }
  learn(trick) { this.tricks.push(trick); return this; }
  static create(name) { return super.create(name + '!'); }
  get description() { return super.description + ', a ' + this.breed; }
}
const a = new Animal('cat');
const d = new Dog('rex', 'lab');
show('instances', [a.name, d.name, d.breed, d.legs, d.tricks]);
show('methods', [a.speak(), d.speak()]);
show('getters', [a.description, d.description]);
a.nickname = 'kitty';
show('setter', a.nick);
show('static', [Animal.count, Dog.count, Animal.create('x') instanceof Animal, Dog.create('y') instanceof Dog, Dog.create('y').name]);
show('instanceof', [d instanceof Dog, d instanceof Animal, d instanceof Object, a instanceof Dog]);
show('constructor', [d.constructor.name, a.constructor === Animal, Object.getPrototypeOf(d) === Dog.prototype, Object.getPrototypeOf(Dog.prototype) === Animal.prototype]);
show('typeof', [typeof Animal, typeof d]);
show('own keys', [Object.keys(d), Object.keys(a)]);
show('for-in skips methods', (function () { const ks = []; for (const k in d) ks.push(k); return ks; })());
show('toString', ['' + a, String(d), `${a}`]);
show('chaining', d.learn('sit').learn('roll').tricks);
show('fields per instance', (function () { const d2 = new Dog('b', 'x'); d2.tricks.push('t'); return [d.tricks.length, d2.tricks.length]; })());

class Empty {}
class Child extends Empty {}
show('default ctors', [new Empty() instanceof Empty, new Child() instanceof Empty, Object.keys(new Child())]);
class Base { constructor(x) { this.x = x; } }
class Forward extends Base {}
show('forwarding ctor', new Forward(42).x);

const Point = class {
  constructor(x, y) { this.x = x; this.y = y; }
  ['plus'](o) { return new Point(this.x + o.x, this.y + o.y); }
  static get origin() { return new Point(0, 0); }
};
show('class expression', [Point.name, new Point(1, 2).plus(Point.origin).x, Point.origin.y]);

class MyError extends Error {
  constructor(msg, code) { super(msg); this.code = code; }
}
show('custom error', thrown(() => { throw new MyError('m', 7); }));
show('custom error props', (function () { try { throw new MyError('m', 7); } catch (e) { return [e.message, e.code, e instanceof MyError, e instanceof Error, e.name, String(e)]; } })());

class Enumerated {
  constructor() { this.own = 1; }
  m() {}
  get g() { return 2; }
  set s(v) {}
  static st() {}
}
Enumerated.prototype.added = 'by hand';
show('what enumerates', [Object.keys(new Enumerated()), Object.keys(Enumerated.prototype), Object.keys(Enumerated)]);
show('for-in reaches the prototype', (function () { const ks = []; for (const k in new Enumerated()) ks.push(k); return ks; })());
show('a function object', (function () { function f(a, b) {} f.tag = 1; return [Object.keys(f), f.name, f.length, f.hasOwnProperty('name')]; })());
show('an accessor pair lists once', Object.keys({ get v() { return 1; }, set v(x) {}, set w(x) {} }));

class Numbered { 1() { return 'method'; } static 2() { return 'static'; } get 3() { return 'getter'; } 4 = 'field'; }
show('number keys', (function () { const n = new Numbered(); return [n[1](), Numbered[2](), n[3], n[4], Object.keys(n)]; })());
show('a deleted member enumerates again', (function () { class D { m() {} } delete D.prototype.m; D.prototype.m = 1; return Object.keys(D.prototype); })());

const ck2 = 'made';
class ByKey { [ck2]() {} static [ck2 + 'S']() {} *[ck2 + 'Y']() {} async [ck2 + 'A']() {} }
show('a computed key names its member', [ByKey.prototype[ck2].name, ByKey[ck2 + 'S'].name, ByKey.prototype[ck2 + 'Y'].name, ByKey.prototype[ck2 + 'A'].name]);
