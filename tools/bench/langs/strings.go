package main

import "fmt"

func run(n int64) int64 {
	var s string = ""
	var i int64 = 0
	for i < n {
		s = s + "x"
		i = i + 1
	}
	return int64(len(s))
}

func main() {
	fmt.Println(run(40000))
}
