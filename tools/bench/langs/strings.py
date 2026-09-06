def run(n):
    s = ""
    i = 0
    while i < n:
        s = s + "x"
        i = i + 1
    return len(s)

print(run(40000))
