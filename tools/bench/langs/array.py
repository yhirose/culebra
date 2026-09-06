def run(n):
    xs = []
    i = 1
    while i <= n:
        xs.append(i * i)
        i = i + 1
    total = 0
    j = 0
    while j < len(xs):
        total = (total + xs[j]) % 1000000007
        j = j + 1
    return total

print(run(500000))
