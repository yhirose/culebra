package main

import "fmt"

func run(n int64) int64 {
	var xs []int64 = []int64{}
	var i int64 = 1
	for i <= n {
		xs = append(xs, i*i)
		i = i + 1
	}
	var total int64 = 0
	var j int64 = 0
	for j < int64(len(xs)) {
		total = (total + xs[j]) % 1000000007
		j = j + 1
	}
	return total
}

func main() {
	fmt.Println(run(500000))
}
