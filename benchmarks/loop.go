package main

import (
	"fmt"
	"time"
)

func main() {
	start := time.Now()
	sum := 0
	for i := 0; i < 100000000; i++ {
		sum += i
	}
	elapsed := time.Since(start)
	fmt.Printf("result: %d\n", sum)
	fmt.Printf("time: %v\n", elapsed)
}
