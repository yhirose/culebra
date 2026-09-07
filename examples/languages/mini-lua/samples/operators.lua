-- The metamethods examples/mini-lua/samples/metatables.lua does not
-- reach: `__add`, `__sub`, `__mul`, `__eq`, `__tostring` and `__call`,
-- plus the real generic-`for` protocol (an iterator function, a state
-- and a control value), which `ipairs`/`pairs` are two fast-path callers
-- of rather than the whole of what `for ... in` means.

local Vec = {}
Vec.__index = Vec

function Vec.new(x, y)
  return setmetatable({x = x, y = y}, Vec)
end

Vec.__add = function(a, b) return Vec.new(a.x + b.x, a.y + b.y) end
Vec.__sub = function(a, b) return Vec.new(a.x - b.x, a.y - b.y) end
Vec.__mul = function(a, k) return Vec.new(a.x * k, a.y * k) end
Vec.__eq = function(a, b) return a.x == b.x and a.y == b.y end
Vec.__tostring = function(v) return "(" .. v.x .. ", " .. v.y .. ")" end

local a = Vec.new(1, 2)
local b = Vec.new(3, 4)
print(tostring(a + b))
print(tostring(a - b))
print(tostring(a * 3))
print(a == Vec.new(1, 2), a == b)
print(tostring(a))
print(a)

-- A callable table: `__call` makes the table itself invokable.
local Adder = setmetatable({}, {
  __call = function(self, x, y) return x + y + self.bias end
})
Adder.bias = 10
print(Adder(1, 2))

local counter = setmetatable({n = 0}, {
  __call = function(self) self.n = self.n + 1; return self.n end
})
print(counter(), counter(), counter())

-- __newindex: a table that logs writes to keys it does not already have.
local log = {}
local watched = setmetatable({}, {
  __newindex = function(t, k, v) log[#log + 1] = k .. "=" .. tostring(v); rawset(t, k, v) end
})
watched.a = 1
watched.b = 2
watched.a = 99  -- already present after the first write: no log entry
print(table.concat(log, ","))
print(watched.a, watched.b)

-- The real generic-for protocol: a stateless iterator function.
local function range(limit, i)
  i = i + 1
  if i > limit then return nil end
  return i
end

local function iter_range(n)
  return range, n, 0
end

local total = 0
for i in iter_range(5) do
  total = total + i
end
print(total)

-- A custom iterator that hands back two values, exactly like `pairs`.
local function enumerate(t)
  local i = 0
  local function step(state, _)
    i = i + 1
    if i > #state then return nil end
    return i, state[i]
  end
  return step, t, nil
end

local out = {}
for idx, v in enumerate({"a", "b", "c"}) do
  out[#out + 1] = idx .. ":" .. v
end
print(table.concat(out, " "))
