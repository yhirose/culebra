def run(n):
    total = 0
    i = 1
    while i <= n:
        total = (total + i * i) % 1000000007
        i = i + 1
    return total

print(run(1200000))
