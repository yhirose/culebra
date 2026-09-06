local function run(n)
  local s = ""
  local i = 0
  while i < n do
    s = s .. "x"
    i = i + 1
  end
  return #s
end

print(run(40000))
