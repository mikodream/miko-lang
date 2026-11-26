package main

import "fmt"

func mix(n int64) int64 {
	var acc int64 = 1
	var i int64 = 1

	for i <= n {
		acc = acc + (i*3)%97
		if i%2 == 0 {
			acc = acc - i/3
		} else {
			acc = acc + i/5
		}
		i = i + 1
	}

	return acc
}

func main() {
	var total int64 = 0
	var round int64 = 0

	for round < 8 {
		total = total + mix(20000000)
		round = round + 1
	}

	fmt.Println(total)
}
