// Report rendering coverage: golden overlays, histograms, ladders,
// and Prometheus export all render from one synthetic run log.
// The latency plots must spread over distinct x positions (never
// one vertical pillar) and carry the p50/p99/max overlay trio.
package clitest

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

func reportLog(t *testing.T) string {
	t.Helper()
	path := filepath.Join("testdata", "report.log")
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("missing fixture %s: %v", path, err)
	}
	return path
}

func TestReportGoldenOverlay(t *testing.T) {
	t.Parallel()
	code, stdout, stderr := runRSuite(t, []string{"report", reportLog(t)})
	if code != 0 {
		t.Fatalf("report exit = %d, want 0 (stderr %q)", code, stderr)
	}
	for _, want := range []string{
		"refs: p50=12 p99=48 max=61 milliseconds",
		"refs: p50=11800 p99=48200 max=61300 microseconds",
		"20-50ms: 150",
		"1-5ms: 40",
		"p50.us p99.us max.us scrape",
		"bench.2.1.ramp.0",
		"ramp ladder:",
		"step rate p50 p99 max",
		"run overview:",
		"step 0",
		"y is seconds",
	} {
		if !strings.Contains(stdout, want) {
			t.Errorf("text report missing %q", want)
		}
	}
}

func distinctCX(t *testing.T, html, key string) int {
	t.Helper()
	seen := map[string]bool{}
	lines := strings.Split(html, "key: "+key)
	if len(lines) < 2 {
		t.Fatalf("plot for %s not found", key)
	}
	// Bound the scan to this plot's svg: cut at the next key line.
	seg := lines[1]
	if i := strings.Index(seg, "key: "); i >= 0 {
		seg = seg[:i]
	}
	for _, m := range regexp.MustCompile(`cx="([0-9.]+)"`).FindAllStringSubmatch(seg, -1) {
		seen[m[1]] = true
	}
	return len(seen)
}

func TestReportHTMLSpread(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	out := filepath.Join(dir, "r.html")
	code, _, stderr := runRSuite(t, []string{"report", "--html", out, reportLog(t)})
	if code != 0 {
		t.Fatalf("report --html exit = %d, want 0 (stderr %q)", code, stderr)
	}
	raw, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read html: %v", err)
	}
	html := string(raw)
	for _, color := range []string{"#0e7490", "#b45309", "#b91c2b"} {
		if !strings.Contains(html, `stroke="`+color+`"`) {
			t.Errorf("html missing golden overlay color %s", color)
		}
	}
	if got := distinctCX(t, html, "bench.2.0.lat_us"); got < 5 {
		t.Errorf("lat_us plot has %d distinct x positions, want >= 5 (pillar)", got)
	}
	if got := distinctCX(t, html, "bench.2.0.lat"); got < 2 {
		t.Errorf("lat plot has %d distinct x positions, want >= 2", got)
	}
	seg := strings.Split(html, "key: res.node0.rss.bytes")[1]
	if i := strings.Index(seg, "key: "); i >= 0 {
		seg = seg[:i]
	}
	if got := len(regexp.MustCompile(`r="1\.0"`).FindAllString(seg, -1)); got < 200 {
		t.Errorf("dense series keeps %d small dots, want >= 200 (blob check)", got)
	}
	if strings.Contains(seg, `r="2.5"`) {
		t.Errorf("dense series still uses large dots")
	}
	if got := distinctCX(t, html, "res.node9.rss.bytes"); got < 2 {
		t.Errorf("sparse series collapsed to %d x position(s)", got)
	} else {
		seg := strings.Split(html, "key: res.node9.rss.bytes")[1]
		if i := strings.Index(seg, "key: "); i >= 0 {
			seg = seg[:i]
		}
		var lo, hi float64
		first := true
		for _, m := range regexp.MustCompile(`cx="([0-9.]+)"`).FindAllStringSubmatch(seg, -1) {
			var v float64
			if _, err := fmt.Sscanf(m[1], "%g", &v); err != nil {
				t.Fatalf("bad cx %q", m[1])
			}
			if first || v < lo {
				lo = v
			}
			if first || v > hi {
				hi = v
			}
			first = false
		}
		if hi-lo < 400 {
			t.Errorf("sparse series spans %.0fpx, want full-width (>400px)", hi-lo)
		}
	}
	if !strings.Contains(html, "ramp ladder") {
		t.Errorf("html missing ramp ladder section")
	}
	if !strings.Contains(html, "step 0 overview") {
		t.Errorf("html missing run overview section")
	}
}

func TestReportProm(t *testing.T) {
	t.Parallel()
	code, stdout, stderr := runRSuite(t, []string{"report", "--format", "prom", reportLog(t)})
	if code != 0 {
		t.Fatalf("report --format prom exit = %d, want 0 (stderr %q)", code, stderr)
	}
	for _, want := range []string{
		"# HELP resonance_metric",
		"# TYPE resonance_metric gauge",
		`resonance_metric{key="bench.2.0.p50.us",phase="2"} 11800`,
		`resonance_metric{key="bench.2.0.scrape_ok",phase="2"} 1`,
		`resonance_metric{key="bench.2.0.hist_us.4",phase="2"} 90`,
		`resonance_metric{key="bench.2.0.lat_us",phase="2"}`,
	} {
		if !strings.Contains(stdout, want) {
			t.Errorf("prom exposition missing %q", want)
		}
	}
	code, _, _ = runRSuite(t, []string{"report", "--format", "bogus", reportLog(t)})
	if code != 2 {
		t.Errorf("bad --format exit = %d, want 2", code)
	}
}

func TestReportDir(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	raw, err := os.ReadFile(reportLog(t))
	if err != nil {
		t.Fatalf("read fixture: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, "a.log"), raw, 0600); err != nil {
		t.Fatalf("write fixture copy: %v", err)
	}
	code, stdout, _ := runRSuite(t, []string{"report", dir})
	if code != 0 {
		t.Fatalf("report dir exit = %d", code)
	}
	if !strings.Contains(stdout, "verdict: pass synth") {
		t.Errorf("dir report missing verdict")
	}
	code, stdout, _ = runRSuite(t, []string{"report", "--format", "prom", dir})
	if code != 0 {
		t.Fatalf("report --format prom dir exit = %d", code)
	}
	if !strings.Contains(stdout, `log="a.log"`) {
		t.Errorf("dir prom exposition missing log label")
	}
	out := filepath.Join(dir, "r.html")
	code, _, stderr := runRSuite(t, []string{"report", "--html", out, dir})
	if code != 0 {
		t.Fatalf("report --html dir exit = %d, stderr %q", code, stderr)
	}
	html, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read html: %v", err)
	}
	if !strings.Contains(string(html), "a.log") {
		t.Errorf("dir html missing log label")
	}
}
