// Command hog holds real resident memory up to its scheduled cap so
// scheduler preemption tests observe genuine pressure. It reads the
// CONCORD_MEM_LIMIT_MB env the daemon injects from the workload
// request (0 or unset means 256), allocates that many megabytes,
// faults every page in, then serves /health like probe and logs while
// holding the pages for life. Static build like probe: the test image
// has no loader or libs.
package main

import (
	"fmt"
	"net/http"
	"os"
	"strconv"
	"time"
)

var hold [][]byte

func main() {
	mb := 256
	if s := os.Getenv("CONCORD_MEM_LIMIT_MB"); s != "" {
		if v, err := strconv.Atoi(s); err == nil && v > 0 && v < 1<<20 {
			mb = v
		}
	}
	const chunk = 1 << 20
	hold = make([][]byte, 0, mb)
	for i := 0; i < mb; i++ {
		b := make([]byte, chunk)
		for o := 0; o < len(b); o += 4096 {
			b[o] = 1
		}
		hold = append(hold, b)
	}
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		_, _ = fmt.Fprintln(w, "ok")
	})
	go func() {
		for {
			_, _ = fmt.Println("hog alive")
			time.Sleep(time.Second)
		}
	}()
	_ = http.ListenAndServe("0.0.0.0:"+port, nil)
}
