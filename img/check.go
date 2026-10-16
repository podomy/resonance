// Command probe-check is the exec-probe half of the resonance
// test image. Bare argv exits 0 (healthy); "fail" exits 1
// (sick), so one image covers both health outcomes.
package main

import (
	"fmt"
	"os"
)

func main() {
	if len(os.Args) > 1 && os.Args[1] == "fail" {
		_, _ = fmt.Fprintln(os.Stderr, "probe sick")
		os.Exit(1)
	}
	_, _ = fmt.Fprintln(os.Stdout, "probe healthy")
}
