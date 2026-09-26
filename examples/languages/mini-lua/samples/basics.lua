-- Numbers, operators, and the three rules Lua does not share with the VM's
-- defaults: what is true, how `%` and `//` round, and what `==` does
-- across two types.

print(1 + 2 * 3, 7 - 2, 3 * 4)
-- `/` is always a float, `//` floors, and `%` follows `//`. BinOp::Div on
-- two ints truncates and BinOp::Mod is C's, so both of these are lowered
-- through a func this front end writes.
print(7 / 2, 7 // 2, -7 // 2, 7 % 3, -7 % 3, 7 % -3)
print(2 ^ 10, 2 ^ 0.5)
print(math.type(1), math.type(1.0), math.type(7 // 2), math.type(7 / 2))

print("a" .. "b", "n=" .. 1, 1 .. 2)
print(#"hello", #"")

-- Only nil and false are false. Value::truthy() calls 0 false, and its
-- comment says why it will not decide: "Lua calls neither" falsy.
local function t(v) if v then return "T" else return "F" end end
print(t(0), t(""), t({}), t(nil), t(false), t(1))

-- `==` across two types is false, where BinOp::Eq refuses the comparison.
print(1 == 1.0, 1 == "1", "a" == "a", nil == false, {} == {})
print(1 ~= 2, 1 < 2, 2 <= 2, "a" < "b")

-- and/or answer one of their operands, not a boolean.
print(1 and 2, nil and 2, false or "x", nil or "y", 0 or "z")
print(not nil, not 0, not "")

print(tostring(1), tostring(1.0), tostring(10 / 2), tostring(0.1 + 0.2))
print(type(1), type("s"), type({}), type(print), type(nil))

local n, i = 0, 0
while i < 5 do i = i + 1; n = n + i end
print(n, i)

local r = 0
repeat r = r + 1 until r == 3
print(r)

print(string.upper("abc"), ("xy"):rep(3), ("hello"):sub(2, 3), ("hello"):sub(-3))
-- sub's end may be left off or negative, and both ends clamp to the string;
-- rep takes an optional separator.
print(("hello"):sub(2, -2), ("hello"):sub(0), ("hello"):sub(-10, 2), ("hello"):sub(3, 10))
print("[" .. ("hello"):sub(4, 2) .. "]", ("ab"):rep(3, ","), ("ab"):rep(0, ","))

-- An if chain is its condition and block, any elseif arms, then the else.
local function sign(v)
  if v > 0 then return "+" elseif v < 0 then return "-" else return "0" end
end
local function size(v)
  if v < 10 then return "small" elseif v < 100 then return "medium"
  elseif v < 1000 then return "large" end
  return "huge"
end
print(sign(5), sign(-5), sign(0), size(1), size(50), size(500), size(5000))

-- A lone string or table constructor may stand for the argument list.
print "one\targ"
local function cat(a) return function(b) return a .. b end end
local o = {tag = "o"}
function o:wrap(s) return self.tag .. "<" .. s .. ">" end
function o:count(t) return #t end
local function first(t) return t[1] end
print(first{"f"}, o:count{1, 2, 3}, cat"x""y", o:wrap"s")
print(pcall(function() error "boom" end))
