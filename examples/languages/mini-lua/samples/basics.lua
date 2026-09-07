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
