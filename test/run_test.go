// Run error-path and skip-path coverage. All cases work without
// privileges: missing files, usage errors, TAP/JSON byte shapes on
// error paths, and a missing-binary skip.
package clitest

import (
	"path/filepath"
	"strings"
	"testing"
)

func TestRunMissingFile(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		args func(missing string) []string
	}{
		{"plain", func(missing string) []string { return []string{"run", missing} }},
		{"jobs2", func(missing string) []string { return []string{"run", "--jobs", "2", missing} }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			missing := filepath.Join(t.TempDir(), "does-not-exist.yaml")
			code, stdout, _ := runRSuite(t, tc.args(missing))
			if code != 1 {
				t.Fatalf("run missing file exit = %d, want 1 (stdout %q)", code, stdout)
			}
			if !strings.Contains(stdout, "not ok 1 ") {
				t.Errorf("stdout missing TAP 'not ok 1 ', got %q", stdout)
			}
			if !strings.Contains(stdout, "# pass=0 fail=1 skip=0") {
				t.Errorf("stdout missing TAP summary, got %q", stdout)
			}
		})
	}
}

func TestRunTapJsonConflict(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		args []string
	}{
		{"tap-then-json", []string{"run", "--tap", "--json", "../suite/examples/agree.yaml"}},
		{"json-then-tap", []string{"run", "--json", "--tap", "../suite/examples/agree.yaml"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			code, _, stderr := runRSuite(t, tc.args)
			if code != 2 {
				t.Fatalf("exit = %d, want 2 (stderr %q)", code, stderr)
			}
			if !strings.Contains(stderr, "--tap and --json conflict") {
				t.Errorf("stderr missing conflict note, got %q", stderr)
			}
			if !strings.Contains(stderr, "usage:") {
				t.Errorf("stderr missing usage, got %q", stderr)
			}
		})
	}
}

func TestRunBadJobs(t *testing.T) {
	t.Parallel()
	const file = "../suite/examples/agree.yaml"
	cases := []struct {
		name string
		args []string
		want string
	}{
		{"zero-space", []string{"run", "--jobs", "0", file}, "bad --jobs value"},
		{"negative", []string{"run", "--jobs", "-1", file}, "bad --jobs value"},
		{"alpha", []string{"run", "--jobs", "abc", file}, "bad --jobs value"},
		{"float", []string{"run", "--jobs", "1.5", file}, "bad --jobs value"},
		{"zero-eq", []string{"run", "--jobs=0", file}, "bad --jobs value"},
		{"alpha-eq", []string{"run", "--jobs=abc", file}, "bad --jobs value"},
		{"missing-value", []string{"run", "--jobs"}, "needs a value after --jobs"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			code, _, stderr := runRSuite(t, tc.args)
			if code != 2 {
				t.Fatalf("exit = %d, want 2 (stderr %q)", code, stderr)
			}
			if !strings.Contains(stderr, tc.want) {
				t.Errorf("stderr missing %q, got %q", tc.want, stderr)
			}
			if !strings.Contains(stderr, "usage:") {
				t.Errorf("stderr missing usage, got %q", stderr)
			}
		})
	}
}

func TestRunTapJsonShapes(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name      string
		flag      string
		checkTAP  bool
		checkJSON bool
	}{
		{"tap", "--tap", true, false},
		{"json", "--json", false, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			missing := filepath.Join(t.TempDir(), "does-not-exist.yaml")
			code, stdout, _ := runRSuite(t, []string{"run", tc.flag, missing})
			if code != 1 {
				t.Fatalf("run %s missing exit = %d, want 1 (stdout %q)", tc.flag, code, stdout)
			}
			lines := strings.Split(strings.TrimSuffix(stdout, "\n"), "\n")
			if tc.checkTAP {
				if len(lines) < 2 {
					t.Fatalf("TAP lines = %d, want >= 2, got %q", len(lines), stdout)
				}
				if !strings.HasPrefix(lines[0], "not ok 1 ") {
					t.Errorf("TAP first line prefix = %q, want 'not ok 1 ', full %q", lines[0], stdout)
				}
				if !strings.Contains(stdout, "# pass=0 fail=1 skip=0") {
					t.Errorf("TAP missing summary, got %q", stdout)
				}
			}
			if tc.checkJSON {
				if len(lines) < 2 {
					t.Fatalf("JSON lines = %d, want >= 2, got %q", len(lines), stdout)
				}
				if !strings.HasPrefix(lines[0], "{\"file\":") {
					t.Errorf("JSON first line prefix = %q, want '{\"file\":', full %q", lines[0], stdout)
				}
				if !strings.Contains(lines[0], "\"verdict\":\"fail\"") {
					t.Errorf("JSON missing fail verdict, got %q", lines[0])
				}
				if !strings.HasPrefix(lines[1], "{\"summary\":") {
					t.Errorf("JSON second line prefix = %q, want '{\"summary\":', full %q", lines[1], stdout)
				}
			}
		})
	}
}

func TestRunSkipMissingBinary(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		args []string
		json bool
	}{
		{"tap", []string{"run", "--tap", "../suite/examples/weld_roundtrip.yaml"}, false},
		{"json", []string{"run", "--json", "../suite/examples/weld_roundtrip.yaml"}, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			code, stdout, stderr := runRSuite(t, tc.args)
			if code != 0 {
				t.Fatalf("run skip exit = %d, want 0 (stdout %q stderr %q)", code, stdout, stderr)
			}
			if !tc.json {
				if !strings.Contains(stdout, "# SKIP") {
					t.Errorf("stdout missing '# SKIP', got %q", stdout)
				}
				if !strings.Contains(stdout, "skip=1") {
					t.Errorf("stdout missing 'skip=1', got %q", stdout)
				}
				if !strings.Contains(stdout, "skip no ") {
					t.Errorf("stdout missing 'skip no ', got %q", stdout)
				}
			} else {
				if !strings.HasPrefix(strings.TrimSpace(stdout), "{\"file\":") {
					t.Errorf("JSON missing file prefix, got %q", stdout)
				}
				if !strings.Contains(stdout, "\"verdict\":\"skip\"") {
					t.Errorf("stdout missing skip verdict, got %q", stdout)
				}
				if !strings.Contains(stdout, "\"skip\":1") {
					t.Errorf("stdout missing '\"skip\":1', got %q", stdout)
				}
			}
		})
	}
}
