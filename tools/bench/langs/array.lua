local function run(n)
  local xs = {}
  local i = 1
  while i <= n do
    xs[i] = i * i
    i = i + 1
  end
  local total = 0
  local j = 1
  while j <= #xs do
    total = (total + xs[j]) % 1000000007
    j = j + 1
  end
  return total
end

print(run(tonumber(arg[1])))
