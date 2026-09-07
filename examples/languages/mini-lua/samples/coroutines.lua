-- Coroutines, in the shape vmlib.h named them for: "What Lua's coroutines,
-- Ruby's Fibers and a goroutine are made of." mini-go drives them from a
-- scheduler and mini-js from async/await, so both use them from
-- underneath; Lua hands them to the program.

local co = coroutine.create(function(a)
  local b = coroutine.yield(a + 1)
  local c = coroutine.yield(b * 2)
  return c .. "!", "and more"
end)

print(coroutine.status(co))
print(coroutine.resume(co, 10))
print(coroutine.status(co))
print(coroutine.resume(co, 5))
print(coroutine.resume(co, "end"))
print(coroutine.status(co))
-- Resuming a finished coroutine answers false and a message rather than
-- raising, which is Lua's rule and not the intrinsic's.
print(coroutine.resume(co))

-- A generator, written as a coroutine. `wrap` is the same thing with the
-- true/false stripped off.
local function range(n)
  return coroutine.wrap(function()
    for i = 1, n do coroutine.yield(i) end
  end)
end
local out = {}
local nextv = range(4)
for i = 1, 4 do out[i] = nextv() end
print(table.concat(out, ","))

-- What a coroutine can do that a generator cannot: yield from inside a
-- call the body made, several frames down. CoroYield parks every frame
-- from the resume to itself, which is exactly what this needs.
local deep = coroutine.create(function()
  local function level3() coroutine.yield("from level 3") end
  local function level2() level3() end
  local function level1() level2() end
  level1()
  return "finished"
end)
print(coroutine.resume(deep))
print(coroutine.resume(deep))

-- Two coroutines interleaved by hand: each keeps its own frames while the
-- other runs.
local function ping(name, n)
  return coroutine.create(function()
    for i = 1, n do coroutine.yield(name .. i) end
    return name .. "done"
  end)
end
local x, y = ping("x", 2), ping("y", 2)
local log = {}
for i = 1, 3 do
  local _, vx = coroutine.resume(x)
  local _, vy = coroutine.resume(y)
  log[#log + 1] = vx .. "/" .. vy
end
print(table.concat(log, " "))

print(type(co), coroutine.status(x), coroutine.status(y))
