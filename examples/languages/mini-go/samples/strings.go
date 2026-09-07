package main

import "fmt"

// A string parameter and a string return: strings are immutable values in
// Go, so nothing here needs the explicit copy a struct crossing into new
// storage does.
func repeat(s string, n int64) string {
	var out string = ""
	var i int64 = 0
	for i < n {
		out = out + s
		i = i + 1
	}
	return out
}

func main() {
	var s string = repeat("ab", 3)
	fmt.Println(s)
	// len is bytes in Go, which is exactly what the Len intrinsic answers.
	// It comes back as int64 here rather than Go's own `int`, a type this
	// front end does not have -- so the conversion real Go needs anyway is
	// what both accept.
	fmt.Println(int64(len(s)))
	fmt.Println(int64(len("")))

	// All six operators: Go orders strings lexicographically, and so does
	// the executor's own compare.
	fmt.Println(s == "ababab")
	fmt.Println(s != "ababab")
	fmt.Println(s < "b")
	fmt.Println(s >= "ababab")

	// The four escapes the grammar accepts; any other one is a diagnostic
	// rather than a character quietly meaning something else than it does
	// under `go run`.
	fmt.Println("tab:\tnewline-next:\nquote:\"backslash:\\")
}
