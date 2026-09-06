package main

import "fmt"

func run(n int64) int64 {
	var total int64 = 0
	var i int64 = 1
	for i <= n {
		total = (total + i*i) % 1000000007
		i = i + 1
	}
	return total
}

func main() {
	fmt.Println(run(1200000))
}
