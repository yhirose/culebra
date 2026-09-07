package main

import "fmt"

// A slice built the only way this front end accepts -- `xs = append(xs, v)`,
// the shape where ArrayPush's grow-in-place and Go's own may-or-may-not-
// reallocate append cannot disagree. See binder.cc's emit_append.
func squares(n int64) []int64 {
	var xs []int64 = []int64{}
	var i int64 = 1
	for i <= n {
		xs = append(xs, i*i)
		i = i + 1
	}
	return xs
}

func main() {
	var xs []int64 = squares(5)
	fmt.Println(int64(len(xs)))
	fmt.Println(xs[0])
	fmt.Println(xs[4])

	// A write through the subscript, then a read of it: a slice is a
	// reference in Go, and the ArrayObj behind it is one here, so the
	// write is visible through the same name it went in by -- no
	// copy_struct-style question, because an element is always a scalar.
	xs[2] = -1
	fmt.Println(xs[2])

	// A literal with elements, at a narrower width.
	var seeded []int32 = []int32{3, 1, 4}
	fmt.Println(seeded[1] + seeded[2])
	fmt.Println(int64(len(seeded)))

	// Two names for one slice, and a write through the second visible
	// through the first: Go's own aliasing, and the reason none of this
	// needs the explicit copy the struct samples do. (`var l Line = ...`
	// in samples/structs is the opposite case -- there the copy is what
	// makes this front end agree with `go run`.)
	var alias []int32 = seeded
	alias[0] = alias[0] + 100
	fmt.Println(seeded[0])
}
