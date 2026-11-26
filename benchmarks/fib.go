package main

import (
	"fmt"
	"time"
)

func fib(n int) int {
	if n <= 1 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	start := time.Now()
	result := fib(30)
	elapsed := time.Since(start)
	fmt.Printf("result: %d\n", result)
	fmt.Printf("time: %v\n", elapsed)
}
