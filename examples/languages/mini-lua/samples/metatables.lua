-- Metatables, and the object idiom Lua builds out of them. `__index` is
-- the one metamethod this subset has, which is the one the idiom needs.

local Point = {}
Point.__index = Point

function Point.new(x, y)
  return setmetatable({x = x, y = y}, Point)
end

function Point:norm2()
  return self.x * self.x + self.y * self.y
end

function Point:label(tag)
  return tag .. "(" .. self.x .. ", " .. self.y .. ")"
end

function Point:scaled(k)
  return Point.new(self.x * k, self.y * k)
end

local p = Point.new(3, 4)
print(p.x, p.y, p.norm2 ~= nil)
-- `p:norm2()` passes p as the first argument, which is why the method was
-- declared with `:` too -- an implicit parameter on both sides.
print(p:norm2())
print(p:label("P"))
print(p:scaled(2):norm2())
print(getmetatable(p) == Point, getmetatable({}) == nil)

-- The lookup walks to the metatable only when the key is absent from the
-- table itself, so shadowing a method is just setting a key.
local q = Point.new(1, 1)
q.norm2 = function() return "shadowed" end
print(q:norm2(), p:norm2())

-- __index as a *function*, which is called with the table and the key.
local logged = {}
local watcher = setmetatable({}, {
  __index = function(t, k) logged[#logged + 1] = k; return "made " .. k end
})
print(watcher.alpha, watcher.beta)
print(table.concat(logged, ","))

-- A chain: one metatable whose __index is a table that has a metatable of
-- its own. The lookup is recursive, which is what makes inheritance work.
local Base = {kind = "base"}
Base.__index = Base
function Base:describe() return "a " .. self.kind end
local Derived = setmetatable({}, Base)
Derived.__index = Derived
local d = setmetatable({kind = "derived"}, Derived)
print(d:describe(), d.kind)

-- Instances in a table, each answering for itself.
local ps = {}
for i = 1, 3 do ps[i] = Point.new(i, i) end
local norms = {}
for i, one in ipairs(ps) do norms[i] = one:norm2() end
print(table.concat(norms, " "))
