local function run(n)
  local total = 0
  local i = 1
  while i <= n do
    total = (total + i * i) % 1000000007
    i = i + 1
  end
  return total
end

print(run(1200000))
