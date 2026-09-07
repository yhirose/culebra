package main

import "fmt"

func main() {
	var f1 float32 = 3.5
	var f2 float32 = 2.0
	fmt.Println(f1 * f2)
	fmt.Println(f1 / f2)

	var g1 float32 = 1.0
	var g2 float32 = 3.0
	var g3 float32 = g1 / g2
	fmt.Println(float64(g3) != 1.0/3.0)

	var h float64 = 1.0 / 3.0
	fmt.Println(h != float64(g3))
}
