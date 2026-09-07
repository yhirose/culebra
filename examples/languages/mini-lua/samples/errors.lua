-- error and pcall. `pcall` is a TryCatch -- which is what it is -- and it
-- answers `true` plus the call's results, or `false` and whatever was
-- raised.

local function boom() error("something broke") end
print(pcall(boom))
print(pcall(function() return "fine", 2 end))

-- What is raised is a value, not a class: a table works as well as a
-- string, and comes back unchanged.
-- A table raised as an error comes back unchanged; only its identity is
-- unprintable (Lua shows an address), so this asks about its contents.
local ok, e = pcall(function() error({code = 42}) end)
print(ok, type(e), e.code)

-- A throw crosses as many frames as it has to.
local function deep(n)
  if n == 0 then error("bottom") end
  return 1 + deep(n - 1)
end
print(pcall(deep, 5))

-- Nested pcall: the inner one catches, so the outer sees success.
print(pcall(function()
  local inner_ok, inner_err = pcall(function() error("inner") end)
  return "outer saw " .. tostring(inner_ok) .. " " .. tostring(inner_err)
end))

-- Re-raising from a handler reaches the next one out.
print(pcall(function()
  local ok2, err2 = pcall(function() error("first") end)
  if not ok2 then error("re: " .. err2) end
end))

-- pcall passes its extra arguments to the function it calls.
print(pcall(function(a, b) return a + b end, 3, 4))

-- A runtime failure the executor raises itself is caught the same way a
-- program's own error is -- TryCatch lands both.
local ok3 = pcall(function() return nil + 1 end)
print(ok3)

-- Failure inside a coroutine surfaces at the resume rather than ending
-- the program.
local co = coroutine.create(function() error("in coroutine") end)
print(pcall(function() return coroutine.resume(co) end))
