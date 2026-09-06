def run(n)
  s = ""
  i = 0
  while i < n
    s = s + "x"
    i = i + 1
  end
  s.length
end

puts run(40000)
