// Command probe serves /health and logs one line per second.
// It is the container half of the resonance test image:
// health probes hit /health, log streaming reads stdout,
// and the exec probe runs /probe-check from the same image.
package main

import (
	"fmt"
	"net/http"
	"os"
	"time"
)

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		_, _ = fmt.Fprintln(w, "ok")
	})
	go func() {
		for {
			_, _ = fmt.Println("probe alive")
			time.Sleep(time.Second)
		}
	}()
	_ = http.ListenAndServe("0.0.0.0:"+port, nil)
}
