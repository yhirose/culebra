-- The headline. Lua guarantees that `return f(...)` reuses the caller's
-- frame instead of stacking on it, which makes `lua` a real oracle for
-- Func::tail_calls rather than an implementation that happens to agree.
--
-- Every number below is past RunOptions::max_call_depth (10000), so this
-- sample does not merely exercise the feature: it *fails* without it, with
-- "recursion limit exceeded".

local function sum(n, acc)
  if n == 0 then return acc end
  return sum(n - 1, acc + n)
end
print(sum(200000, 0))

-- Mutual tail recursion: the frame is reused across two functions, so
-- neither stack grows.
local is_even, is_odd
function is_even(n) if n == 0 then return true end return is_odd(n - 1) end
function is_odd(n) if n == 0 then return false end return is_even(n - 1) end
print(is_even(100000), is_odd(100001))

-- A tail call whose callee returns several values passes them all through,
-- because what it hands back is what the callee handed back.
local function pair() return "a", "b" end
local function forward() return pair() end
print(forward())

-- A state machine as a call chain: each state tail-calls the next, so a
-- run of any length costs one frame.
local step
local function done(n) return "done after " .. n end
local function tick(n)
  if n <= 0 then return done(0) end
  return step(n - 1)
end
function step(n)
  if n <= 0 then return tick(0) end
  return tick(n - 1)
end
print(tick(150000))

-- A tail call through a value, not a name: the callee is whatever the
-- variable holds at the time.
local function loop(f, n) if n == 0 then return "ok" end return f(f, n - 1) end
print(loop(loop, 120000))

-- Not every return is a tail call, and that is the point of the
-- distinction: this one has work left to do after the call comes back, so
-- it stacks -- and stays well inside the limit on purpose.
local function depth(n) if n == 0 then return 0 end return 1 + depth(n - 1) end
print(depth(5000))
