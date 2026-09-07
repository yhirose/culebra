package main

import "fmt"

// Point exercises the plain case: two scalar fields, read right after
// construction (FieldGet) and again after one is reassigned (FieldSet).
type Point struct {
	X int32
	Y int32
}

// Line nests one struct field inside another -- building it goes through
// ObjectLit twice (once per Point, once for Line), and reading From.X
// takes a FieldGet of what another FieldGet just produced.
type Line struct {
	From Point
	To   Point
}

func main() {
	var p Point = Point{X: 3, Y: 4}
	fmt.Println(p.X)
	fmt.Println(p.Y)

	p.X = p.X + 10
	p.Y = p.Y * 2
	fmt.Println(p.X)
	fmt.Println(p.Y)

	var origin Point = Point{X: 0, Y: 0}
	var l Line = Line{From: origin, To: p}
	fmt.Println(l.From.X)
	fmt.Println(l.To.X)
	fmt.Println(l.To.Y)

	var dx int32 = l.To.X - l.From.X
	var dy int32 = l.To.Y - l.From.Y
	fmt.Println(dx)
	fmt.Println(dy)

	// Structs are values: writing through l.To must not alias p (the
	// struct literal that built it) or move through origin either.
	l.To.X = 999
	fmt.Println(l.To.X)
	fmt.Println(p.X)

	var q Point = p
	q.Y = -1
	fmt.Println(p.Y)
	fmt.Println(q.Y)
}
