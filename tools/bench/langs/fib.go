package main

import (
	"fmt"
	"os"
	"strconv"
)

func fib(n int64) int64 {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	fmt.Println(fib(size()))
}

func size() int64 {
	n, _ := strconv.ParseInt(os.Args[1], 10, 64)
	return n
}
