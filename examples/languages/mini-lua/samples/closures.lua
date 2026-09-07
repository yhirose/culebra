-- Closures and multiple return values -- the second of which is the
-- calling convention this front end writes over the IR's, since a function
-- here answers one value and a Lua function answers any number.

local function counter()
  local n = 0
  return function() n = n + 1; return n end
end
local c1, c2 = counter(), counter()
print(c1(), c1(), c2(), c1())

-- Multiple returns, and the rule that only the last expression in a list
-- expands to all of them.
local function two() return 1, 2 end
local a, b = two()
print(a, b)
print(two())
print(two(), 9)
print(9, two())
local t = {two()}
print(#t, t[1], t[2])

-- Extra values are dropped and missing ones are nil, which is Lua's arity
-- rule and Func::lenient_arity's.
local function one(x) return x end
print(one(), one(1, 2, 3))

-- Parentheses truncate, deliberately.
print((two()))

-- Each iteration of a numeric `for` gets its own binding, so three
-- closures made in the loop remember three different numbers. That is
-- CellFresh, once per iteration.
local fs = {}
for i = 1, 3 do fs[i] = function() return i end end
print(fs[1](), fs[2](), fs[3]())

-- An upvalue shared between two closures is one cell, not two copies.
local function pair()
  local v = 0
  return function() v = v + 1 end, function() return v end
end
local bump, read = pair()
bump(); bump()
print(read())

-- Two levels of nesting: the middle function carries `x` for the inner one.
local function outer(x)
  return function(y) return function(z) return x + y + z end end
end
print(outer(1)(2)(3))

-- A local function sees its own name, which is what makes it recurse.
local function fact(n) if n < 2 then return 1 end return n * fact(n - 1) end
print(fact(10))

-- Swapping through a multiple assignment.
local p, q = 1, 2
p, q = q, p
print(p, q)
