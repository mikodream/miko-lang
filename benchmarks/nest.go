package main

import (
	"fmt"
	"time"
)

func main() {
	start := time.Now()
	count := 0
	for i := 0; i < 1000; i++ {
		for j := 0; j < 1000; j++ {
			if i*j%7 == 0 {
				count++
			}
		}
	}
	elapsed := time.Since(start)
	fmt.Printf("result: %d\n", count)
	fmt.Printf("time: %v\n", elapsed)
}
