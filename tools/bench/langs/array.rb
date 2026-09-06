def run(n)
  xs = []
  i = 1
  while i <= n
    xs.push(i * i)
    i = i + 1
  end
  total = 0
  j = 0
  while j < xs.length
    total = (total + xs[j]) % 1000000007
    j = j + 1
  end
  total
end

puts run(500000)
