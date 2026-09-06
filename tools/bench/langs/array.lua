local function run(n)
  local xs = {}
  local i = 1
  while i <= n do
    xs[i] = i * i
    i = i + 1
  end
  local total = 0
  local j = 1
  local len = #xs
  while j <= len do
    total = (total + xs[j]) % 1000000007
    j = j + 1
  end
  return total
end

print(run(500000))
