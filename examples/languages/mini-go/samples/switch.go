package main

import "fmt"

// A dense set of int32 keys (1, 2, 3), plus a default -- and case 1, 2:
// shares one body between two keys, README's "Go's case a, b:" recipe.
func classify(n int32) int32 {
	switch n {
	case 1, 2:
		return 100
	case 3:
		return 300
	default:
		return -1
	}
}

func main() {
	fmt.Println(classify(1))
	fmt.Println(classify(2))
	fmt.Println(classify(3))
	fmt.Println(classify(99))

	var n int64 = 7
	switch n {
	case 7:
		fmt.Println(700)
	default:
		fmt.Println(-1)
	}

	// u is always non-negative in its normalized representation, so a key
	// this large compares correctly without any extra wrapping.
	var u uint32 = 4000000000
	switch u {
	case 4000000000:
		fmt.Println(1)
	default:
		fmt.Println(0)
	}

	// No default, no match: the switch itself is a statement here (its
	// value, nil, is unobserved either way), the same as an If with no
	// else taken. An empty case body is legal too.
	var m int32 = 42
	switch m {
	case 1:
	}
	fmt.Println(m)
}
