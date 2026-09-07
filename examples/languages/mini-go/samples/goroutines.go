package main

import "fmt"

// Goroutines and unbuffered channels -- vmlib's Coroutines + scheduler
// recipe, checked against go run. Every print here is ordered by a channel
// handshake, so the output is the same under any scheduler: Go's, which
// may run goroutines in parallel, and vmlib's, which runs them one at a
// time on one thread.

// worker prints what it receives on in (times ten) and answers on out,
// until it is sent 0; then it answers -1 and returns.
func worker(in chan int32, out chan int32) {
	var v int32 = <-in
	for v != 0 {
		fmt.Println(v * 10)
		out <- v + 1
		v = <-in
	}
	out <- -1
}

// producer sends 1..n on ch. Each send waits for main's receive, so main
// sees them in order.
func producer(ch chan int32, n int32) {
	var i int32 = 1
	for i <= n {
		ch <- i
		i = i + 1
	}
}

// ping and pong pass a counter back and forth, each printing what it
// got, until it reaches the limit; then pong tells main it is done.
func ping(toPong chan int64, fromPong chan int64, limit int64) {
	var n int64 = <-fromPong
	for n < limit {
		fmt.Println(n)
		toPong <- n + 1
		n = <-fromPong
	}
	toPong <- n
}

func pong(fromPing chan int64, toPing chan int64, limit int64, done chan int64) {
	toPing <- 0
	var n int64 = <-fromPing
	for n < limit {
		fmt.Println(n)
		toPing <- n + 1
		n = <-fromPing
	}
	done <- n
}

func main() {
	var in chan int32 = make(chan int32)
	var out chan int32 = make(chan int32)
	go worker(in, out)
	in <- 1
	fmt.Println(<-out)
	in <- 5
	fmt.Println(<-out)
	in <- 0
	fmt.Println(<-out)

	var ch chan int32 = make(chan int32)
	go producer(ch, 3)
	fmt.Println(<-ch)
	fmt.Println(<-ch)
	fmt.Println(<-ch)

	var a chan int64 = make(chan int64)
	var b chan int64 = make(chan int64)
	var done chan int64 = make(chan int64)
	go ping(a, b, 6)
	go pong(a, b, 6, done)
	fmt.Println(<-done)

	// A goroutine spawned in a loop gets its own copy of the loop's value
	// (the argument is evaluated at the go statement), and each one hands
	// it back through the same channel, in the order they are received.
	var results chan int32 = make(chan int32)
	var k int32 = 0
	for k < 3 {
		go send(results, k*k)
		k = k + 1
	}
	var got int32 = 0
	var count int32 = 0
	for count < 3 {
		got = got + <-results
		count = count + 1
	}
	fmt.Println(got)

	if got > 4 {
		fmt.Println(1)
	} else {
		fmt.Println(0)
	}
}

func send(ch chan int32, v int32) {
	ch <- v
}
