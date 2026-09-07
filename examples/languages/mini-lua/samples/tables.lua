-- Tables. A Lua table is one container with an array part and a hash part,
-- keyed by any value -- which is what IntrinsicId::MapNew's value-keyed Map
-- is, so a table here is a Map and nothing else.

local t = {10, 20, 30, name = "row", [100] = "far"}
print(t[1], t[3], t.name, t[100], t[4])
print(#t)

t[4] = 40
print(#t, t[4])
t[2] = nil
-- Assigning nil removes the key. `#` on a table with a hole is explicitly
-- *any* border in Lua, so the two implementations may disagree about which
-- one they pick and this only asks what is there.
print(t[2], t[1], t[3])

local a = {}
for i = 1, 5 do a[i] = i * i end
print(#a, table.concat(a, ","))
table.insert(a, 36)
print(#a, a[6])

-- Keys are values: 1 and 1.0 are the same slot, and a string key is not
-- the same as the number that prints like it.
local k = {}
k[1] = "int"
k[1.0] = "float overwrites it"
k["1"] = "string is its own key"
print(k[1], k["1"])

local sum = 0
for i, v in ipairs({4, 5, 6}) do sum = sum + i * v end
print(sum)

local names = {}
for i, v in ipairs({"a", "b", "c"}) do names[#names + 1] = i .. v end
print(table.concat(names, " "))

-- Nested tables, and a table as a value inside another.
local m = {row = {1, 2}, meta = {deep = {"x"}}}
print(m.row[2], m.meta.deep[1], m.missing)
m.row[3] = 3
print(#m.row, table.concat(m.row, "-"))

-- pairs over a table with one key, so the order this front end produces
-- (insertion) and Lua's (unspecified) cannot disagree.
for key, value in pairs({only = 42}) do print(key, value) end

print(type(t), #({}))
